#pragma once

/*
 * Reverse USB/IP tunnel client.
 *
 * One tunnel carries one USB device. The client owns two sockets and copies
 * bytes between them without interpreting a single USB/IP byte:
 *
 *   local  : TCP to the platform USB/IP server on 127.0.0.1:3240
 *            (usbipd-win on Windows, usbip-host on Linux,
 *             USBIPServerForAndroid on Android)
 *   remote : TLS to Sunshine, authenticated with the paired client
 *            certificate, carrying the configured shared token
 *
 * Session owns the tunnel and closes it when streaming ends. The tunnel uses
 * a separate socket from video/audio/control; its port and token currently
 * come from environment overrides, not RTSP negotiation.
 *
 * Only the handshake is Moonlight's own protocol: one line of JSON in each
 * direction. Everything after that is an opaque byte stream.
 */

#include <QObject>
#include <QSslConfiguration>
#include <QString>

class QSslSocket;
class QTcpSocket;
class QTimer;

namespace UsbForwarding {

struct TunnelConfig {
    /* Configured Sunshine USB endpoint. */
    QString host;
    quint16 port = 0;
    /* Shared token configured on both ends; not yet a per-session credential. */
    QByteArray sessionToken;
    /* Paired client certificate/key plus the pinned server certificate. */
    QSslConfiguration sslConfiguration;
    QSslCertificate pinnedServerCertificate;

    /* Bus id of the local USB/IP server, forwarded to Sunshine verbatim. */
    QByteArray busId;

    /* Local USB/IP server endpoint. */
    QString localHost = QStringLiteral("127.0.0.1");
    quint16 localPort = 3240;

    bool valid() const noexcept;
};

class Tunnel final : public QObject
{
    Q_OBJECT

public:
    explicit Tunnel(TunnelConfig config, QObject *parent = nullptr);
    ~Tunnel() override;

    Q_DISABLE_COPY(Tunnel)

    bool start(QString *error = nullptr);
    void stop() noexcept;

signals:
    /* The authenticated byte tunnel is ready for USB/IP import/enumeration. */
    void forwarding();
    /* Terminal: the tunnel is done. message is empty on a clean stop. */
    void finished(QString message);

private:
    void handleRemoteReadyRead();
    void handleLocalReadyRead();
    void failWith(const QString &message);
    void finishCleanly();

    TunnelConfig m_Config;
    QTcpSocket *m_Local = nullptr;
    QSslSocket *m_Remote = nullptr;
    QTimer *m_StartupTimer = nullptr;
    QByteArray m_HandshakeBuffer;
    bool m_HandshakeDone = false;
    bool m_PeerVerified = false;
    bool m_Finished = false;
};

} // namespace UsbForwarding
