#include "remote_usb_broker_client.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QThread>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace RemoteUsb {

namespace {

constexpr quint64 kMaxUInt32 = std::numeric_limits<quint32>::max();
constexpr quint32 kMaxExpiresMs = kBrokerCapabilityMaxExpiresMs;

bool isAsciiHex(QChar value) noexcept
{
    return (value >= QLatin1Char('0') && value <= QLatin1Char('9')) ||
           (value >= QLatin1Char('a') && value <= QLatin1Char('f')) ||
           (value >= QLatin1Char('A') && value <= QLatin1Char('F'));
}

int base64UrlValue(QChar value) noexcept
{
    if (value >= QLatin1Char('A') && value <= QLatin1Char('Z')) {
        return value.unicode() - QLatin1Char('A').unicode();
    }
    if (value >= QLatin1Char('a') && value <= QLatin1Char('z')) {
        return value.unicode() - QLatin1Char('a').unicode() + 26;
    }
    if (value >= QLatin1Char('0') && value <= QLatin1Char('9')) {
        return value.unicode() - QLatin1Char('0').unicode() + 52;
    }
    if (value == QLatin1Char('-')) {
        return 62;
    }
    if (value == QLatin1Char('_')) {
        return 63;
    }
    return -1;
}

QString stripHostBrackets(QString host)
{
    host = host.trimmed();
    if (host.size() >= 2 && host.startsWith(QLatin1Char('[')) &&
        host.endsWith(QLatin1Char(']'))) {
        host = host.mid(1, host.size() - 2);
    }
    return host;
}

bool equalCertificate(const QSslCertificate &left,
                      const QSslCertificate &right) noexcept
{
    return !left.isNull() && !right.isNull() && left.toDer() == right.toDer();
}

bool isPinnedTrustError(const QSslError &error,
                        const QSslCertificate &pinned) noexcept
{
    if (!equalCertificate(error.certificate(), pinned)) {
        return false;
    }
    switch (error.error()) {
    case QSslError::UnableToGetLocalIssuerCertificate:
    case QSslError::UnableToVerifyFirstCertificate:
    case QSslError::CertificateUntrusted:
    case QSslError::SelfSignedCertificate:
        return true;
    default:
        /* Pinning must not bypass hostname, validity, or revocation checks. */
        return false;
    }
}

QString firstNonEmpty(const QJsonObject &object,
                      const QStringList &names)
{
    for (const QString &name : names) {
        const QJsonValue value = object.value(name);
        if (value.isString() && !value.toString().trimmed().isEmpty()) {
            return value.toString().trimmed();
        }
    }
    return {};
}

} // namespace

bool BrokerCapability::valid() const noexcept
{
    return !host.trimmed().isEmpty() && port != 0 && nonce.size() == 16 &&
           !nonce.isEmpty() && nonce != QByteArray(nonce.size(), '\0') &&
           maxUrb >= kBrokerCapabilityMinUrb &&
           maxUrb <= kBrokerCapabilityMaxUrb && maxInflight != 0 &&
           maxInflight <= kBrokerCapabilityMaxInflight && txWindowBytes != 0 &&
           txWindowBytes <= kBrokerCapabilityMaxWindowBytes &&
           txWindowPdus != 0 && txWindowPdus <= kBrokerCapabilityMaxWindowPdus &&
           rxWindowBytes != 0 && rxWindowBytes <= kBrokerCapabilityMaxWindowBytes &&
           rxWindowPdus != 0 && rxWindowPdus <= kBrokerCapabilityMaxWindowPdus &&
           maxReassemblySize >= 48 &&
           maxReassemblySize <= kBrokerCapabilityMaxUrb && maxFragments != 0 &&
           maxFragments <= kBrokerCapabilityMaxWindowPdus;
}

bool BrokerHostConfig::valid() const noexcept
{
    return !host.trimmed().isEmpty() && httpsPort != 0 &&
           !serverCertificate.isNull() &&
           !RemoteUsbBrokerClient::canonicalIdentity(clientIdentity).isEmpty() &&
           !clientName.trimmed().isEmpty() &&
           !sslConfiguration.localCertificate().isNull() &&
           !sslConfiguration.privateKey().isNull() &&
           sslConfiguration.peerVerifyMode() != QSslSocket::VerifyNone;
}

