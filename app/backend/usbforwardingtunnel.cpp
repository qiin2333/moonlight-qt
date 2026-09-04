#include "usbforwardingtunnel.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSslSocket>
#include <QTimer>
#include <QTcpSocket>

namespace UsbForwarding {

namespace {
/* Bound the handshake line so a hostile peer cannot grow the buffer. */
constexpr qsizetype kMaxHandshakeBytes = 4 * 1024;
/* Stop reading from one side while the other side is this far behind. TCP
 * back-pressures the USB/IP peer instead of us buffering without limit. */
constexpr qint64 kHighWaterMark = 4 * 1024 * 1024;
/* Deadline covering TCP connect, TLS, and the JSON handshake. Without it a
 * reachable-but-stalled endpoint would pin the session in "Connecting". */
constexpr int kStartupTimeoutMs = 10 * 1000;
} // namespace

bool TunnelConfig::valid() const noexcept
{
    return !host.isEmpty() && port != 0 && !sessionToken.isEmpty() &&
           !busId.isEmpty() && localPort != 0;
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

    /* Trust exactly the pinned certificate — not the system CA set. */
    QSslConfiguration sslConfig = m_Config.sslConfiguration;
    sslConfig.setCaCertificates({m_Config.pinnedServerCertificate});
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_Remote->setSslConfiguration(sslConfig);

    connect(m_Remote, &QSslSocket::encrypted, this, [this] {
        /* The handshake is the only Moonlight-owned protocol on this socket. */
        QJsonObject request {
            { QStringLiteral("op"), QStringLiteral("forward") },
            { QStringLiteral("token"),
              QString::fromLatin1(m_Config.sessionToken) },
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
    m_Forwarding = false;
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
    if (m_Remote == nullptr || m_Local == nullptr) {
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
        const QByteArray line = m_HandshakeBuffer.left(newline);
        m_HandshakeBuffer.remove(0, newline + 1);

        const QJsonObject reply =
            QJsonDocument::fromJson(line).object();
        const QString op = reply.value(QStringLiteral("op")).toString();
        if (op != QStringLiteral("ready")) {
            const QString reason =
                reply.value(QStringLiteral("reason")).toString();
            failWith(reason.isEmpty()
                         ? tr("The host refused to forward this device.")
                         : tr("The host refused to forward this device: %1")
                               .arg(reason));
            return;
        }
        m_HandshakeDone = true;
        m_Forwarding = true;
        m_StartupTimer->stop();
        emit forwarding();
        /* Residual bytes past the handshake line stay in m_HandshakeBuffer and
         * are flushed through the bounded pump below — never written directly,
         * so kHighWaterMark always applies. */
    }

    /* Bounded opaque byte pump: host -> local USB/IP server. Handshake
     * residual bytes and fresh socket reads share the same high-water limit;
     * m_Local's bytesWritten signal resumes this pump after a pause. */
    while (m_Local->bytesToWrite() < kHighWaterMark) {
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
    if (m_HandshakeBuffer.isEmpty() && m_Local->bytesToWrite() < kHighWaterMark) {
        handleLocalReadyRead();
    }
}

void Tunnel::handleLocalReadyRead()
{
    if (m_Remote == nullptr || m_Local == nullptr || !m_HandshakeDone) {
        /* Hold local data back until Sunshine has attached the device. */
        return;
    }

    /* Opaque byte pump: local USB/IP server -> host. */
    while (m_Local->bytesAvailable() > 0 &&
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
    m_Finished = true;
    m_Forwarding = false;
    m_StartupTimer->stop();
    emit finished(message);
}

void Tunnel::finishCleanly()
{
    if (m_Finished) {
        return;
    }
    m_Finished = true;
    m_Forwarding = false;
    m_StartupTimer->stop();
    emit finished(QString());
}

} // namespace UsbForwarding
