#include "usb_agent_backend.h"

#include "../app/remoteusb/remote_usb_libusb_adapter.h"

#ifdef MOONLIGHT_USB_AGENT_RUNTIME
#include "../app/remoteusb/remote_usb_broker_client.h"
#include "../app/remoteusb/remote_usb_session_binding.h"
#include "../app/remoteusb/remote_usb_tls_channel.h"
#endif

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSslKey>
#include <QSslSocket>

#include <algorithm>
#include <limits>
#include <utility>

namespace RemoteUsbAgent {

struct LibusbBackend::Impl
{
    RemoteUsb::RemoteUsbLibusbAdapter adapter;
    BackendCallbacks callbacks;
    quint64 generation = 0;
    bool stopping = false;

#ifdef MOONLIGHT_USB_AGENT_RUNTIME
    QNetworkAccessManager networkManager;
    RemoteUsb::RemoteUsbBrokerClient *brokerClient = nullptr;
    RemoteUsb::RemoteUsbTlsChannel *channel = nullptr;
    RemoteUsb::RemoteUsbSessionBinding *binding = nullptr;
    RemoteUsb::DeviceSnapshot selected;
    RemoteUsb::BrokerCapabilityRequest lease;
#endif
};

namespace {
QJsonObject snapshotToJson(const RemoteUsb::DeviceSnapshot &device)
{
    QJsonObject result {
        { QStringLiteral("deviceId"), QString::fromUtf8(device.deviceId) },
        { QStringLiteral("busId"), QString::fromUtf8(device.busId) },
        { QStringLiteral("displayName"), device.displayName },
        { QStringLiteral("vendorId"), static_cast<int>(device.vendorId) },
        { QStringLiteral("productId"), static_cast<int>(device.productId) },
        { QStringLiteral("deviceBcd"), static_cast<int>(device.deviceBcd) },
        { QStringLiteral("deviceClass"), static_cast<int>(device.deviceClass) },
        { QStringLiteral("deviceSubclass"), static_cast<int>(device.deviceSubclass) },
        { QStringLiteral("deviceProtocol"), static_cast<int>(device.deviceProtocol) },
        { QStringLiteral("isochronous"), device.hasIsochronousEndpoints }
    };
    QJsonArray endpoints;
    for (const RemoteUsb::Endpoint &endpoint : device.endpoints) {
        endpoints.append(QJsonObject {
            { QStringLiteral("interface"), static_cast<int>(endpoint.interfaceNumber) },
            { QStringLiteral("alternate"), static_cast<int>(endpoint.alternateSetting) },
            { QStringLiteral("address"), static_cast<int>(endpoint.address) },
            { QStringLiteral("attributes"), static_cast<int>(endpoint.attributes) },
            { QStringLiteral("maxPacketSize"), static_cast<int>(endpoint.maxPacketSize) },
            { QStringLiteral("interval"), static_cast<int>(endpoint.interval) }
        });
    }
    result.insert(QStringLiteral("endpoints"), endpoints);
    return result;
}

#ifdef MOONLIGHT_USB_AGENT_RUNTIME
bool readUnsigned(const QJsonObject &request, const QString &name,
                  quint64 *value)
{
    if (value == nullptr || !request.value(name).isString()) {
        return false;
    }
    bool ok = false;
    const quint64 parsed = request.value(name).toString().toULongLong(&ok);
    if (!ok || parsed == 0) {
        return false;
    }
    *value = parsed;
    return true;
}

QByteArray decodeBase64(const QJsonObject &request, const QString &name,
                        qsizetype maximum)
{
    const QJsonValue value = request.value(name);
    if (!value.isString() || value.toString().size() > maximum * 2) {
        return {};
    }
    const QByteArray encoded = value.toString().toLatin1();
    const QByteArray decoded = QByteArray::fromBase64(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    return decoded.size() <= maximum ? decoded : QByteArray {};
}
#endif
}

LibusbBackend::LibusbBackend()
    : m_impl(std::make_unique<Impl>())
{
}

LibusbBackend::~LibusbBackend() = default;

QJsonArray LibusbBackend::enumerate(QString *error)
{
    QJsonArray devices;
    if (m_impl == nullptr || !m_impl->adapter.isAvailable()) {
        if (error != nullptr) {
            *error = QStringLiteral("libusb backend is unavailable");
        }
        return devices;
    }
    const QVector<RemoteUsb::DeviceSnapshot> snapshots =
        m_impl->adapter.enumerate(error);
    for (const RemoteUsb::DeviceSnapshot &snapshot : snapshots) {
        devices.append(snapshotToJson(snapshot));
    }
    return devices;
}

void LibusbBackend::setCallbacks(BackendCallbacks callbacks)
{
    if (m_impl != nullptr) {
        m_impl->callbacks = std::move(callbacks);
    }
}

bool LibusbBackend::start(const QJsonObject &request, QString *error)
{
#ifndef MOONLIGHT_USB_AGENT_RUNTIME
    Q_UNUSED(request);
    if (error != nullptr) {
        *error = QStringLiteral("USB agent was built without the shared-core runtime");
    }
    return false;
#else
    if (m_impl == nullptr || !m_impl->adapter.isAvailable() ||
        m_impl->binding != nullptr) {
        if (error != nullptr) {
            *error = m_impl != nullptr && m_impl->binding != nullptr
                ? QStringLiteral("a USB lease is already active")
                : QStringLiteral("libusb backend is unavailable");
        }
        return false;
    }

    quint64 generation = 0;
    RemoteUsb::BrokerCapabilityRequest lease;
    if (!readUnsigned(request, QStringLiteral("generation"), &generation) ||
        !readUnsigned(request, QStringLiteral("streamGeneration"),
                      &lease.streamGeneration) ||
        !readUnsigned(request, QStringLiteral("sessionToken"),
                      &lease.sessionToken) ||
        !readUnsigned(request, QStringLiteral("attachmentToken"),
                      &lease.attachmentToken) ||
        !readUnsigned(request, QStringLiteral("leaseToken"),
                      &lease.leaseToken)) {
        if (error != nullptr) {
            *error = QStringLiteral("USB lease identifiers are invalid");
        }
        return false;
    }

    const QString requestedId = request.value(QStringLiteral("deviceId"))
                                    .toString().trimmed();
    QString enumerateError;
    const QVector<RemoteUsb::DeviceSnapshot> devices =
        m_impl->adapter.enumerate(&enumerateError);
    if (!enumerateError.isEmpty()) {
        if (error != nullptr) {
            *error = enumerateError;
        }
        return false;
    }
    RemoteUsb::DeviceSnapshot selected;
    QVector<RemoteUsb::DeviceSnapshot> supported;
    for (const RemoteUsb::DeviceSnapshot &device : devices) {
        if (!device.hasIsochronousEndpoints) {
            supported.append(device);
        }
        if (!requestedId.isEmpty() &&
            (device.deviceId == requestedId.toUtf8() ||
             device.busId == requestedId.toUtf8())) {
            selected = device;
        }
    }
    if (requestedId.isEmpty() && supported.size() == 1) {
        selected = supported.first();
    }
    if (selected.deviceId.isEmpty() || selected.hasIsochronousEndpoints) {
        if (error != nullptr) {
            *error = selected.hasIsochronousEndpoints
                ? QStringLiteral("isochronous USB devices are not supported")
                : QStringLiteral("requested USB device is not available");
        }
        return false;
    }

    const QByteArray serverDer = decodeBase64(
        request, QStringLiteral("serverCertificate"), 16 * 1024);
    const QByteArray clientDer = decodeBase64(
        request, QStringLiteral("clientCertificate"), 16 * 1024);
    const QByteArray privateKeyPem = decodeBase64(
        request, QStringLiteral("clientPrivateKey"), 32 * 1024);
    QSslCertificate serverCertificate(serverDer, QSsl::Der);
    QSslCertificate clientCertificate(clientDer, QSsl::Der);
    QSslKey privateKey(privateKeyPem, QSsl::Rsa, QSsl::Pem,
                       QSsl::PrivateKey);
    if (privateKey.isNull()) {
        privateKey = QSslKey(privateKeyPem, QSsl::Ec, QSsl::Pem,
                             QSsl::PrivateKey);
    }

    QSslConfiguration sslConfiguration = QSslConfiguration::defaultConfiguration();
    sslConfiguration.setLocalCertificate(clientCertificate);
    sslConfiguration.setPrivateKey(privateKey);
    sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyPeer);