RemoteUsbBrokerClient::RemoteUsbBrokerClient(BrokerHostConfig hostConfig,
                                             QNetworkAccessManager *networkManager,
                                             QObject *parent)
    : QObject(parent),
      m_networkManager(networkManager != nullptr
                           ? networkManager
                           : new QNetworkAccessManager(this)),
      m_hostConfig(std::move(hostConfig))
{
    if (m_networkManager != nullptr) {
        m_networkManager->setProxy(QNetworkProxy::NoProxy);
    }
}

QString RemoteUsbBrokerClient::canonicalIdentity(const QString &value)
{
    const QString normalized = value.trimmed();
    if (normalized.size() == 16) {
        bool valid = true;
        for (const QChar character : normalized) {
            if (!isAsciiHex(character)) {
                valid = false;
                break;
            }
        }
        if (valid) {
            return normalized.toUpper();
        }
    }

    if (normalized.isEmpty()) {
        return {};
    }
    const QByteArray digest = QCryptographicHash::hash(
        normalized.toUtf8(), QCryptographicHash::Sha256).left(16);
    return QString::fromLatin1(digest.toHex().toUpper());
}

QByteArray RemoteUsbBrokerClient::wireIdentity(const QString &value)
{
    const QString canonical = canonicalIdentity(value);
    if (canonical.size() == 16) {
        return canonical.toLatin1();
    }
    if (canonical.size() == 32) {
        return QByteArray::fromHex(canonical.toLatin1());
    }
    return {};
}

bool RemoteUsbBrokerClient::sameHost(const QString &left,
                                     const QString &right) noexcept
{
    const QString normalizedLeft = stripHostBrackets(left);
    const QString normalizedRight = stripHostBrackets(right);
    if (normalizedLeft.isEmpty() || normalizedRight.isEmpty()) {
        return false;
    }

    QHostAddress leftAddress;
    QHostAddress rightAddress;
    if (leftAddress.setAddress(normalizedLeft) &&
        rightAddress.setAddress(normalizedRight)) {
        return leftAddress == rightAddress;
    }
    return normalizedLeft.compare(normalizedRight, Qt::CaseInsensitive) == 0;
}

bool RemoteUsbBrokerClient::decodeNonce(const QString &encoded,
                                        QByteArray *out) noexcept
{
    if (out == nullptr || encoded.isEmpty() || encoded.size() > 64) {
        return false;
    }

    QByteArray decoded;
    decoded.reserve(16);
    quint32 accumulator = 0;
    int bits = 0;
    for (const QChar character : encoded) {
        const int value = base64UrlValue(character);
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<quint32>(value);
        bits += 6;
        while (bits >= 8) {
            bits -= 8;
            decoded.append(static_cast<char>((accumulator >> bits) & 0xffu));
            if (decoded.size() > 16) {
                return false;
            }
        }
    }
    if (bits != 0 && (accumulator & ((1u << bits) - 1u)) != 0) {
        return false;
    }
    if (decoded.size() != 16 || decoded == QByteArray(16, '\0')) {
        return false;
    }
    *out = std::move(decoded);
    return true;
}

bool RemoteUsbBrokerClient::readUnsigned(const QJsonValue &value,
                                         quint64 maximum,
                                         quint64 *out) noexcept
{
    if (out == nullptr || !value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(maximum) ||
        std::floor(number) != number) {
        return false;
    }
    *out = static_cast<quint64>(number);
    return true;
}

bool RemoteUsbBrokerClient::readField(const QJsonObject &object,
                                      const QJsonObject &limits,
                                      const QStringList &names,
                                      quint64 maximum,
                                      quint64 defaultValue,
                                      quint64 *out) noexcept
{
    if (out == nullptr) {
        return false;
    }
    for (const QString &name : names) {
        if (limits.contains(name)) {
            return readUnsigned(limits.value(name), maximum, out);
        }
    }
    for (const QString &name : names) {
        if (object.contains(name)) {
            return readUnsigned(object.value(name), maximum, out);
        }
    }
    *out = defaultValue;
    return true;
}

