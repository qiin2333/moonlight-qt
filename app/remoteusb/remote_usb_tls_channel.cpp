#include "remote_usb_tls_channel.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QSslError>
#include <QSslKey>

#include <algorithm>
#include <utility>

namespace RemoteUsb {

namespace {

bool sameCertificate(const QSslCertificate &left,
                     const QSslCertificate &right) noexcept
{
    return !left.isNull() && !right.isNull() && left.toDer() == right.toDer();
}

bool isPinnedTrustError(const QSslError &error,
                        const QSslCertificate &pinned) noexcept
{
    if (!sameCertificate(error.certificate(), pinned)) {
        return false;
    }
    switch (error.error()) {
    case QSslError::UnableToGetLocalIssuerCertificate:
    case QSslError::UnableToVerifyFirstCertificate:
    case QSslError::CertificateUntrusted:
    case QSslError::SelfSignedCertificate:
        return true;
    default:
        /* In particular, never bypass hostname, validity, or revocation
         * errors merely because the leaf happens to be pinned. */
        return false;
    }
}

bool validHost(const QString &host)
{
    const QString value = host.trimmed();
    if (value.isEmpty() || value.size() > 255 ||
        value.contains(QLatin1Char('/')) || value.contains(QLatin1Char('\\')) ||
        value.contains(QLatin1Char('?')) || value.contains(QLatin1Char('#'))) {
        return false;
    }
    for (const QChar character : value) {
        if (character.isSpace() || character.unicode() < 0x20u) {
            return false;
        }
    }
    if (value == QStringLiteral(".") || value == QStringLiteral("..")) {
        return false;
    }
    return true;
}

} // namespace

RemoteUsbTlsChannel::RemoteUsbTlsChannel(RemoteUsbTlsChannelConfig config,
                                         QObject *parent)
    : QObject(parent),
      m_config(std::move(config)),
      m_readTimer()
{
    m_readTimer.setSingleShot(true);
    connect(&m_readTimer, &QTimer::timeout,
            this, &RemoteUsbTlsChannel::handleReadTimeout);
}

RemoteUsbTlsChannel::~RemoteUsbTlsChannel()
{
    if (QThread::currentThread() == thread()) {
        close();
    } else if (m_socket != nullptr || m_started) {
        qWarning("RemoteUsbTlsChannel destroyed from a non-owner thread");
    }
}

ChannelCapabilities RemoteUsbTlsChannel::capabilities() const noexcept
{
    return ChannelCapabilities { kWireProtocolVersion, true, true };
}

void RemoteUsbTlsChannel::setCallbacks(BytesCallback bytesCallback,
                                       ErrorCallback errorCallback,
                                       ClosedCallback closedCallback)
{
    if (QThread::currentThread() != thread()) {
        qWarning("RemoteUsbTlsChannel::setCallbacks called off owner thread");
        return;
    }
    /* The binding clears callbacks after the closed notification.  Permit
     * that one cleanup operation even though the terminal channel state is
     * still represented by m_closing. */
    if (m_started || m_starting || (m_closing && !m_closedNotified)) {
        qWarning("RemoteUsbTlsChannel callbacks must be installed before start");
        return;
    }
    m_bytesCallback = std::move(bytesCallback);
    m_errorCallback = std::move(errorCallback);
    m_closedCallback = std::move(closedCallback);
}

bool RemoteUsbTlsChannel::onOwnerThread(QString *error) const
{
    if (QThread::currentThread() == thread()) {
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral("Remote USB TLS channel must run on its owner thread");
    }
    return false;
}

bool RemoteUsbTlsChannel::fail(const QString &message, QString *error)
{
    if (error != nullptr) {
        *error = message;
    }
    notifyError(message);
    /* A failed handshake/configuration is terminal for this channel.  If a
     * socket exists, drive it through the same close/quiescence path used by
     * the normal session stop. */
    if (m_socket != nullptr) {
        close();
    }
    return false;
}

bool RemoteUsbTlsChannel::start(QString *error)
{
    if (!onOwnerThread(error)) {
        return false;
    }
    if (m_started) {
        return true;
    }
    if (m_starting) {
        return fail(QStringLiteral("Remote USB TLS channel is already starting"),
                    error);
    }
    if (m_closing || m_closedNotified) {
        return fail(QStringLiteral("Remote USB TLS channel is closing"), error);
    }
    if (!validHost(m_config.host) || m_config.port == 0 ||
        m_config.connectTimeoutMs <= 0 || m_config.connectTimeoutMs > 120000 ||
        m_config.readTimeoutMs <= 0 || m_config.readTimeoutMs > 300000 ||
        m_config.maxQueuedBytes <= 0 ||
        m_config.maxQueuedBytes > 64 * 1024 * 1024 ||
        m_config.maxReadChunkBytes <= 0 ||
        static_cast<std::size_t>(m_config.maxReadChunkBytes) > kWireMaxFrameSize) {
        return fail(QStringLiteral("Remote USB TLS channel configuration is invalid"),
                    error);
    }
    if (m_config.sslConfiguration.localCertificate().isNull() ||
        m_config.sslConfiguration.privateKey().isNull()) {
        return fail(QStringLiteral("Remote USB TLS channel requires a client certificate"),
                    error);
    }
    /* Pinning is an additional identity check; it never turns off normal
     * certificate/hostname verification. */
    if (m_config.sslConfiguration.peerVerifyMode() == QSslSocket::VerifyNone) {
        return fail(QStringLiteral("Remote USB TLS channel has no server verification"),
                    error);
    }

    m_starting = true;
    m_closing = false;
    m_closedNotified = false;
    m_errorNotified = false;
    m_finishPending = false;
    m_finishScheduled = false;
    m_writeQueue.clear();
    m_writeOffset = 0;
    m_readTimer.stop();
    m_socket = new QSslSocket(this);
    m_socket->setSslConfiguration(m_config.sslConfiguration);
    m_socket->setPeerVerifyName(m_config.host);
    m_socket->setReadBufferSize(
        static_cast<qint64>(std::max<qsizetype>(m_config.maxReadChunkBytes * 2,
                                                kWireMaxFrameSize)));

    connect(m_socket,
            &QSslSocket::sslErrors,
            this,
            [this](const QList<QSslError> &errors) {
                if (m_socket == nullptr ||
                    m_config.pinnedServerCertificate.isNull() ||
                    errors.isEmpty()) {
                    return;
                }
                QList<QSslError> trustErrors;
                for (const QSslError &error : errors) {
                    if (!isPinnedTrustError(error,
                                            m_config.pinnedServerCertificate)) {
                        return;
                    }
                    trustErrors.append(error);
                }
                if (!trustErrors.isEmpty()) {
                    m_socket->ignoreSslErrors(trustErrors);
                }
            });
    connect(m_socket, &QIODevice::readyRead,
            this, &RemoteUsbTlsChannel::handleReadyRead);
    connect(m_socket, &QIODevice::bytesWritten,
            this, &RemoteUsbTlsChannel::handleBytesWritten);
    connect(m_socket, &QAbstractSocket::disconnected,
            this, &RemoteUsbTlsChannel::handleDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &RemoteUsbTlsChannel::handleSocketError);
#else
    connect(m_socket,
            static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
                &QAbstractSocket::error),
            this, &RemoteUsbTlsChannel::handleSocketError);
#endif