    RemoteUsb::BrokerHostConfig hostConfig;
    hostConfig.host = request.value(QStringLiteral("host")).toString();
    const int httpsPort = request.value(QStringLiteral("httpsPort")).toInt();
    if (httpsPort > 0 && httpsPort <= std::numeric_limits<quint16>::max()) {
        hostConfig.httpsPort = static_cast<quint16>(httpsPort);
    }
    hostConfig.serverCertificate = serverCertificate;
    hostConfig.clientIdentity =
        request.value(QStringLiteral("clientIdentity")).toString();
    hostConfig.clientName =
        request.value(QStringLiteral("clientName")).toString();
    hostConfig.sslConfiguration = sslConfiguration;
    if (!hostConfig.valid()) {
        if (error != nullptr) {
            *error = QStringLiteral("USB lease host identity is invalid");
        }
        return false;
    }

    delete m_impl->brokerClient;
    m_impl->brokerClient = new RemoteUsb::RemoteUsbBrokerClient(
        hostConfig, &m_impl->networkManager);
    auto capability = m_impl->brokerClient->fetch(lease, 5000, error);
    if (!capability) {
        delete m_impl->brokerClient;
        m_impl->brokerClient = nullptr;
        return false;
    }

    const QByteArray wireIdentity =
        RemoteUsb::RemoteUsbBrokerClient::wireIdentity(hostConfig.clientIdentity);
    if (wireIdentity.size() != 16 || capability->nonce.size() != 16) {
        if (error != nullptr) {
            *error = QStringLiteral("USB broker identity is invalid");
        }
        delete m_impl->brokerClient;
        m_impl->brokerClient = nullptr;
        return false;
    }