bool RemoteUsbBrokerClient::parseCapability(const QByteArray &json,
                                            const QString &expectedHost,
                                            BrokerCapability *out,
                                            QString *error)
{
    auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (out == nullptr || expectedHost.trimmed().isEmpty() ||
        json.isEmpty() || json.size() > kBrokerCapabilityMaxResponseBytes) {
        return fail(QStringLiteral("Remote USB capability response is invalid"));
    }

    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("Remote USB capability is not valid JSON"));
    }
    const QJsonObject object = document.object();

    quint64 version = 0;
    if (!readUnsigned(object.value(QStringLiteral("version")),
                      kMaxUInt32, &version) ||
        version != kBrokerCapabilityVersion) {
        return fail(QStringLiteral("Remote USB capability version is unsupported"));
    }
    if (object.contains(QStringLiteral("enabled")) &&
        (!object.value(QStringLiteral("enabled")).isBool() ||
         !object.value(QStringLiteral("enabled")).toBool())) {
        return fail(QStringLiteral("Remote USB broker is disabled"));
    }

    const QJsonObject endpoint = object.value(QStringLiteral("endpoint")).toObject();
    QString host = firstNonEmpty(endpoint, { QStringLiteral("host") });
    if (host.isEmpty()) {
        host = firstNonEmpty(object,
                             { QStringLiteral("brokerHost"), QStringLiteral("host") });
    }
    if (host.isEmpty()) {
        host = stripHostBrackets(expectedHost);
    }
    if (!sameHost(host, expectedHost)) {
        return fail(QStringLiteral("Remote USB broker endpoint host is not paired"));
    }

    quint64 port = 0;
    const QJsonValue endpointPort = endpoint.value(QStringLiteral("port"));
    if (!endpointPort.isUndefined()) {
        if (!readUnsigned(endpointPort, 65535, &port)) {
            return fail(QStringLiteral("Remote USB broker endpoint port is invalid"));
        }
    } else {
        const QJsonValue flatPort = object.contains(QStringLiteral("brokerPort"))
            ? object.value(QStringLiteral("brokerPort"))
            : object.value(QStringLiteral("port"));
        if (!readUnsigned(flatPort, 65535, &port)) {
            return fail(QStringLiteral("Remote USB broker endpoint port is invalid"));
        }
    }
    if (port == 0) {
        return fail(QStringLiteral("Remote USB broker endpoint port is invalid"));
    }

    const QString nonceText = firstNonEmpty(
        object,
        { QStringLiteral("nonce"), QStringLiteral("capabilityNonce"),
          QStringLiteral("capability_nonce") });
    QByteArray nonce;
    if (!decodeNonce(nonceText, &nonce)) {
        return fail(QStringLiteral("Remote USB capability nonce is invalid"));
    }

    const QJsonObject limits = object.value(QStringLiteral("limits")).toObject();
    quint64 maxUrb = 0;
    quint64 maxInflight = 0;
    quint64 txWindowBytes = 0;
    quint64 txWindowPdus = 0;
    quint64 rxWindowBytes = 0;
    quint64 rxWindowPdus = 0;
    quint64 maxReassemblySize = 0;
    quint64 maxFragments = 0;
    quint64 expiresMs = 0;
    if (!readField(object, limits,
                   { QStringLiteral("maxUrb"), QStringLiteral("max_urb") },
                   kBrokerCapabilityMaxUrb, kBrokerCapabilityMaxUrb, &maxUrb) ||
        !readField(object, limits,
                   { QStringLiteral("maxInflight"), QStringLiteral("max_inflight") },
                   kBrokerCapabilityMaxInflight, kBrokerCapabilityMaxInflight,
                   &maxInflight) ||
        !readField(object, limits,
                   { QStringLiteral("txWindowBytes"), QStringLiteral("tx_window_bytes") },
                   kBrokerCapabilityMaxWindowBytes,
                   kBrokerCapabilityDefaultWindowBytes, &txWindowBytes) ||
        !readField(object, limits,
                   { QStringLiteral("txWindowPdus"), QStringLiteral("tx_window_pdus") },
                   kBrokerCapabilityMaxWindowPdus,
                   kBrokerCapabilityDefaultWindowPdus, &txWindowPdus) ||
        !readField(object, limits,
                   { QStringLiteral("rxWindowBytes"), QStringLiteral("rx_window_bytes") },
                   kBrokerCapabilityMaxWindowBytes,
                   kBrokerCapabilityDefaultWindowBytes, &rxWindowBytes) ||
        !readField(object, limits,
                   { QStringLiteral("rxWindowPdus"), QStringLiteral("rx_window_pdus") },
                   kBrokerCapabilityMaxWindowPdus,
                   kBrokerCapabilityDefaultWindowPdus, &rxWindowPdus) ||
        !readField(object, limits,
                   { QStringLiteral("maxReassemblySize"),
                     QStringLiteral("max_reassembly_size") },
                   kBrokerCapabilityMaxUrb, kBrokerCapabilityDefaultReassemblySize,
                   &maxReassemblySize) ||
        !readField(object, limits,
                   { QStringLiteral("maxFragments"), QStringLiteral("max_fragments") },
                   kBrokerCapabilityMaxWindowPdus,
                   kBrokerCapabilityDefaultMaxFragments, &maxFragments) ||
        !readField(object, limits,
                   { QStringLiteral("expiresMs"), QStringLiteral("expires_ms") },
                   kMaxExpiresMs, 0, &expiresMs)) {
        return fail(QStringLiteral("Remote USB capability limits are invalid"));
    }

    BrokerCapability capability;
    capability.host = stripHostBrackets(host);
    capability.port = static_cast<quint16>(port);
    capability.nonce = std::move(nonce);
    capability.maxUrb = static_cast<quint32>(maxUrb);
    capability.maxInflight = static_cast<quint32>(maxInflight);
    capability.txWindowBytes = txWindowBytes;
    capability.txWindowPdus = static_cast<quint32>(txWindowPdus);
    capability.rxWindowBytes = rxWindowBytes;
    capability.rxWindowPdus = static_cast<quint32>(rxWindowPdus);
    capability.maxReassemblySize = static_cast<quint32>(maxReassemblySize);
    capability.maxFragments = static_cast<quint32>(maxFragments);
    capability.expiresMs = static_cast<quint32>(expiresMs);
    if (!capability.valid()) {
        return fail(QStringLiteral("Remote USB capability limits are out of range"));
    }
    *out = std::move(capability);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::optional<BrokerCapability> RemoteUsbBrokerClient::fetch(
    const BrokerCapabilityRequest &request,
    int timeoutMs,
    QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<BrokerCapability> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    if (request.streamGeneration == 0 || request.sessionToken == 0 ||
        request.attachmentToken == 0 || request.leaseToken == 0 ||
        timeoutMs <= 0 || timeoutMs > 120000 || m_networkManager == nullptr) {
        return fail(QStringLiteral("Remote USB capability request is invalid"));
    }
    if (QThread::currentThread() != m_networkManager->thread()) {
        return fail(QStringLiteral("Remote USB capability request must run on the network thread"));
    }

    const BrokerHostConfig &host = m_hostConfig;
    if (!host.valid()) {
        return fail(QStringLiteral("Remote USB paired host is not ready"));
    }
    const QString identity = canonicalIdentity(host.clientIdentity);

    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(host.host);
    url.setPort(host.httpsPort);
    url.setPath(QStringLiteral("/api/v1/remote-usb/capability"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("stream_generation"),
                       QString::number(request.streamGeneration));
    query.addQueryItem(QStringLiteral("uniqueid"), identity);
    query.addQueryItem(QStringLiteral("clientname"),
                       host.clientName.trimmed());
    url.setQuery(query);

    QNetworkRequest networkRequest(url);
    networkRequest.setRawHeader("Accept", "application/json");
    /* Lease tokens authorize a one-shot broker capability.  Keep them out of
     * URLs so proxies, access logs, referrers, and browser history cannot
     * capture bearer material.  Sunshine retains a query path only for older
     * clients during migration; this client always uses the header contract. */
    networkRequest.setRawHeader("X-Remote-USB-Session-Token",
                                QByteArray::number(request.sessionToken));
    networkRequest.setRawHeader("X-Remote-USB-Attachment-Token",
                                QByteArray::number(request.attachmentToken));
    networkRequest.setRawHeader("X-Remote-USB-Lease-Token",
                                QByteArray::number(request.leaseToken));
    networkRequest.setSslConfiguration(host.sslConfiguration);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    networkRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    networkRequest.setAttribute(
        QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
#endif

    QNetworkReply *reply = m_networkManager->get(networkRequest);
    if (reply == nullptr) {
        return fail(QStringLiteral("Remote USB capability request could not be started"));
    }

    const QMetaObject::Connection sslConnection = connect(
        reply,
        &QNetworkReply::sslErrors,
        reply,
        [reply, pinned = host.serverCertificate](const QList<QSslError> &errors) {
            if (pinned.isNull() || errors.isEmpty()) {
                return;
            }
            QList<QSslError> trustErrors;
            for (const QSslError &sslError : errors) {
                if (!isPinnedTrustError(sslError, pinned)) {
                    return;
                }
                trustErrors.append(sslError);
            }
            if (!trustErrors.isEmpty()) {
                reply->ignoreSslErrors(trustErrors);
            }
        });

    QByteArray body;
    bool bodyTooLarge = false;
    const QMetaObject::Connection bodyConnection = connect(
        reply,
        &QIODevice::readyRead,
        reply,
        [&body, &bodyTooLarge, reply]() {
            if (bodyTooLarge) {
                return;
            }
            const qsizetype remaining =
                kBrokerCapabilityMaxResponseBytes - body.size();
            const qint64 readSize = static_cast<qint64>(std::max<qsizetype>(
                1, remaining + 1));
            body += reply->read(readSize);
            if (body.size() > kBrokerCapabilityMaxResponseBytes) {
                bodyTooLarge = true;
                reply->abort();
            }
        });

    QEventLoop loop;
    QMetaObject::Connection finishedConnection = connect(
        reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QMetaObject::Connection aboutToQuitConnection;
    if (QCoreApplication::instance() != nullptr) {
        aboutToQuitConnection = connect(QCoreApplication::instance(),
                                        &QCoreApplication::aboutToQuit,
                                        &loop,
                                        &QEventLoop::quit);
    }
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    if (!reply->isFinished()) {
        reply->abort();
    }
    if (!bodyTooLarge) {
        const qsizetype remaining =
            kBrokerCapabilityMaxResponseBytes - body.size();
        const qint64 readSize = static_cast<qint64>(std::max<qsizetype>(
            1, remaining + 1));
        body += reply->read(readSize);
        if (body.size() > kBrokerCapabilityMaxResponseBytes) {
            bodyTooLarge = true;
        }
    }
    disconnect(finishedConnection);
    disconnect(aboutToQuitConnection);
    disconnect(bodyConnection);
    disconnect(sslConnection);

    if (bodyTooLarge) {
        delete reply;
        return fail(QStringLiteral("Remote USB capability response is too large"));
    }
    if (!reply->isFinished()) {
        delete reply;
        return fail(QStringLiteral("Remote USB capability request timed out"));
    }
    if (reply->error() != QNetworkReply::NoError) {
        const bool timeout = reply->error() == QNetworkReply::OperationCanceledError;
        delete reply;
        return fail(timeout ? QStringLiteral("Remote USB capability request timed out")
                            : QStringLiteral("Remote USB capability request failed"));
    }
    const int statusCode = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    delete reply;
    if (statusCode < 200 || statusCode >= 300) {
        return fail(QStringLiteral("Remote USB capability request was rejected"));
    }

    BrokerCapability capability;
    if (!parseCapability(body, host.host, &capability, error)) {
        return std::nullopt;
    }
    return capability;
}

} // namespace RemoteUsb