    QSslSocket *socket = m_socket;
    socket->connectToHostEncrypted(m_config.host, m_config.port);
    if (!socket->waitForEncrypted(m_config.connectTimeoutMs)) {
        /* waitForEncrypted() may dispatch socket signals synchronously.  A
         * disconnected/error callback can therefore have already finalized
         * and nulled m_socket; never dereference it after that point. */
        const QString socketError = m_socket == socket
            ? socket->errorString() : QString();
        const QString message = socketError.isEmpty()
            ? QStringLiteral("Remote USB TLS handshake timed out")
            : QStringLiteral("Remote USB TLS handshake failed: %1").arg(socketError);
        m_starting = false;
        return fail(message, error);
    }

    if (m_socket != socket || m_closedNotified || m_closing ||
        !socket->isEncrypted()) {
        m_starting = false;
        return fail(QStringLiteral("Remote USB TLS channel closed during handshake"),
                    error);
    }
    const QSslCertificate peer = socket->peerCertificate();
    if (peer.isNull() ||
        (!m_config.pinnedServerCertificate.isNull() &&
         !sameCertificate(peer, m_config.pinnedServerCertificate))) {
        m_starting = false;
        return fail(QStringLiteral("Remote USB broker certificate mismatch"),
                    error);
    }

    m_starting = false;
    m_closing = false;
    m_closedNotified = false;
    m_errorNotified = false;
    m_started = true;
    m_readTimer.start(m_config.readTimeoutMs);
    return true;
}

