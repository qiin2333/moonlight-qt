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
 *            certificate, carrying the stream session token
 *
 * The tunnel is a fourth stream of the streaming session (video / audio /
 * control / usb): it inherits the session identity, the negotiated port and the
 * session lifetime, but keeps its own socket so USB traffic can never
 * head-of-line block input on the control stream.
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
    /* Sunshine endpoint negotiated for this session's USB stream. */
    QString host;
    quint16 port = 0;
    /* One-shot token bound to the current streaming session. */
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

    QByteArray busId() const noexcept { return m_Config.busId; }
    bool isForwarding() const noexcept { return m_Forwarding; }

    bool start(QString *error = nullptr);
    void stop() noexcept;

signals:
    /* Sunshine accepted the handshake and attached the device. */
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
    bool m_Forwarding = false;
    bool m_Finished = false;
};

} // namespace UsbForwarding
