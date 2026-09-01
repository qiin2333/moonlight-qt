#include "remoteusb/remote_usb_broker_client.h"
#include "remoteusb/remote_usb_session_binding.h"

#include "backend/identitymanager.h"
#include "backend/nvaddress.h"
#include "backend/nvcomputer.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSslConfiguration>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

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

void putLe16(std::uint8_t *bytes, quint16 value)
{
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8u);
}

void putLe32(std::uint8_t *bytes, quint32 value)
{
    for (unsigned int index = 0; index < 4u; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

void putLe64(std::uint8_t *bytes, quint64 value)
{
    for (unsigned int index = 0; index < 8u; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

RemoteUsbBrokerHello testHello()
{
    RemoteUsbBrokerHello hello;
    for (std::size_t index = 0; index < hello.clientUuid.size(); ++index) {
        hello.clientUuid[index] = static_cast<std::uint8_t>('A' + index);
        hello.capabilityNonce[index] = static_cast<std::uint8_t>(index + 1);
    }
    hello.streamGeneration = 7;
    hello.sessionToken = 0x1111222233334444ULL;
    hello.attachmentToken = 0x5555666677778888ULL;
    hello.leaseToken = 0x9999aaaabbbbccccULL;
    hello.maxPdu = 64 * 1024;
    hello.maxInflight = 8;
    return hello;
}

QByteArray makeHello(const RemoteUsbBrokerHello &hello)
{
    QByteArray wire(static_cast<qsizetype>(kBrokerHelloSize), 0);
    auto *bytes = reinterpret_cast<std::uint8_t *>(wire.data());
    putLe32(bytes, 0x42535552u);
    putLe16(bytes + 4u, 1u);
    putLe16(bytes + 6u, static_cast<quint16>(kBrokerHelloSize));
    std::copy(hello.clientUuid.cbegin(), hello.clientUuid.cend(), bytes + 8u);
    putLe64(bytes + 24u, hello.streamGeneration);
    putLe64(bytes + 32u, hello.sessionToken);
    putLe64(bytes + 40u, hello.attachmentToken);
    putLe64(bytes + 48u, hello.leaseToken);
    std::copy(hello.capabilityNonce.cbegin(), hello.capabilityNonce.cend(),
              bytes + 56u);
    putLe32(bytes + 72u, hello.maxPdu);
    putLe32(bytes + 76u, hello.maxInflight);
    return wire;
}

QByteArray makePeerOpenFrame(const RemoteUsbBrokerHello &hello)
{
    QByteArray wire(static_cast<qsizetype>(kWireHeaderSize + 16u), 0);
    auto *bytes = reinterpret_cast<std::uint8_t *>(wire.data());
    putLe32(bytes, 0x42535552u);
    bytes[4] = 1u;
    bytes[5] = 2u;
    putLe16(bytes + 6u, static_cast<quint16>(kWireHeaderSize));
    putLe32(bytes + 12u, 16u);
    putLe64(bytes + 16u, hello.sessionToken);
    putLe64(bytes + 24u, 1u);
    putLe64(bytes + 32u, hello.leaseToken);
    putLe64(bytes + 40u, hello.attachmentToken);
    return wire;
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
    options.maxReassemblySize = options.brokerHello.maxPdu;
    options.maxFragments = 4096;
    options.maxInflight = options.brokerHello.maxInflight;
    options.maxTransferSize = options.brokerHello.maxPdu - kPduHeaderSize;
    RemoteUsbSessionBinding binding(&platform, &channel, options);
    int helloAccepted = 0;
    int openRequests = 0;
    int openAccepted = 0;
    int stopped = 0;
    QObject::connect(&binding, &RemoteUsbSessionBinding::helloAccepted,
                     &binding, [&helloAccepted]() { ++helloAccepted; });
    QObject::connect(&binding, &RemoteUsbSessionBinding::openRequested,
                     &binding, [&openRequests](quint64, quint64) { ++openRequests; });
    QObject::connect(&binding, &RemoteUsbSessionBinding::openAccepted,
                     &binding, [&openAccepted]() { ++openAccepted; });
    QObject::connect(&binding, &RemoteUsbSessionBinding::stopped,
                     &binding, [&stopped]() { ++stopped; });

    QString error;
    ok &= require(binding.start(&error),
                  QStringLiteral("session binding did not start: %1").arg(error), err);
    ok &= require(channel.sent.size() == 1 &&
                      channel.sent.first().size() == static_cast<qsizetype>(kBrokerHelloSize),
                  QStringLiteral("HELLO was not emitted as the first 84 bytes"), err);

    const QByteArray hello = makeHello(options.brokerHello);
    channel.feed(hello.left(3));
    channel.feed(hello.mid(3));
    ok &= require(binding.feedBytes({}, &error),
                  QStringLiteral("empty deterministic feed failed: %1").arg(error), err);

    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ok &= require(helloAccepted == 1,
                  QStringLiteral("fragmented HELLO was not accepted"), err);

    DeviceSnapshot device;
    device.busId = QByteArrayLiteral("1-2");
    device.rawDescriptors = QByteArrayLiteral("\x12\x01");
    device.endpoints.append(Endpoint { 0, 0, 0x81, 2, 64, 1, 0 });
    ok &= require(binding.sendCapability(device, &error),
                  QStringLiteral("capability was not emitted: %1").arg(error), err);
    ok &= require(channel.sent.size() == 2,
                  QStringLiteral("capability frame was not sent"), err);

    const QByteArray open = makePeerOpenFrame(options.brokerHello);
    for (qsizetype offset = 0; offset < open.size();) {
        const qsizetype size = std::min<qsizetype>(5, open.size() - offset);
        channel.feed(open.mid(offset, size));
        offset += size;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ok &= require(openRequests == 1,
                  QStringLiteral("fragmented OPEN was not delivered"), err);
    ok &= require(binding.sendOpenOk(&error),
                  QStringLiteral("OPEN_OK was not emitted: %1").arg(error), err);
    ok &= require(openAccepted == 1 && channel.sent.size() == 3,
                  QStringLiteral("OPEN_OK did not transition to open"), err);

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
