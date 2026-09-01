#pragma once

#include "remote_usb_platform_adapter.h"

#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>

namespace RemoteUsb {

inline constexpr qsizetype kRemoteUsbDefaultChannelQueueBytes = 4 * 1024 * 1024;
/* Keep one read callback below the RUSB frame ceiling.  The channel delivers
 * arbitrary byte chunks, but accepting a chunk larger than the parser's
 * bounded frame would make the default configuration fail at startup. */
inline constexpr qsizetype kRemoteUsbDefaultChannelReadChunkBytes = 128 * 1024;

/*
 * Configuration for the independent broker TLS stream.  The SSL configuration
 * must contain the paired client certificate and private key.  A pinned
 * server certificate is preferred; when it is absent, normal Qt hostname/CA
 * verification remains mandatory.
 */
struct RemoteUsbTlsChannelConfig {
    QString host;
    quint16 port = 0;
    QSslConfiguration sslConfiguration;
    QSslCertificate pinnedServerCertificate;
    int connectTimeoutMs = 5000;
    int readTimeoutMs = 15000;
    qsizetype maxQueuedBytes = kRemoteUsbDefaultChannelQueueBytes;
    qsizetype maxReadChunkBytes = kRemoteUsbDefaultChannelReadChunkBytes;
};

/*
 * Qt implementation of the platform-neutral byte-channel boundary.
 *
 * The object is owner-thread affine.  `start()` performs the TLS handshake
 * synchronously (call it from a session worker, not the GUI thread), while
 * readyRead callbacks are delivered by the socket's event loop.  Outbound
 * writes are bounded by maxQueuedBytes and are accepted only when QSslSocket
 * queues the complete byte array.
 */
class RemoteUsbTlsChannel final : public QObject, public RemoteUsbByteChannel
{
public:
    explicit RemoteUsbTlsChannel(RemoteUsbTlsChannelConfig config,
                                 QObject *parent = nullptr);
    ~RemoteUsbTlsChannel() override;

    Q_DISABLE_COPY(RemoteUsbTlsChannel)

    ChannelCapabilities capabilities() const noexcept override;
    void setCallbacks(BytesCallback bytesCallback,
                      ErrorCallback errorCallback,
                      ClosedCallback closedCallback) override;
    bool start(QString *error = nullptr) override;
    bool send(const QByteArray &bytes, QString *error = nullptr) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;

private:
    bool onOwnerThread(QString *error = nullptr) const;
    bool fail(const QString &message, QString *error = nullptr);
    void handleReadyRead();
    void handleBytesWritten(qint64 bytes);
    void handleDisconnected();
    void handleSocketError();
    void handleReadTimeout();
    void flushWriteQueue();
    void finishClosed();
    void scheduleFinishClosed();
    void notifyError(const QString &message);

    RemoteUsbTlsChannelConfig m_config;
    QSslSocket *m_socket = nullptr;
    QTimer m_readTimer;
    BytesCallback m_bytesCallback;
    ErrorCallback m_errorCallback;
    ClosedCallback m_closedCallback;
    QByteArray m_writeQueue;
    qsizetype m_writeOffset = 0;
    bool m_started = false;
    bool m_starting = false;
    bool m_closing = false;
    bool m_closedNotified = false;
    bool m_errorNotified = false;
    bool m_inCallback = false;
    bool m_finishPending = false;
    bool m_finishScheduled = false;
    bool m_flushingWrites = false;
};

} // namespace RemoteUsb
