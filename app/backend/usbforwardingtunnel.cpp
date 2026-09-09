#include "usbforwardingtunnel.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QTcpSocket>

namespace UsbForwarding {

namespace {
/* Bound the handshake line so a hostile peer cannot grow the buffer. The
 * limit covers content bytes before '\n', matching the server's buffer. */
constexpr qsizetype kMaxHandshakeBytes = 4 * 1024;
/* Stop reading from one side while the other side is this far behind. TCP
 * back-pressures the USB/IP peer instead of us buffering without limit. */
constexpr qint64 kHighWaterMark = 4 * 1024 * 1024;
/* Deadline covering TCP connect, TLS, the JSON handshake, and the host-side
 * usbip attach that must finish before the server sends its ready line
 * (the server allows 12 s for the same window; the client must outlast it). */
constexpr int kStartupTimeoutMs = 15 * 1000;
/* Character set and length the server's valid_busid() accepts; validating
 * here rejects a misconfigured request before the TLS connection. */
constexpr qsizetype kMaxBusIdBytes = 31;
} // namespace

bool TunnelConfig::valid() const noexcept
{
    if (host.isEmpty() || port == 0 || sessionToken.isEmpty() ||
        busId.isEmpty() || localPort == 0) {
        return false;
    }
    /* Mirror the server's valid_busid(): printable busid characters only,
     * at most 31 bytes. */
    if (busId.size() > kMaxBusIdBytes) {
        return false;
    }
    for (const char c : busId) {
        const bool allowed = (c >= '0' && c <= '9') ||
                             (c >= 'a' && c <= 'z') ||
                             (c >= 'A' && c <= 'Z') ||
                             c == '-' || c == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

Tunnel::Tunnel(TunnelConfig config, QObject *parent)
    : QObject(parent), m_Config(std::move(config))
{
    m_StartupTimer = new QTimer(this);
    m_StartupTimer->setSingleShot(true);
    connect(m_StartupTimer, &QTimer::timeout, this, [this] {
        failWith(tr("The USB tunnel connection timed out."));
    });
}

Tunnel::~Tunnel()
{
    stop();
}

bool Tunnel::start(QString *error)
{
    if (!m_Config.valid()) {
        if (error != nullptr) {
            *error = tr("The USB tunnel configuration is incomplete.");
        }
        return false;
    }
    if (m_Local != nullptr || m_Remote != nullptr) {
        if (error != nullptr) {
            *error = tr("The USB tunnel is already running.");
        }
        return false;
    }

    if (m_Config.pinnedServerCertificate.isNull()) {
        /* A forwarded USB device is a high-trust channel: never fall back to
         * default CA verification. Require the cert pinned at pairing time. */
        if (error != nullptr) {
            *error = tr("Pair with this host before forwarding USB devices.");
        }
        return false;
    }

    m_Local = new QTcpSocket(this);
    m_Remote = new QSslSocket(this);
    m_Finished = false;
    m_HandshakeDone = false;
    m_PeerVerified = false;
    m_HandshakeBuffer.clear();
    m_Local->setReadBufferSize(kHighWaterMark);
    m_Remote->setReadBufferSize(kHighWaterMark);

    /* Verification is manual (mirrors nvhttp): Qt's chain/hostname checks
     * cannot express "dial by IP + trust exactly this generic-CN self-signed
     * certificate", so the handshake runs unverified and the encrypted
     * callback below rejects anything whose DER differs from the pin. */
    QSslConfiguration sslConfig = m_Config.sslConfiguration;
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    m_Remote->setSslConfiguration(sslConfig);

    connect(m_Remote, &QSslSocket::encrypted, this, [this] {
        if (m_Remote->peerCertificate() != m_Config.pinnedServerCertificate) {
            failWith(tr("The host certificate was rejected: unexpected certificate"));
            return;
        }
        m_PeerVerified = true;
        /* The handshake is the only Moonlight-owned protocol on this socket. */
        QJsonObject request {
            { QStringLiteral("op"), QStringLiteral("forward") },
            { QStringLiteral("token"),
              QString::fromUtf8(m_Config.sessionToken) },
            { QStringLiteral("busid"), QString::fromUtf8(m_Config.busId) },
        };
        QByteArray line =
            QJsonDocument(request).toJson(QJsonDocument::Compact);
        line.append('\n');
        m_Remote->write(line);
    });
    connect(m_Remote, &QSslSocket::readyRead,
            this, &Tunnel::handleRemoteReadyRead);
    connect(m_Remote, &QSslSocket::disconnected, this, [this] {
        finishCleanly();
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_Remote, &QSslSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
        failWith(tr("The connection to the host was lost: %1")
                     .arg(m_Remote->errorString()));
    });
#else
    connect(m_Remote,
            QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        failWith(tr("The connection to the host was lost: %1")
                     .arg(m_Remote->errorString()));
    });
#endif
    connect(m_Remote,
            QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
            this, [this](const QList<QSslError> &errors) {
        // Pairing pins the exact certificate, independent of hostname/CA
        // validity. Never ignore errors for a different peer certificate.
        if (m_Remote->peerCertificate() == m_Config.pinnedServerCertificate) {
            m_Remote->ignoreSslErrors(errors);
            return;
        }
        QStringList messages;
        for (const QSslError &sslError : errors) {
            messages.append(sslError.errorString());
        }
        failWith(tr("The host certificate was rejected: %1")
                     .arg(messages.join(QStringLiteral("; "))));
    });
    /* Drain the peer once our queue empties so the pump resumes after a
     * high-water pause. */
    connect(m_Remote, &QSslSocket::bytesWritten, this,
            [this](qint64) { handleLocalReadyRead(); });

    connect(m_Local, &QTcpSocket::readyRead,
            this, &Tunnel::handleLocalReadyRead);
    connect(m_Local, &QTcpSocket::connected,
            this, &Tunnel::handleRemoteReadyRead);
    connect(m_Local, &QTcpSocket::disconnected, this, [this] {
        finishCleanly();
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_Local, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
        failWith(tr("The local USB service connection failed: %1")
                     .arg(m_Local->errorString()));
    });
#else
    connect(m_Local,
            QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        failWith(tr("The local USB service connection failed: %1")
                     .arg(m_Local->errorString()));
    });
#endif
    connect(m_Local, &QTcpSocket::bytesWritten, this,
            [this](qint64) { handleRemoteReadyRead(); });

    m_Local->connectToHost(m_Config.localHost, m_Config.localPort);
    m_Remote->connectToHostEncrypted(m_Config.host, m_Config.port);
    m_StartupTimer->start(kStartupTimeoutMs);
    return true;
}

void Tunnel::stop() noexcept
{
    m_Finished = true;
    m_StartupTimer->stop();
    if (m_Local != nullptr) {
        m_Local->disconnect(this);
        m_Local->abort();
        m_Local->deleteLater();
        m_Local = nullptr;
    }
    if (m_Remote != nullptr) {
        m_Remote->disconnect(this);
        m_Remote->abort();
        m_Remote->deleteLater();
        m_Remote = nullptr;
    }
}

void Tunnel::handleRemoteReadyRead()
{
    if (m_Finished || !m_PeerVerified || m_Remote == nullptr || m_Local == nullptr ||
        m_Local->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    if (!m_HandshakeDone) {
        m_HandshakeBuffer.append(m_Remote->readAll());
        const qsizetype newline = m_HandshakeBuffer.indexOf('\n');
        if (newline < 0) {
            if (m_HandshakeBuffer.size() > kMaxHandshakeBytes) {
                failWith(tr("The host sent an invalid USB tunnel response."));
            }
            return;
        }
        if (newline > kMaxHandshakeBytes) {
            /* Content alone exceeds the mirrored server limit; the server
             * would have closed at the same boundary. */
            failWith(tr("The host sent an invalid USB tunnel response."));
            return;
        }
        if (newline > kMaxHandshakeBytes) {
            failWith(tr("The host sent an invalid USB tunnel response."));
            return;
        }
        const QByteArray line = m_HandshakeBuffer.left(newline);
        m_HandshakeBuffer.remove(0, newline + 1);

        const QJsonObject reply =
            QJsonDocument::fromJson(line).object();
        const QString op = reply.value(QStringLiteral("op")).toString();
        if (op != QStringLiteral("ready")) {
            const QString reason =
                reply.value(QStringLiteral("reason")).toString();
            failWith(reason.isEmpty()
                         ? tr("The host could not start USB forwarding.")
                         : tr("The host could not start USB forwarding: %1")
                               .arg(reason));
            return;
        }
        m_HandshakeDone = true;
        m_StartupTimer->stop();
        emit forwarding();
        if (m_Finished) return;
        /* Residual bytes past the handshake line stay in m_HandshakeBuffer and
         * are flushed through the bounded pump below — never written directly,
         * so kHighWaterMark always applies. */
    }

    /* Bounded opaque byte pump: host -> local USB/IP server. Handshake
     * residual bytes and fresh socket reads share the same high-water limit;
     * m_Local's bytesWritten signal resumes this pump after a pause. */
    while (!m_Finished && m_Local && m_Remote && m_Local->bytesToWrite() < kHighWaterMark) {
        QByteArray chunk;
        if (!m_HandshakeBuffer.isEmpty()) {
            chunk = m_HandshakeBuffer.left(64 * 1024);
            m_HandshakeBuffer.remove(0, chunk.size());
        } else if (m_Remote->bytesAvailable() > 0) {
            chunk = m_Remote->read(64 * 1024);
        } else {
            break;
        }
        if (chunk.isEmpty()) {
            break;
        }
        m_Local->write(chunk);
    }

    /* Drain anything the local USB/IP server queued before we were ready. */
    if (!m_Finished && m_Local && m_HandshakeBuffer.isEmpty() && m_Local->bytesToWrite() < kHighWaterMark) {
        handleLocalReadyRead();
    }
}

void Tunnel::handleLocalReadyRead()
{
    if (m_Remote == nullptr || m_Local == nullptr || !m_HandshakeDone) {
        /* Hold local data until the authenticated tunnel is ready. USB/IP
         * import must flow before the host's attach command can complete. */
        return;
    }

    /* Opaque byte pump: local USB/IP server -> host. */
    while (!m_Finished && m_Local && m_Remote && m_Local->bytesAvailable() > 0 &&
           m_Remote->bytesToWrite() < kHighWaterMark) {
        const QByteArray chunk = m_Local->read(64 * 1024);
        if (chunk.isEmpty()) {
            break;
        }
        m_Remote->write(chunk);
    }
}

void Tunnel::failWith(const QString &message)
{
    if (m_Finished) {
        return;
    }
    stop();
    emit finished(message);
}

void Tunnel::finishCleanly()
{
    if (m_Finished) {
        return;
    }
    if (!m_HandshakeDone) {
        failWith(tr("The USB tunnel closed before the connection was ready."));
        return;
    }
    stop();
    emit finished(QString());
}

} // namespace UsbForwarding