bool RemoteUsbTlsChannel::send(const QByteArray &bytes, QString *error)
{
    if (!onOwnerThread(error)) {
        return false;
    }
    if (bytes.isEmpty()) {
        return true;
    }
    if (!m_started || m_closing || m_socket == nullptr ||
        !m_socket->isEncrypted()) {
        return fail(QStringLiteral("Remote USB TLS channel is not open"), error);
    }
    const qint64 socketPending = m_socket->bytesToWrite();
    const qsizetype localPending = m_writeQueue.size() - m_writeOffset;
    if (socketPending < 0 || localPending < 0 ||
        socketPending > m_config.maxQueuedBytes ||
        localPending > m_config.maxQueuedBytes - socketPending ||
        bytes.size() > m_config.maxQueuedBytes - socketPending - localPending) {
        return fail(QStringLiteral("Remote USB TLS channel output window is full"),
                    error);
    }
    try {
        /* Copy into our bounded queue first.  QIODevice::write() is allowed to
         * accept a short prefix; retaining the remainder here makes the
         * boundary's successful-send contract genuinely all-or-nothing. */
        if (m_writeOffset != 0 &&
            (m_writeOffset == m_writeQueue.size() || m_writeOffset > 64 * 1024)) {
            m_writeQueue.remove(0, m_writeOffset);
            m_writeOffset = 0;
        }
        m_writeQueue.append(bytes);
        flushWriteQueue();
        return true;
    } catch (...) {
        return fail(QStringLiteral("Remote USB TLS channel output queue failed"),
                    error);
    }
}

void RemoteUsbTlsChannel::close() noexcept
{
    try {
        if (QThread::currentThread() != thread()) {
            QPointer<RemoteUsbTlsChannel> guard(this);
            QMetaObject::invokeMethod(
                this,
                [guard]() {
                    if (guard) {
                        guard->close();
                    }
                },
                Qt::QueuedConnection);
            return;
        }
        if (m_closedNotified) {
            return;
        }
        m_closing = true;
        m_started = false;
        m_starting = false;
        m_readTimer.stop();
        if (m_socket == nullptr ||
            m_socket->state() == QAbstractSocket::UnconnectedState) {
            finishClosed();
            return;
        }
        m_socket->disconnectFromHost();
        if (m_socket->state() == QAbstractSocket::UnconnectedState) {
            finishClosed();
        }
    } catch (...) {
        /* The byte-channel boundary is noexcept.  Ensure the closed callback
         * is still attempted after a failed socket operation. */
        try {
            finishClosed();
        } catch (...) {
        }
    }
}

bool RemoteUsbTlsChannel::isOpen() const noexcept
{
    return m_started && !m_closing && m_socket != nullptr &&
           m_socket->isEncrypted();
}

void RemoteUsbTlsChannel::flushWriteQueue()
{
    if (m_flushingWrites || m_socket == nullptr || m_closing ||
        !m_socket->isEncrypted()) {
        return;
    }
    m_flushingWrites = true;
    while (m_writeOffset < m_writeQueue.size() && !m_closing &&
           m_socket != nullptr && m_socket->isEncrypted()) {
        const qint64 remaining = m_writeQueue.size() - m_writeOffset;
        const qint64 written = m_socket->write(
            m_writeQueue.constData() + m_writeOffset, remaining);
        if (written <= 0) {
            /* Keep the unsent suffix for a bytesWritten retry.  The channel is
             * already transitioning to close on an actual socket error. */
            if (written < 0) {
                notifyError(QStringLiteral("Remote USB TLS channel write failed"));
            }
            break;
        }
        m_writeOffset += static_cast<qsizetype>(written);
    }
    if (m_writeOffset == m_writeQueue.size()) {
        m_writeQueue.clear();
        m_writeOffset = 0;
    } else if (m_writeOffset > 64 * 1024) {
        m_writeQueue.remove(0, m_writeOffset);
        m_writeOffset = 0;
    }
    m_flushingWrites = false;
}

