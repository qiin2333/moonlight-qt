#pragma once

/*
 * Reverse USB/IP tunnel client.
 *
 * One tunnel carries one USB device. The client owns two sockets and copies
 * bytes between them without interpreting a single USB/IP byte:
 *
 *   local  : TCP to the platform USB/IP server (usbipd-win on 3240 on
 *            Windows; the per-session moonlight-usbd helper on an
 *            ephemeral loopback port on macOS; the distro usbipd daemon
 *            on 3240 on Linux)
 *   remote : TLS to Sunshine, authenticated with the paired client
 *            certificate, carrying the configured shared token
 *
 * Transport sockets run on a dedicated worker thread with its own event
 * loop, independent of the Session/GUI loop: a forwarded 1000 Hz mouse
 * produces a continuous URB stream, and pumping it on the Session loop
 * starves input processing (measured in #241/#242 — aldobarr's mouse
 * latency reports). Notifications are relayed back to the owner thread.
 *
 * Session owns the tunnel and closes it when streaming ends. The tunnel uses
 * a separate socket from video/audio/control; its port and token come from
 * the GET /api/v1/usb-forwarding capability endpoint.
 *
 * Only the handshake is Moonlight's own protocol: one line of JSON in each
 * direction. Everything after that is an opaque byte stream.
 */

#include <QObject>
#include <QSslConfiguration>
#include <QString>
#include <memory>

namespace UsbForwarding {

struct TunnelConfig
{
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

    // Call on this object's owning thread. Notifications return to that
    // thread; sockets and timers are serviced by a private worker thread.
    bool start(QString *error = nullptr);
    // Closes the transport and joins its worker before returning.
    void stop() noexcept;

signals:
    /* The authenticated byte tunnel is ready for USB/IP import/enumeration. */
    void forwarding();
    /* Terminal: the tunnel is done. message is empty on a clean stop. */
    void finished(QString message);

private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace UsbForwarding