    RemoteUsb::RemoteUsbBrokerHello hello;
    std::copy_n(reinterpret_cast<const std::uint8_t *>(wireIdentity.constData()),
                hello.clientUuid.size(), hello.clientUuid.begin());
    hello.streamGeneration = lease.streamGeneration;
    hello.sessionToken = lease.sessionToken;
    hello.attachmentToken = lease.attachmentToken;
    hello.leaseToken = lease.leaseToken;
    std::copy_n(reinterpret_cast<const std::uint8_t *>(capability->nonce.constData()),
                hello.capabilityNonce.size(), hello.capabilityNonce.begin());
    hello.maxPdu = capability->maxUrb;
    hello.maxInflight = capability->maxInflight;
    hello.isochronous = false;

    RemoteUsb::RemoteUsbTlsChannelConfig channelConfig;
    channelConfig.host = capability->host;
    channelConfig.port = capability->port;
    channelConfig.sslConfiguration = sslConfiguration;
    channelConfig.pinnedServerCertificate = serverCertificate;

    RemoteUsb::RemoteUsbSessionBindingOptions bindingOptions;
    bindingOptions.brokerHello = hello;
    bindingOptions.txWindowBytes = capability->txWindowBytes;
    bindingOptions.txWindowPdus = capability->txWindowPdus;
    bindingOptions.rxWindowBytes = capability->rxWindowBytes;
    bindingOptions.rxWindowPdus = capability->rxWindowPdus;
    bindingOptions.maxReassemblySize = capability->maxReassemblySize;
    bindingOptions.maxFragments = capability->maxFragments;
    bindingOptions.maxInflight = capability->maxInflight;
    bindingOptions.maxTransferSize = capability->maxUrb - RemoteUsb::kPduHeaderSize;

    m_impl->generation = generation;
    m_impl->lease = lease;
    m_impl->selected = selected;
    m_impl->stopping = false;
    m_impl->channel = new RemoteUsb::RemoteUsbTlsChannel(channelConfig);
    m_impl->binding = new RemoteUsb::RemoteUsbSessionBinding(
        &m_impl->adapter, m_impl->channel, bindingOptions);

