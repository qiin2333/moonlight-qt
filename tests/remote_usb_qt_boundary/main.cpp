#include "remoteusb/remote_usb_broker_client.h"
#include "remoteusb/remote_usb_session_binding.h"

#include "backend/identitymanager.h"
#include "backend/nvaddress.h"
#include "backend/nvcomputer.h"

extern "C" {
#include "remote_usb_broker.h"
#include "remote_usb_wire.h"
}

#include <QCoreApplication>
#include <QEventLoop>
#include <QSslConfiguration>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

/* The parser-only smoke does not call the network fetch path.  Keep the
 * executable independent of the full application backend by supplying the
 * handful of backend symbols referenced by that unused path. */
IdentityManager *IdentityManager::get()
{
    return nullptr;
}

QString IdentityManager::getUniqueId()
{
    return {};
}

QSslConfiguration IdentityManager::getSslConfig()
{
    return {};
}

QString NvComputer::getPairname(const QString &)
{
    return {};
}

QString NvAddress::address() const
{
    return {};
}

namespace {

using namespace RemoteUsb;

class FakeChannel final : public RemoteUsbByteChannel
{
public:
    ChannelCapabilities capabilities() const noexcept override
    {
        return { kWireProtocolVersion, true, true };
    }

    void setCallbacks(BytesCallback bytesCallback,
                      ErrorCallback errorCallback,
                      ClosedCallback closedCallback) override
    {
        bytes = std::move(bytesCallback);
        errors = std::move(errorCallback);
        closed = std::move(closedCallback);
    }

    bool start(QString *error) override
    {
        if (started || closing) {
            if (error != nullptr) {
                *error = QStringLiteral("already started");
            }
            return false;
        }
        started = true;
        return true;
    }

    bool send(const QByteArray &wire, QString *error) override
    {
        if (!started || closing) {
            if (error != nullptr) {
                *error = QStringLiteral("channel is closed");
            }
            return false;
        }
        sent.push_back(wire);
        return true;
    }

    void close() noexcept override
    {
        if (closing) {
            return;
        }
        closing = true;
        started = false;
        auto callback = std::move(closed);
        if (callback) {
            callback();
        }
    }

    bool isOpen() const noexcept override
    {
        return started && !closing;
    }

    void feed(const QByteArray &wire)
    {
        if (bytes) {
            bytes(wire);
        }
    }

    BytesCallback bytes;
    ErrorCallback errors;
    ClosedCallback closed;
    QVector<QByteArray> sent;
    bool started = false;
    bool closing = false;
};

class FakePlatform final : public RemoteUsbPlatformAdapter
{
public:
    QVector<DeviceSnapshot> enumerate(QString *) override
    {
        return {};
    }

    bool claim(const DeviceSnapshot &, QString *) override
    {
        return true;
    }

    void release() noexcept override
    {
        ++releaseCount;
    }

    EndpointResolution resolveEndpoint(const TransferRequest &request,
                                       Endpoint *endpointOut,
                                       QString *) const override
    {
        if (endpointOut == nullptr) {
            return EndpointResolution::Rejected;
        }
        *endpointOut = {};
        endpointOut->address = request.endpoint == 0
            ? 0
            : static_cast<quint8>((request.endpoint & 0x0f) |
                                  (request.direction == TransferDirection::In
                                       ? 0x80 : 0));
        endpointOut->maxPacketSize = request.endpoint == 0 ? 0 : 64;
        return EndpointResolution::Found;
    }

    SubmitDisposition submitControl(const TransferRequest &,
                                    TransferCompletionCallback,
                                    QString *) override
    {
        return SubmitDisposition::Rejected;
    }

    SubmitDisposition submitData(const TransferRequest &,
                                 TransferCompletionCallback,
                                 QString *) override
    {
        return SubmitDisposition::Rejected;
    }

    CancelDisposition cancel(const TransferRequest &,
                             CancelCompletionCallback,
                             qint32 *statusOut,
                             QString *) override
    {
        if (statusOut != nullptr) {
            *statusOut = -95;
        }
        return CancelDisposition::Failed;
    }