void RemoteUsbTlsChannel::handleBytesWritten(qint64 bytes)
{
    Q_UNUSED(bytes);
    flushWriteQueue();
}

void RemoteUsbTlsChannel::handleReadyRead()
{
    if (m_socket == nullptr || m_closing) {
        return;
    }
    while (m_socket->bytesAvailable() > 0 && !m_closing) {
        const qint64 available = m_socket->bytesAvailable();
        const qint64 chunkSize = std::min<qint64>(
            available, static_cast<qint64>(m_config.maxReadChunkBytes));
        QByteArray bytes = m_socket->read(chunkSize);
        if (bytes.isEmpty()) {
            break;
        }
        if (!m_bytesCallback) {
            notifyError(QStringLiteral("Remote USB TLS channel has no read callback"));
            return;
        }
        m_inCallback = true;
        try {
            m_bytesCallback(std::move(bytes));
        } catch (...) {
            m_inCallback = false;
            notifyError(QStringLiteral("Remote USB read callback failed"));
            return;
        }
        m_inCallback = false;
        if (!m_closing) {
            m_readTimer.start(m_config.readTimeoutMs);
        }
    }
}

void RemoteUsbTlsChannel::handleDisconnected()
{
    if (!m_closing && !m_errorNotified) {
        notifyError(QStringLiteral("Remote USB TLS channel disconnected"));
    }
    finishClosed();
}

void RemoteUsbTlsChannel::handleSocketError()
{
    if (!m_closing && m_socket != nullptr) {
        const QString detail = m_socket->errorString();
        notifyError(detail.isEmpty() ? QStringLiteral("Remote USB TLS socket error")
                                    : QStringLiteral("Remote USB TLS socket error: %1")
                                          .arg(detail));
    }
}

void RemoteUsbTlsChannel::handleReadTimeout()
{
    if (!m_started || m_closing) {
        return;
    }
    notifyError(QStringLiteral("Remote USB TLS channel read timed out"));
}

void RemoteUsbTlsChannel::notifyError(const QString &message)
{
    if (m_errorNotified) {
        return;
    }
    m_errorNotified = true;
    /* Mark the channel terminal before entering user code.  An error handler
     * may synchronously call send()/close(); neither should observe an open
     * socket while the error callback is running. */
    m_closing = true;
    m_started = false;
    m_starting = false;
    m_readTimer.stop();
    if (m_errorCallback) {
        m_inCallback = true;
        try {
            m_errorCallback(message);
        } catch (...) {
        }
        m_inCallback = false;
    }
    if (m_socket != nullptr) {
        m_socket->abort();
    } else {
        finishClosed();
    }
}

void RemoteUsbTlsChannel::finishClosed()
{
    if (m_closedNotified) {
        return;
    }
    if (m_inCallback) {
        m_finishPending = true;
        scheduleFinishClosed();
        return;
    }
    if (m_socket != nullptr &&
        m_socket->state() != QAbstractSocket::UnconnectedState) {
        return;
    }
    m_finishPending = false;
    m_started = false;
    m_starting = false;
    m_closing = true;
    m_readTimer.stop();
    m_closedNotified = true;
    ClosedCallback callback = std::move(m_closedCallback);
    m_bytesCallback = {};
    m_errorCallback = {};
    m_writeQueue.clear();
    m_writeOffset = 0;
    if (m_socket != nullptr) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (callback) {
        m_inCallback = true;
        try {
            callback();
        } catch (...) {
        }
        m_inCallback = false;
    }
    if (m_finishPending) {
        scheduleFinishClosed();
    }
}

void RemoteUsbTlsChannel::scheduleFinishClosed()
{
    if (m_finishScheduled || m_closedNotified) {
        return;
    }
    m_finishScheduled = true;
    QPointer<RemoteUsbTlsChannel> guard(this);
    try {
        if (!QMetaObject::invokeMethod(
                this,
                [guard]() {
                    if (!guard) {
                        return;
                    }
                    guard->m_finishScheduled = false;
                    guard->finishClosed();
                },
                Qt::QueuedConnection)) {
            m_finishScheduled = false;
        }
    } catch (...) {
        m_finishScheduled = false;
    }
}

} // namespace RemoteUsb