    Impl *impl = m_impl.get();
    QPointer<RemoteUsb::RemoteUsbSessionBinding> binding(impl->binding);
    QObject::connect(impl->binding,
                     &RemoteUsb::RemoteUsbSessionBinding::helloAccepted,
                     &impl->networkManager, [impl, binding] {
        if (!binding) {
            return;
        }
        QString sendError;
        if (!binding->sendCapability(impl->selected, &sendError) &&
            impl->callbacks.failed) {
            impl->callbacks.failed(impl->generation, sendError);
            binding->stop();
        }
    }, Qt::QueuedConnection);
    QObject::connect(impl->binding,
                     &RemoteUsb::RemoteUsbSessionBinding::openRequested,
                     &impl->networkManager, [impl, binding](quint64 leaseToken,
                                                    quint64 attachmentToken) {
        if (!binding || leaseToken != impl->lease.leaseToken ||
            attachmentToken != impl->lease.attachmentToken) {
            return;
        }
        QString claimError;
        if (!impl->adapter.claim(impl->selected, &claimError)) {
            binding->sendOpenReject(1, nullptr);
            if (impl->callbacks.failed) {
                impl->callbacks.failed(impl->generation, claimError);
            }
            binding->stop();
            return;
        }
        QString openError;
        if (!binding->sendOpenOk(&openError)) {
            if (impl->callbacks.failed) {
                impl->callbacks.failed(impl->generation, openError);
            }
            binding->stop();
            return;
        }
        if (impl->callbacks.opened) {
            impl->callbacks.opened(impl->selected.deviceId, impl->generation);
        }
    }, Qt::QueuedConnection);
    QObject::connect(impl->binding,
                     &RemoteUsb::RemoteUsbSessionBinding::openRejected,
                     &impl->networkManager, [impl, binding](quint32) {
        if (impl->callbacks.failed) {
            impl->callbacks.failed(impl->generation,
                                   QStringLiteral("Remote USB host rejected OPEN"));
        }
        if (binding) {
            binding->stop();
        }
    }, Qt::QueuedConnection);
    QObject::connect(impl->binding,
                     &RemoteUsb::RemoteUsbSessionBinding::peerClosed,
                     &impl->networkManager, [binding](quint64) {
        if (binding) {
            binding->stop();
        }
    }, Qt::QueuedConnection);
    QObject::connect(impl->binding,
                     &RemoteUsb::RemoteUsbSessionBinding::errorOccurred,
                     &impl->networkManager, [impl, binding](const QString &message) {
        if (!impl->stopping && impl->callbacks.failed) {
            impl->callbacks.failed(impl->generation, message);
        }
        if (binding) {
            binding->stop();
        }
    }, Qt::QueuedConnection);
    QObject::connect(impl->binding,
                     &RemoteUsb::RemoteUsbSessionBinding::stopped,
                     &impl->networkManager, [impl] {
        const quint64 stoppedGeneration = impl->generation;
        delete impl->binding;
        impl->binding = nullptr;
        delete impl->channel;
        impl->channel = nullptr;
        delete impl->brokerClient;
        impl->brokerClient = nullptr;
        impl->adapter.release();
        impl->selected = {};
        impl->lease = {};
        impl->generation = 0;
        impl->stopping = false;
        if (impl->callbacks.stopped) {
            impl->callbacks.stopped(stoppedGeneration);
        }
    }, Qt::QueuedConnection);

    QString startError;
    if (!m_impl->binding->start(&startError)) {
        if (error != nullptr) {
            *error = startError.isEmpty()
                ? QStringLiteral("USB lease tunnel failed to start") : startError;
        }
        m_impl->stopping = true;
        m_impl->binding->stop();
        return false;
    }
    return true;
#endif
}

void LibusbBackend::stop() noexcept
{
    if (m_impl != nullptr) {
#ifdef MOONLIGHT_USB_AGENT_RUNTIME
        if (m_impl->binding != nullptr) {
            m_impl->stopping = true;
            m_impl->binding->stop();
            return;
        }
#endif
        m_impl->adapter.release();
        if (m_impl->callbacks.stopped) {
            m_impl->callbacks.stopped(m_impl->generation);
        }
    }
}

} // namespace RemoteUsbAgent