    int releaseCount = 0;
};

bool require(bool condition, const QString &message, QTextStream &err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

ml_remote_usb_broker_hello testHello()
{
    ml_remote_usb_broker_hello hello {};
    hello.size = sizeof(hello);
    hello.version = ML_REMOTE_USB_BROKER_VERSION;
    for (std::size_t index = 0; index < sizeof(hello.client_uuid); ++index) {
        hello.client_uuid[index] = static_cast<std::uint8_t>('A' + index);
        hello.capability_nonce[index] = static_cast<std::uint8_t>(index + 1);
    }
    hello.stream_generation = 7;
    hello.session_token = 0x1111222233334444ULL;
    hello.attachment_token = 0x5555666677778888ULL;
    hello.lease_token = 0x9999aaaabbbbccccULL;
    hello.max_urb = 64 * 1024;
    hello.max_inflight = 8;
    hello.isochronous = 0;
    return hello;
}

QByteArray makePeerCapabilityFrame(const ml_remote_usb_broker_hello &hello)
{
    std::vector<std::uint8_t> payload(kWireMaxPayload);
    const QByteArray busId = QByteArrayLiteral("1-2");
    const QByteArray descriptors = QByteArrayLiteral("\x12\x01");
    ml_remote_usb_wire_endpoint endpoint {};
    endpoint.address = 0x81;
    endpoint.attributes = 2;
    endpoint.max_packet_size = 64;
    endpoint.interval = 1;

    ml_remote_usb_wire_capability capability {};
    capability.size = sizeof(capability);
    capability.version = ML_REMOTE_USB_WIRE_VERSION;
    capability.lease_token = hello.lease_token;
    capability.attachment_token = hello.attachment_token;
    capability.vendor_id = 0x1234;
    capability.product_id = 0x5678;
    capability.device_bcd = 0x0100;
    capability.bus_id = reinterpret_cast<const std::uint8_t *>(busId.constData());
    capability.bus_id_length = static_cast<std::size_t>(busId.size());
    capability.raw_descriptors = reinterpret_cast<const std::uint8_t *>(
        descriptors.constData());
    capability.raw_descriptor_size = static_cast<std::size_t>(descriptors.size());
    capability.endpoints = &endpoint;
    capability.endpoint_count = 1;

    std::size_t payloadSize = 0;
    if (ml_remote_usb_wire_encode_capability(&capability, payload.data(),
                                             payload.size(), &payloadSize) !=
        ML_REMOTE_USB_WIRE_OK) {
        return {};
    }
    std::vector<std::uint8_t> frame(kWireHeaderSize + payloadSize);
    std::size_t frameSize = 0;
    if (ml_remote_usb_wire_encode_frame(
            frame.data(), frame.size(), ML_REMOTE_USB_WIRE_MESSAGE_CAPABILITY,
            0, payload.data(), payloadSize, hello.session_token, 1,
            &frameSize) != ML_REMOTE_USB_WIRE_OK) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char *>(frame.data()),
                      static_cast<qsizetype>(frameSize));
}

bool testCapabilityParser(QTextStream &err)
{
    bool ok = true;
    ok &= require(RemoteUsbBrokerClient::canonicalIdentity(QStringLiteral("abcdef0123456789")) ==
                      QStringLiteral("ABCDEF0123456789"),
                  QStringLiteral("16-byte identity was not canonicalized"), err);
    const QString hashed = RemoteUsbBrokerClient::canonicalIdentity(
        QStringLiteral("a non-hex identity"));
    ok &= require(hashed.size() == 32, QStringLiteral("hashed identity has wrong size"), err);
    ok &= require(RemoteUsbBrokerClient::wireIdentity(hashed).size() == 16,
                  QStringLiteral("wire identity was not decoded"), err);

    const QByteArray json = QByteArrayLiteral(
        "{\"version\":1,\"endpoint\":{\"host\":\"127.0.0.1\",\"port\":4242},"
        "\"nonce\":\"AQIDBAUGBwgJCgsMDQ4PEA\",\"maxUrb\":65536,"
        "\"maxInflight\":8,\"expiresMs\":15000}");
    BrokerCapability capability;
    QString error;
    ok &= require(RemoteUsbBrokerClient::parseCapability(
                      json, QStringLiteral("127.0.0.1"), &capability, &error),
                  QStringLiteral("valid capability did not parse: %1").arg(error), err);
    ok &= require(capability.valid() && capability.nonce.size() == 16,
                  QStringLiteral("parsed capability is not valid"), err);

    ok &= require(!RemoteUsbBrokerClient::parseCapability(
                      json, QStringLiteral("127.0.0.2"), &capability, &error),
                  QStringLiteral("endpoint host mismatch was accepted"), err);
    QByteArray badNonce = json;
    badNonce.replace(QByteArrayLiteral("AQIDBAUGBwgJCgsMDQ4PEA"),
                     QByteArrayLiteral("bad"));
    ok &= require(!RemoteUsbBrokerClient::parseCapability(
                      badNonce, QStringLiteral("127.0.0.1"), &capability, &error),
                  QStringLiteral("invalid nonce was accepted"), err);
    return ok;
}

bool testSessionBinding(QCoreApplication &app, QTextStream &err)
{
    bool ok = true;
    FakePlatform platform;
    FakeChannel channel;
    RemoteUsbSessionBindingOptions options;
    options.brokerHello = testHello();
    RemoteUsbSessionBinding binding(&platform, &channel, options);
    int capabilities = 0;
    int stopped = 0;
    QObject::connect(&binding, &RemoteUsbSessionBinding::capabilityReceived,
                     &binding, [&capabilities](DeviceSnapshot snapshot) {
                         if (snapshot.busId == QByteArrayLiteral("1-2") &&
                             snapshot.endpoints.size() == 1) {
                             ++capabilities;
                         }
                     });
    QObject::connect(&binding, &RemoteUsbSessionBinding::stopped,
                     &binding, [&stopped]() { ++stopped; });

    QString error;
    ok &= require(binding.start(&error),
                  QStringLiteral("session binding did not start: %1").arg(error), err);
    ok &= require(channel.sent.size() == 1 &&
                      channel.sent.first().size() == static_cast<qsizetype>(kBrokerHelloSize),
                  QStringLiteral("HELLO was not emitted as the first 84 bytes"), err);

    std::array<std::uint8_t, ML_REMOTE_USB_BROKER_HELLO_SIZE> helloWire {};
    ok &= require(ml_remote_usb_broker_encode_hello(&options.brokerHello,
                                                     helloWire.data()) ==
                      ML_REMOTE_USB_BROKER_OK,
                  QStringLiteral("could not encode peer HELLO"), err);
    const QByteArray hello(reinterpret_cast<const char *>(helloWire.data()),
                           static_cast<qsizetype>(helloWire.size()));
    channel.feed(hello.left(3));
    channel.feed(hello.mid(3));
    ok &= require(binding.feedBytes({}, &error),
                  QStringLiteral("empty deterministic feed failed: %1").arg(error), err);

    const QByteArray capability = makePeerCapabilityFrame(options.brokerHello);
    ok &= require(!capability.isEmpty(), QStringLiteral("could not build capability frame"), err);
    for (qsizetype offset = 0; offset < capability.size();) {
        const qsizetype size = std::min<qsizetype>(5, capability.size() - offset);
        channel.feed(capability.mid(offset, size));
        offset += size;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ok &= require(capabilities == 1,
                  QStringLiteral("fragmented capability was not delivered"), err);

    binding.stop();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ok &= require(stopped == 1 && platform.releaseCount == 1 && binding.isStopped(),
                  QStringLiteral("stop did not drain channel/core exactly once"), err);
    Q_UNUSED(app);
    return ok;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    bool ok = true;
    ok &= testCapabilityParser(err);
    ok &= testSessionBinding(app, err);
    if (!ok) {
        return 1;
    }
    out << "remote_usb_qt_boundary=passed\n";
    return 0;
}
