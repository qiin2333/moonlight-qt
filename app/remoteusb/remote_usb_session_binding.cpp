#include "remote_usb_session_binding.h"

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace RemoteUsb {

namespace {

/* Keep the Qt-side parser limits tied to the C wire contract.  A silent
 * change on one side would otherwise turn a valid frame into truncation (or
 * allow the accumulator to grow beyond the negotiated ceiling). */
static_assert(kBrokerHelloSize == ML_REMOTE_USB_BROKER_HELLO_SIZE,
              "Qt/C broker HELLO sizes differ");
static_assert(kWireHeaderSize == ML_REMOTE_USB_WIRE_HEADER_SIZE,
              "Qt/C wire header sizes differ");
static_assert(kWireMaxPayload == ML_REMOTE_USB_WIRE_MAX_PAYLOAD,
              "Qt/C wire payload limits differ");
static_assert(kMaxReassemblySize == ML_REMOTE_USB_WIRE_MAX_REASSEMBLY_SIZE,
              "Qt/C reassembly limits differ");
static_assert(kMaxFragments == ML_REMOTE_USB_WIRE_MAX_FRAGMENTS,
              "Qt/C fragment limits differ");
static_assert(kPduHeaderSize == ML_REMOTE_USB_PDU_HEADER_SIZE,
              "Qt/C PDU header sizes differ");
static_assert(kMaxTransferSize == ML_REMOTE_USB_PDU_MAX_TRANSFER_SIZE,
              "Qt/C transfer limits differ");
static_assert(kBusIdMaxBytes == ML_REMOTE_USB_WIRE_MAX_BUS_ID_BYTES,
              "Qt/C bus id limits differ");
static_assert(kRawDescriptorMaxBytes ==
                  ML_REMOTE_USB_WIRE_MAX_RAW_DESCRIPTOR_SIZE,
              "Qt/C descriptor limits differ");
static_assert(kEndpointMaxCount == ML_REMOTE_USB_WIRE_MAX_ENDPOINT_COUNT,
              "Qt/C endpoint limits differ");

constexpr qint32 kRejectedStatus =
    ML_REMOTE_USB_EXECUTOR_USB_STATUS_NOT_SUPPORTED;
constexpr qint32 kCallbackFailureStatus =
    ML_REMOTE_USB_EXECUTOR_USB_STATUS_NO_DEVICE;

QString cStatusMessage(const char *operation,
                       ml_remote_usb_session_status status)
{
    return QStringLiteral("Remote USB %1 failed (status %2)")
        .arg(QString::fromLatin1(operation))
        .arg(static_cast<int>(status));
}

bool sizeFitsQByteArray(std::size_t size)
{
    return size <= static_cast<std::size_t>(std::numeric_limits<qsizetype>::max());
}

bool copyBytes(const std::uint8_t *source, std::size_t size, QByteArray *out)
{
    if (out == nullptr || !sizeFitsQByteArray(size) ||
        (size != 0 && source == nullptr)) {
        return false;
    }
    if (size == 0) {
        out->clear();
        return true;
    }
    *out = QByteArray(reinterpret_cast<const char *>(source),
                      static_cast<qsizetype>(size));
    return true;
}

bool allZero(const std::array<std::uint8_t, kSetupPacketSize> &bytes)
{
    for (const std::uint8_t byte : bytes) {
        if (byte != 0u) {
            return false;
        }
    }
    return true;
}

quint16 readLe16(const std::array<std::uint8_t, kSetupPacketSize> &bytes,
                 std::size_t offset)
{
    return static_cast<quint16>(bytes[offset]) |
           static_cast<quint16>(bytes[offset + 1u] << 8u);
}

} // namespace

struct RemoteUsbSessionBinding::SubmitGate {
    QPointer<RemoteUsbSessionBinding> binding;
    std::mutex mutex;
    bool returned = false;
    bool callbackSeen = false;
    bool duplicateCallback = false;
    bool lateCallback = false;
    std::uint64_t requestToken = 0;
    TransferRequest request;
    TransferCompletion completion;
};

struct RemoteUsbSessionBinding::CancelGate {
    QPointer<RemoteUsbSessionBinding> binding;
    std::mutex mutex;
    bool returned = false;
    bool callbackSeen = false;
    bool duplicateCallback = false;
    bool lateCallback = false;
    std::uint64_t requestToken = 0;
    std::int32_t status = kCallbackFailureStatus;
};

RemoteUsbSessionBinding::RemoteUsbSessionBinding(
    RemoteUsbPlatformAdapter *platform,
    RemoteUsbByteChannel *channel,
    const RemoteUsbSessionBindingOptions &options,
    QObject *parent)
    : QObject(parent),
      m_platform(platform),
      m_channel(channel),
      m_options(options)
{
    /* Permit callers to use a value-initialized hello while still making the
     * ABI fields explicit before it reaches the C session. */
    if (m_options.brokerHello.size == 0) {
        m_options.brokerHello.size = sizeof(m_options.brokerHello);
    }
    if (m_options.brokerHello.version == 0) {
        m_options.brokerHello.version = ML_REMOTE_USB_BROKER_VERSION;
    }
    m_payloadBuffer.reserve(static_cast<qsizetype>(kWireMaxPayload));
}

RemoteUsbSessionBinding::~RemoteUsbSessionBinding()
{
    /* Destruction cannot block an event loop.  The documented ownership rule
     * is therefore to destroy after stopped(); a synchronous, already-closed
     * channel can still finish here.  Never call release()/destroy() a second
     * time after an asynchronous stop has been requested. */
    if (QThread::currentThread() == thread()) {
        if (!m_stopped || m_session != nullptr) {
            stop();
        }
    } else if (m_session != nullptr || m_channelStarted) {
        qWarning("RemoteUsbSessionBinding destroyed from a non-owner thread");
    }

    if (m_session != nullptr) {
        qWarning("RemoteUsbSessionBinding destroyed before stopped(); C session retained");
    }

    if (m_channel != nullptr && m_channelCallbacksInstalled) {
        try {
            m_channel->setCallbacks({}, {}, {});
        } catch (...) {
            qWarning("RemoteUsbByteChannel rejected callback cleanup");
        }
    }
}

bool RemoteUsbSessionBinding::onOwnerThread(QString *error) const
{
    if (QThread::currentThread() == thread()) {
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral("Remote USB operation must run on its owner thread");
    }
    return false;
}

bool RemoteUsbSessionBinding::setError(const QString &message, QString *error)
{
    m_lastError = message;
    if (error != nullptr) {
        *error = message;
    }
    notifyError(message);
    return false;
}

void RemoteUsbSessionBinding::notifyError(const QString &message) noexcept
{
    try {
        const QString detail = message.isEmpty()
            ? QStringLiteral("Remote USB operation failed")
            : message;
        if (QThread::currentThread() != thread()) {
            /* A platform callback may arrive on an I/O thread.  Do not emit
             * a QObject signal or mutate owner-thread state from that thread. */
            enqueueError(detail);
            return;
        }
        m_lastError = detail;
        emit errorOccurred(detail);
    } catch (...) {
        /* C callbacks are noexcept; retaining the failure must not terminate
         * the process if a Qt allocation or queued invocation fails. */
    }
}

bool RemoteUsbSessionBinding::checkSession(QString *error) const
{
    if (m_session != nullptr && !m_stopped) {
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral("Remote USB session is not available");
    }
    return false;
}

bool RemoteUsbSessionBinding::checkStatus(ml_remote_usb_session_status status,
                                          const char *operation,
                                          QString *error)
{
    if (status == ML_REMOTE_USB_SESSION_OK) {
        return true;
    }
    return setError(cStatusMessage(operation, status), error);
}

RemoteUsbSessionBinding *RemoteUsbSessionBinding::fromContext(void *context) noexcept
{
    return static_cast<RemoteUsbSessionBinding *>(context);
}

bool RemoteUsbSessionBinding::createSession(QString *error)
{
    if (m_session != nullptr) {
        return true;
    }
    if (m_platform == nullptr || m_channel == nullptr) {
        return setError(QStringLiteral("Remote USB adapter and channel are required"),
                        error);
    }

    const ml_remote_usb_broker_hello &hello = m_options.brokerHello;
    if (hello.size != sizeof(hello) ||
        hello.version != ML_REMOTE_USB_BROKER_VERSION ||
        std::all_of(std::begin(hello.client_uuid), std::end(hello.client_uuid),
                    [](std::uint8_t value) { return value == 0u; }) ||
        hello.stream_generation == 0 ||
        std::all_of(std::begin(hello.capability_nonce),
                    std::end(hello.capability_nonce),
                    [](std::uint8_t value) { return value == 0u; }) ||
        hello.max_urb < ML_REMOTE_USB_BROKER_MIN_URB ||
        hello.max_urb > ML_REMOTE_USB_BROKER_MAX_PDU_SIZE ||
        hello.max_inflight == 0 ||
        hello.max_inflight > ML_REMOTE_USB_BROKER_MAX_INFLIGHT ||
        hello.session_token == 0 || hello.attachment_token == 0 ||
        hello.lease_token == 0 || hello.isochronous != 0 ||
        (m_options.maxInflight != 0 &&
         m_options.maxInflight > ML_REMOTE_USB_EXECUTOR_MAX_INFLIGHT) ||
        (m_options.maxTransferSize != 0 &&
         m_options.maxTransferSize > ML_REMOTE_USB_EXECUTOR_MAX_TRANSFER_SIZE) ||
        (m_options.maxReassemblySize != 0 &&
         m_options.maxReassemblySize > ML_REMOTE_USB_WIRE_MAX_REASSEMBLY_SIZE) ||
        (m_options.maxFragments != 0 &&
         m_options.maxFragments > ML_REMOTE_USB_WIRE_MAX_FRAGMENTS) ||
        (m_options.txWindowBytes != 0 &&
         m_options.txWindowBytes > ML_REMOTE_USB_BROKER_MAX_WINDOW_BYTES) ||
        (m_options.rxWindowBytes != 0 &&
         m_options.rxWindowBytes > ML_REMOTE_USB_BROKER_MAX_WINDOW_BYTES) ||
        (m_options.txWindowPdus != 0 &&
         m_options.txWindowPdus > ML_REMOTE_USB_BROKER_MAX_WINDOW_PDUS) ||
        (m_options.rxWindowPdus != 0 &&
         m_options.rxWindowPdus > ML_REMOTE_USB_BROKER_MAX_WINDOW_PDUS)) {
        return setError(QStringLiteral("Remote USB broker HELLO configuration is invalid"),
                        error);
    }

    ml_remote_usb_executor_callbacks executorCallbacks {};
    executorCallbacks.size = sizeof(executorCallbacks);
    executorCallbacks.version = ML_REMOTE_USB_EXECUTOR_ABI_VERSION;
    executorCallbacks.submit_control = &RemoteUsbSessionBinding::submitControl;
    executorCallbacks.submit_data = &RemoteUsbSessionBinding::submitData;
    executorCallbacks.cancel = &RemoteUsbSessionBinding::cancel;
    executorCallbacks.resolve_endpoint = &RemoteUsbSessionBinding::resolveEndpoint;

    ml_remote_usb_transport_callbacks transportCallbacks {};
    transportCallbacks.size = sizeof(transportCallbacks);
    transportCallbacks.version = ML_REMOTE_USB_TRANSPORT_ABI_VERSION;
    transportCallbacks.send_hello = &RemoteUsbSessionBinding::sendHello;
    transportCallbacks.send_frame = &RemoteUsbSessionBinding::sendFrame;
    transportCallbacks.on_capability = &RemoteUsbSessionBinding::onCapability;
    transportCallbacks.on_open = &RemoteUsbSessionBinding::onOpen;
    transportCallbacks.on_open_ok = &RemoteUsbSessionBinding::onOpenOk;
    transportCallbacks.on_open_reject = &RemoteUsbSessionBinding::onOpenReject;
    transportCallbacks.on_pdu = &RemoteUsbSessionBinding::onPdu;
    transportCallbacks.on_close = &RemoteUsbSessionBinding::onClose;
    transportCallbacks.on_error = &RemoteUsbSessionBinding::onCoreError;
    transportCallbacks.on_stopped = &RemoteUsbSessionBinding::onCoreStopped;

    ml_remote_usb_session_config config {};
    config.size = sizeof(config);
    config.version = ML_REMOTE_USB_SESSION_ABI_VERSION;
    config.executor.size = sizeof(config.executor);
    config.executor.version = ML_REMOTE_USB_EXECUTOR_ABI_VERSION;
    config.executor.max_inflight = m_options.maxInflight;
    config.executor.max_transfer_size = m_options.maxTransferSize;
    config.transport.size = sizeof(config.transport);
    config.transport.version = ML_REMOTE_USB_TRANSPORT_ABI_VERSION;
    config.transport.hello = hello;
    config.transport.tx_window_bytes = m_options.txWindowBytes;
    config.transport.tx_window_pdus = m_options.txWindowPdus;
    config.transport.rx_window_bytes = m_options.rxWindowBytes;
    config.transport.rx_window_pdus = m_options.rxWindowPdus;
    config.transport.max_reassembly_size = m_options.maxReassemblySize;
    config.transport.max_fragments = m_options.maxFragments;
    config.executor_callbacks = &executorCallbacks;
    config.transport_callbacks = &transportCallbacks;
    config.context = this;

    const ml_remote_usb_session_status status =
        ml_remote_usb_session_create(&config, &m_session);
    if (status != ML_REMOTE_USB_SESSION_OK) {
        m_session = nullptr;
        return checkStatus(status, "create", error);
    }
    return true;
}

void RemoteUsbSessionBinding::installChannelCallbacks()
{
    QPointer<RemoteUsbSessionBinding> guard(this);
    m_channel->setCallbacks(
        [guard](QByteArray bytes) mutable {
            if (!guard) {
                return;
            }
            guard->enqueueBytes(std::move(bytes));
        },
        [guard](QString message) mutable {
            if (!guard) {
                return;
            }
            guard->enqueueError(std::move(message));
        },
        [guard]() {
            if (!guard) {
                return;
            }
            guard->enqueueClosed();
        });
    m_channelCallbacksInstalled = true;
}

bool RemoteUsbSessionBinding::start(QString *error)
{
    if (!onOwnerThread(error)) {
        return false;
    }
    if (m_started) {
        return true;
    }
    if (m_startAttempted) {
        return setError(QStringLiteral(
                            "Remote USB session binding is single-use; create a new binding to reconnect"),
                        error);
    }
    if (m_stopping) {
        return setError(QStringLiteral("Remote USB session is stopping"), error);
    }
    if (m_channel == nullptr || m_platform == nullptr) {
        return setError(QStringLiteral("Remote USB adapter and channel are required"),
                        error);
    }
    if (!m_channel->capabilities().usable()) {
        return setError(QStringLiteral("Remote USB channel is not authenticated or compatible"),
                        error);
    }

    try {
        m_startAttempted = true;
        m_failed = false;
        m_stopped = false;
        m_stopping = false;
        m_coreStopDone = false;
        m_releaseCalled = false;
        m_channelCloseRequested = false;
        m_stoppedSignalEmitted = false;
        m_channelClosed = true;
        m_helloAccepted = false;
        m_helloOffset = 0;
        m_headerOffset = 0;
        m_payloadOffset = 0;
        m_expectedPayload = 0;
        m_payloadBuffer.clear();
        if (!createSession(error)) {
            return false;
        }
        installChannelCallbacks();
        m_channelClosed = false;
        if (!m_channel->start(error)) {
            m_stopping = true;
            m_channelCloseRequested = true;
            m_channel->close();
            if (m_channelClosed) {
                finishStop();
            } else {
                scheduleStopRetry();
            }
            return false;
        }
        m_channelStarted = true;

        /* A channel implementation may report a synchronous close while
         * completing start(). Never enter the C transport on a closed
         * channel; use the normal teardown path instead. */
        if (m_channelClosed) {
            m_stopping = true;
            m_channelCloseRequested = true;
            finishStop();
            return setError(QStringLiteral("Remote USB channel closed during startup"),
                            error);
        }

        const ml_remote_usb_session_status status =
            ml_remote_usb_session_start(m_session);
        if (!checkStatus(status, "start", error)) {
            m_stopping = true;
            m_channelCloseRequested = true;
            m_channel->close();
            if (m_channelClosed) {
                finishStop();
            } else {
                scheduleStopRetry();
            }
            return false;
        }
        m_started = true;
        return true;
    } catch (const std::exception &exception) {
        m_stopping = true;
        m_channelCloseRequested = true;
        try {
            m_channel->close();
        } catch (...) {
            /* Preserve the startup exception below. */
        }
        if (m_channelClosed) {
            finishStop();
        } else {
            scheduleStopRetry();
        }
        return setError(QString::fromUtf8(exception.what()), error);
    } catch (...) {
        m_stopping = true;
        m_channelCloseRequested = true;
        try {
            m_channel->close();
        } catch (...) {
            /* Preserve the startup failure below. */
        }
        if (m_channelClosed) {
            finishStop();
        } else {
            scheduleStopRetry();
        }
        return setError(QStringLiteral("Remote USB startup failed"), error);
    }
}

void RemoteUsbSessionBinding::stop() noexcept
{
    try {
        if (QThread::currentThread() != thread()) {
            qWarning("RemoteUsbSessionBinding::stop called off owner thread");
            return;
        }
        if (m_stopped && m_session == nullptr) {
            return;
        }
        m_stopping = true;
        m_started = false;
        if (m_channel != nullptr && m_channelStarted &&
            !m_channelCloseRequested) {
            m_channelCloseRequested = true;
            m_channel->close();
        }
        /* Do not enter the C core until the channel has quiesced.  This keeps
         * late receive callbacks from racing stop()/destroy(). */
        if (m_channelClosed) {
            finishStop();
        } else {
            scheduleStopRetry();
        }
    } catch (...) {
        notifyError(QStringLiteral("Remote USB shutdown failed"));
        if (m_stopping) {
            scheduleStopRetry();
        }
        /* Never allow an exception to cross the noexcept boundary. */
    }
}

void RemoteUsbSessionBinding::destroySessionBestEffort() noexcept
{
    if (m_session == nullptr) {
        return;
    }
    const ml_remote_usb_session_status status =
        ml_remote_usb_session_destroy(m_session);
    if (status == ML_REMOTE_USB_SESSION_OK) {
        m_session = nullptr;
        return;
    }
    if (status != ML_REMOTE_USB_SESSION_BUSY) {
        m_lastError = cStatusMessage("destroy", status);
    }
}

void RemoteUsbSessionBinding::finishStop()
{
    if (QThread::currentThread() != thread()) {
        enqueueClosed();
        return;
    }
    if (!m_stopping) {
        return;
    }
    /* `start()` may fail after allocating channel resources, so
     * m_channelStarted is not proof that close() has completed. The only
     * quiescence signal is closedCallback (represented by m_channelClosed). */
    if (!m_channelClosed) {
        scheduleStopRetry();
        return;
    }

    /* Stop the C session only after the byte channel has drained. */
    if (m_session != nullptr && !m_coreStopDone) {
        const ml_remote_usb_session_status status =
            ml_remote_usb_session_stop(m_session);
        if (status == ML_REMOTE_USB_SESSION_BUSY) {
            scheduleStopRetry();
            return;
        }
        if (status != ML_REMOTE_USB_SESSION_OK) {
            notifyError(cStatusMessage("stop", status));
            /* Keep the object alive and retry: a terminal transport error can
             * still leave callback references that must be drained. */
            scheduleStopRetry();
            return;
        }
        m_coreStopDone = true;
    }

    /* The adapter owns pending native operations.  Release exactly once,
     * after core stop has cancelled them, and before destroying the C views. */
    if (!m_releaseCalled) {
        m_releaseCalled = true;
        if (m_platform != nullptr) {
            try {
                m_platform->release();
            } catch (...) {
                notifyError(QStringLiteral("Remote USB platform release failed"));
            }
        }
    }

    destroySessionBestEffort();
    if (m_session != nullptr) {
        scheduleStopRetry();
        return;
    }
    m_channelStarted = false;
    m_channelCloseRequested = false;
    m_stopping = false;
    m_stopped = true;
    m_helloAccepted = false;
    m_parseStage = ParseStage::Hello;
    m_helloOffset = 0;
    m_headerOffset = 0;
    m_payloadOffset = 0;
    m_expectedPayload = 0;
    m_payloadBuffer.clear();
    if (m_channel != nullptr && m_channelCallbacksInstalled) {
        try {
            m_channel->setCallbacks({}, {}, {});
            m_channelCallbacksInstalled = false;
        } catch (...) {
            notifyError(QStringLiteral("Remote USB channel callback cleanup failed"));
        }
    }
    if (!m_stoppedSignalEmitted) {
        m_stoppedSignalEmitted = true;
        emit stopped();
    }
}

void RemoteUsbSessionBinding::scheduleStopRetry()
{
    if (m_stopRetryScheduled || !m_stopping) {
        return;
    }
    try {
        m_stopRetryScheduled = true;
        QPointer<RemoteUsbSessionBinding> guard(this);
        if (!QMetaObject::invokeMethod(
                this,
                [guard]() {
                    if (!guard) {
                        return;
                    }
                    guard->m_stopRetryScheduled = false;
                    guard->finishStop();
                },
                Qt::QueuedConnection)) {
            m_stopRetryScheduled = false;
        }
    } catch (...) {
        m_stopRetryScheduled = false;
    }
}

ml_remote_usb_session_state RemoteUsbSessionBinding::state() const noexcept
{
    if (m_session == nullptr) {
        return m_stopped ? ML_REMOTE_USB_SESSION_STATE_STOPPED
                         : ML_REMOTE_USB_SESSION_STATE_NEW;
    }
    return ml_remote_usb_session_get_state(m_session);
}

bool RemoteUsbSessionBinding::sendWire(const std::uint8_t *wire,
                                       std::size_t wireSize,
                                       const char *kind)
{
    if (m_channel == nullptr || wire == nullptr || !sizeFitsQByteArray(wireSize)) {
        m_lastError = QStringLiteral("Remote USB %1 output is invalid")
                          .arg(QString::fromLatin1(kind));
        return false;
    }
    try {
        QString error;
        const QByteArray bytes(reinterpret_cast<const char *>(wire),
                               static_cast<qsizetype>(wireSize));
        if (!m_channel->send(bytes, &error)) {
            m_lastError = error.isEmpty()
                ? QStringLiteral("Remote USB %1 output failed")
                      .arg(QString::fromLatin1(kind))
                : error;
            return false;
        }
        return true;
    } catch (const std::exception &exception) {
        m_lastError = QString::fromUtf8(exception.what());
        return false;
    } catch (...) {
        m_lastError = QStringLiteral("Remote USB output failed");
        return false;
    }
}

bool RemoteUsbSessionBinding::feedBytes(const QByteArray &bytes, QString *error)
{
    if (!onOwnerThread(error)) {
        return false;
    }
    if (!checkSession(error)) {
        return false;
    }
    processBytes(bytes);
    if (m_failed) {
        return setError(m_lastError, error);
    }
    return true;
}

void RemoteUsbSessionBinding::enqueueBytes(QByteArray bytes)
{
    try {
        QPointer<RemoteUsbSessionBinding> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, bytes = std::move(bytes)]() mutable {
                if (guard) {
                    guard->processBytes(bytes);
                }
            },
            Qt::QueuedConnection);
    } catch (...) {
        notifyError(QStringLiteral("Remote USB receive queue failed"));
    }
}

void RemoteUsbSessionBinding::enqueueError(QString message)
{
    try {
        QPointer<RemoteUsbSessionBinding> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, message = std::move(message)]() mutable {
                if (guard) {
                    guard->processChannelError(message);
                }
            },
            Qt::QueuedConnection);
    } catch (...) {
        /* No safe way to report a queue allocation failure from here. */
    }
}

void RemoteUsbSessionBinding::enqueueClosed()
{
    try {
        QPointer<RemoteUsbSessionBinding> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard]() {
                if (guard) {
                    guard->processChannelClosed();
                }
            },
            Qt::QueuedConnection);
    } catch (...) {
        /* Closing remains best-effort; the owner can call stop() again. */
    }
}

void RemoteUsbSessionBinding::processChannelError(const QString &message)
{
    if (QThread::currentThread() != thread()) {
        enqueueError(message);
        return;
    }
    const QString detail = message.isEmpty() ? QStringLiteral("Remote USB channel error")
                                             : message;
    notifyError(detail);
    m_failed = true;
    if (!m_stopping) {
        stop();
    }
}

void RemoteUsbSessionBinding::processChannelClosed()
{
    if (QThread::currentThread() != thread()) {
        enqueueClosed();
        return;
    }
    m_channelClosed = true;
    if (m_stopping) {
        finishStop();
    } else if (m_session != nullptr) {
        /* An unexpected peer close must not leave the executor holding
         * platform callbacks indefinitely. */
        stop();
    }
}

void RemoteUsbSessionBinding::failProtocol(const QString &message)
{
    if (QThread::currentThread() != thread()) {
        enqueueError(message);
        return;
    }
    notifyError(message);
    m_failed = true;
    m_stopping = true;
    m_started = false;
    if (m_channel != nullptr) {
        try {
            if (!m_channelCloseRequested) {
                m_channelCloseRequested = true;
                m_channel->close();
            }
        } catch (...) {
            /* The protocol error is retained as the primary failure. */
        }
    }
    if (m_channelClosed) {
        stop();
    } else {
        scheduleStopRetry();
    }
}

void RemoteUsbSessionBinding::processBytes(const QByteArray &bytes)
{
    if (QThread::currentThread() != thread()) {
        enqueueBytes(bytes);
        return;
    }
    if (m_session == nullptr || m_stopped || m_failed || bytes.isEmpty()) {
        return;
    }
    try {
        qsizetype offset = 0;
        while (offset < bytes.size() && !m_failed) {
        if (!m_helloAccepted) {
            m_parseStage = ParseStage::Hello;
            const std::size_t remaining = kBrokerHelloSize - m_helloOffset;
            const qsizetype available = bytes.size() - offset;
            const std::size_t take = std::min<std::size_t>(
                remaining, static_cast<std::size_t>(available));
            std::memcpy(m_helloBuffer.data() + m_helloOffset,
                        bytes.constData() + offset, take);
            m_helloOffset += take;
            offset += static_cast<qsizetype>(take);
            if (m_helloOffset != kBrokerHelloSize) {
                continue;
            }
            const ml_remote_usb_session_status status =
                ml_remote_usb_session_accept_hello(m_session,
                                                   m_helloBuffer.data());
            if (!checkStatus(status, "accept hello", nullptr)) {
                failProtocol(m_lastError);
                return;
            }
            m_helloAccepted = true;
            m_parseStage = ParseStage::Header;
            m_headerOffset = 0;
            emit helloAccepted();
            continue;
        }

        if (m_headerOffset < kWireHeaderSize) {
            m_parseStage = ParseStage::Header;
            const std::size_t remaining = kWireHeaderSize - m_headerOffset;
            const qsizetype available = bytes.size() - offset;
            const std::size_t take = std::min<std::size_t>(
                remaining, static_cast<std::size_t>(available));
            std::memcpy(m_headerBuffer.data() + m_headerOffset,
                        bytes.constData() + offset, take);
            m_headerOffset += take;
            offset += static_cast<qsizetype>(take);
            if (m_headerOffset != kWireHeaderSize) {
                continue;
            }

            ml_remote_usb_wire_frame frame {};
            const ml_remote_usb_wire_status decodeStatus =
                ml_remote_usb_wire_decode_header(m_headerBuffer.data(), &frame);
            if (decodeStatus != ML_REMOTE_USB_WIRE_OK ||
                frame.payload_length > kWireMaxPayload) {
                failProtocol(QStringLiteral("Remote USB frame header is invalid"));
                return;
            }
            m_expectedPayload = frame.payload_length;
            m_payloadOffset = 0;
            m_payloadBuffer.resize(static_cast<qsizetype>(m_expectedPayload));
            m_parseStage = ParseStage::Payload;
            if (m_expectedPayload == 0) {
                const ml_remote_usb_session_status status =
                    ml_remote_usb_session_accept_frame(
                        m_session, m_headerBuffer.data(), nullptr, 0);
                if (!checkStatus(status, "accept frame", nullptr)) {
                    failProtocol(m_lastError);
                    return;
                }
                m_headerOffset = 0;
                m_parseStage = ParseStage::Header;
            }
            continue;
        }

        m_parseStage = ParseStage::Payload;
        const std::size_t remaining = m_expectedPayload - m_payloadOffset;
        const qsizetype available = bytes.size() - offset;
        const std::size_t take = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(available));
        if (take != 0) {
            std::memcpy(m_payloadBuffer.data() + m_payloadOffset,
                        bytes.constData() + offset, take);
        }
        m_payloadOffset += take;
        offset += static_cast<qsizetype>(take);
        if (m_payloadOffset != m_expectedPayload) {
            continue;
        }

        const ml_remote_usb_session_status status =
            ml_remote_usb_session_accept_frame(
                m_session, m_headerBuffer.data(),
                m_payloadBuffer.isEmpty()
                    ? nullptr
                    : reinterpret_cast<const std::uint8_t *>(
                          m_payloadBuffer.constData()),
                m_payloadBuffer.size());
        if (!checkStatus(status, "accept frame", nullptr)) {
            failProtocol(m_lastError);
            return;
        }
        m_headerOffset = 0;
        m_payloadOffset = 0;
        m_expectedPayload = 0;
        m_payloadBuffer.clear();
        m_parseStage = ParseStage::Header;
        }
    } catch (const std::exception &exception) {
        failProtocol(QString::fromUtf8(exception.what()));
    } catch (...) {
        failProtocol(QStringLiteral("Remote USB frame processing failed"));
    }
}

bool RemoteUsbSessionBinding::sendCapability(const DeviceSnapshot &device,
                                             QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    if (device.busId.isEmpty() ||
        static_cast<std::size_t>(device.busId.size()) > kBusIdMaxBytes ||
        device.busId.contains('\0') || device.rawDescriptors.isEmpty() ||
        device.rawDescriptors.size() > static_cast<qsizetype>(kRawDescriptorMaxBytes) ||
        device.endpoints.size() > static_cast<qsizetype>(kEndpointMaxCount) ||
        device.hasIsochronousEndpoints) {
        return setError(QStringLiteral("Remote USB device capability is invalid"), error);
    }

    std::vector<ml_remote_usb_wire_endpoint> endpoints;
    try {
        endpoints.resize(static_cast<std::size_t>(device.endpoints.size()));
    } catch (...) {
        return setError(QStringLiteral("Remote USB endpoint allocation failed"), error);
    }
    for (int index = 0; index < device.endpoints.size(); ++index) {
        const Endpoint &source = device.endpoints.at(index);
        if (source.reserved != 0) {
            return setError(QStringLiteral("Remote USB endpoint has non-zero reserved bits"),
                            error);
        }
        const quint8 endpointNumber = static_cast<quint8>(source.address & 0x0fu);
        if (endpointNumber == 0u && source.address != 0u) {
            return setError(QStringLiteral("Remote USB endpoint address is invalid"),
                            error);
        }
        if (endpointNumber != 0u && source.maxPacketSize == 0u) {
            return setError(QStringLiteral("Remote USB endpoint packet size is invalid"),
                            error);
        }
        ml_remote_usb_wire_endpoint &destination =
            endpoints[static_cast<std::size_t>(index)];
        destination.interface_number = source.interfaceNumber;
        destination.alternate_setting = source.alternateSetting;
        destination.address = source.address;
        destination.attributes = source.attributes;
        destination.max_packet_size = source.maxPacketSize;
        destination.interval = source.interval;
        destination.reserved = source.reserved;
    }

    ml_remote_usb_wire_capability capability {};
    capability.size = sizeof(capability);
    capability.version = ML_REMOTE_USB_WIRE_VERSION;
    capability.lease_token = m_options.brokerHello.lease_token;
    capability.attachment_token = m_options.brokerHello.attachment_token;
    capability.vendor_id = device.vendorId;
    capability.product_id = device.productId;
    capability.device_bcd = device.deviceBcd;
    capability.device_class = device.deviceClass;
    capability.device_subclass = device.deviceSubclass;
    capability.device_protocol = device.deviceProtocol;
    capability.bus_id = reinterpret_cast<const std::uint8_t *>(device.busId.constData());
    capability.bus_id_length = static_cast<std::size_t>(device.busId.size());
    capability.raw_descriptors = reinterpret_cast<const std::uint8_t *>(
        device.rawDescriptors.constData());
    capability.raw_descriptor_size =
        static_cast<std::size_t>(device.rawDescriptors.size());
    capability.endpoints = endpoints.empty() ? nullptr : endpoints.data();
    capability.endpoint_count = endpoints.size();

    const ml_remote_usb_session_status status =
        ml_remote_usb_session_send_capability(m_session, &capability);
    return checkStatus(status, "send capability", error);
}

bool RemoteUsbSessionBinding::sendOpen(quint64 leaseToken,
                                       quint64 attachmentToken,
                                       QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    if (leaseToken == 0 || attachmentToken == 0) {
        return setError(QStringLiteral("Remote USB OPEN tokens are invalid"), error);
    }
    const ml_remote_usb_session_status status = ml_remote_usb_session_send_open(
        m_session, leaseToken, attachmentToken);
    return checkStatus(status, "send open", error);
}

bool RemoteUsbSessionBinding::sendOpenOk(QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    return checkStatus(ml_remote_usb_session_send_open_ok(m_session),
                       "send open ok", error);
}

bool RemoteUsbSessionBinding::sendOpenReject(quint32 status, QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    return checkStatus(ml_remote_usb_session_send_open_reject(m_session, status),
                       "send open reject", error);
}

bool RemoteUsbSessionBinding::sendClose(QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    return checkStatus(ml_remote_usb_session_send_close(m_session),
                       "send close", error);
}

bool RemoteUsbSessionBinding::sendPdu(quint64 pduId, const QByteArray &pdu,
                                     QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    if (pdu.isEmpty() ||
        pdu.size() > static_cast<qsizetype>(kPduHeaderSize + kMaxTransferSize)) {
        return setError(QStringLiteral("Remote USB PDU size is invalid"), error);
    }
    const ml_remote_usb_session_status status = ml_remote_usb_session_send_pdu(
        m_session, pduId,
        reinterpret_cast<const std::uint8_t *>(pdu.constData()),
        static_cast<std::size_t>(pdu.size()));
    return checkStatus(status, "send PDU", error);
}

bool RemoteUsbSessionBinding::ackTx(quint64 pduId, QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    return checkStatus(ml_remote_usb_session_ack_tx(m_session, pduId),
                       "ack PDU", error);
}

bool RemoteUsbSessionBinding::copyTransfer(
    const ml_remote_usb_executor_transfer *source,
    TransferRequest *destination,
    QString *error)
{
    if (source == nullptr || destination == nullptr || source->request_token == 0 ||
        source->seqnum == 0 || source->direction > ML_REMOTE_USB_PDU_DIR_IN ||
        source->endpoint > ML_REMOTE_USB_PDU_MAX_ENDPOINT ||
        source->buffer_length > kMaxTransferSize ||
        !sizeFitsQByteArray(source->buffer_length) ||
        (source->buffer_length != 0 && source->buffer == nullptr) ||
        source->transfer_buffer_length < 0 ||
        static_cast<std::size_t>(source->transfer_buffer_length) !=
            source->buffer_length) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB transfer view is invalid");
        }
        return false;
    }
    *destination = TransferRequest {};
    destination->requestToken = source->request_token;
    destination->sequence = source->seqnum;
    destination->deviceId = source->devid;
    destination->direction = source->direction == ML_REMOTE_USB_PDU_DIR_IN
        ? TransferDirection::In : TransferDirection::Out;
    destination->endpoint = source->endpoint & 0x0fu;
    destination->transferFlags = source->transfer_flags;
    destination->transferBufferLength = source->transfer_buffer_length;
    destination->startFrame = source->start_frame;
    destination->numberOfPackets = source->number_of_packets;
    destination->interval = source->interval;
    std::memcpy(destination->setup.data(), source->setup,
                kSetupPacketSize);
    destination->kind = destination->endpoint == 0
        ? TransferKind::Control : TransferKind::Data;
    destination->endpointMetadataValid = source->endpoint_metadata_valid != 0;
    if (destination->endpointMetadataValid) {
        destination->endpointMetadata.interfaceNumber =
            source->endpoint_metadata.interface_number;
        destination->endpointMetadata.alternateSetting =
            source->endpoint_metadata.alternate_setting;
        destination->endpointMetadata.address = source->endpoint_metadata.address;
        destination->endpointMetadata.attributes =
            source->endpoint_metadata.attributes;
        destination->endpointMetadata.maxPacketSize =
            source->endpoint_metadata.max_packet_size;
        destination->endpointMetadata.interval = source->endpoint_metadata.interval;
        destination->endpointMetadata.reserved = source->endpoint_metadata.reserved;
    }
    if (destination->direction == TransferDirection::Out) {
        if (!copyBytes(source->buffer, source->buffer_length, &destination->data)) {
            if (error != nullptr) {
                *error = QStringLiteral("Remote USB OUT payload could not be copied");
            }
            return false;
        }
    } else {
        destination->data.clear();
    }
    if (source->kind != (destination->endpoint == 0
                             ? ML_REMOTE_USB_EXECUTOR_TRANSFER_CONTROL
                             : ML_REMOTE_USB_EXECUTOR_TRANSFER_DATA)) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB transfer kind is invalid");
        }
        return false;
    }
    if (!validateTransfer(*destination, error)) {
        return false;
    }
    if (destination->endpointMetadataValid &&
        !validateEndpoint(*destination, destination->endpointMetadata, error)) {
        return false;
    }
    return true;
}

bool RemoteUsbSessionBinding::copyPduRequest(
    const ml_remote_usb_pdu_request *source,
    TransferRequest *destination,
    QString *error)
{
    if (source == nullptr || destination == nullptr ||
        source->direction > ML_REMOTE_USB_PDU_DIR_IN ||
        source->endpoint > ML_REMOTE_USB_PDU_MAX_ENDPOINT ||
        source->data_length > kMaxTransferSize ||
        !sizeFitsQByteArray(source->data_length) ||
        (source->data_length != 0 && source->data == nullptr)) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB PDU request is invalid");
        }
        return false;
    }
    *destination = TransferRequest {};
    destination->requestToken = 0;
    destination->sequence = source->seqnum;
    destination->deviceId = source->devid;
    destination->direction = source->direction == ML_REMOTE_USB_PDU_DIR_IN
        ? TransferDirection::In : TransferDirection::Out;
    destination->endpoint = source->endpoint & 0x0fu;
    destination->transferFlags = source->transfer_flags;
    destination->transferBufferLength = source->transfer_buffer_length;
    destination->startFrame = source->start_frame;
    destination->numberOfPackets = source->number_of_packets;
    destination->interval = source->interval;
    std::memcpy(destination->setup.data(), source->setup, kSetupPacketSize);
    destination->kind = destination->endpoint == 0
        ? TransferKind::Control : TransferKind::Data;
    destination->endpointMetadataValid = false;
    if (destination->direction == TransferDirection::Out &&
        !copyBytes(source->data, source->data_length, &destination->data)) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB OUT payload could not be copied");
        }
        return false;
    }
    return validateTransfer(*destination, error);
}

bool RemoteUsbSessionBinding::validateTransfer(const TransferRequest &request,
                                               QString *error)
{
    const auto reject = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (request.direction != TransferDirection::Out &&
        request.direction != TransferDirection::In) {
        return reject(QStringLiteral("Remote USB transfer direction is invalid"));
    }
    if (request.endpoint > ML_REMOTE_USB_PDU_MAX_ENDPOINT) {
        return reject(QStringLiteral("Remote USB endpoint is invalid"));
    }
    if (request.transferBufferLength < 0 ||
        static_cast<std::size_t>(request.transferBufferLength) > kMaxTransferSize) {
        return reject(QStringLiteral("Remote USB transfer length is invalid"));
    }
    if (request.startFrame < 0 || request.interval < 0 ||
        request.numberOfPackets != 0) {
        return reject(QStringLiteral("Remote USB transfer timing fields are invalid"));
    }

    const qsizetype expectedLength =
        static_cast<qsizetype>(request.transferBufferLength);
    if (request.direction == TransferDirection::Out) {
        if (request.data.size() != expectedLength) {
            return reject(QStringLiteral("Remote USB OUT payload length is invalid"));
        }
    } else if (!request.data.isEmpty()) {
        return reject(QStringLiteral("Remote USB IN request must not carry payload"));
    }

    if (request.endpoint == 0) {
        const bool setupIn = (request.setup[0] & 0x80u) != 0u;
        if (setupIn != (request.direction == TransferDirection::In) ||
            request.transferBufferLength > std::numeric_limits<quint16>::max() ||
            readLe16(request.setup, 6u) !=
                static_cast<quint16>(request.transferBufferLength)) {
            return reject(QStringLiteral("Remote USB control setup is inconsistent"));
        }
    } else if (!allZero(request.setup)) {
        return reject(QStringLiteral("Remote USB data transfer has a setup packet"));
    }

    const TransferKind expectedKind = request.endpoint == 0
        ? TransferKind::Control : TransferKind::Data;
    if (request.kind != expectedKind) {
        return reject(QStringLiteral("Remote USB transfer kind is invalid"));
    }
    return true;
}

bool RemoteUsbSessionBinding::copyCompletion(
    const TransferRequest &request,
    const TransferCompletion &source,
    ml_remote_usb_executor_completion *destination,
    QByteArray *stableData,
    QString *error)
{
    if (destination == nullptr || stableData == nullptr ||
        !validateTransfer(request, error) ||
        source.actualLength < 0 || source.startFrame < 0 ||
        source.errorCount < 0 ||
        source.actualLength > static_cast<qint32>(kMaxTransferSize) ||
        source.actualLength > request.transferBufferLength ||
        (request.direction == TransferDirection::Out && !source.data.isEmpty()) ||
        (request.direction == TransferDirection::In &&
         source.data.size() != static_cast<qsizetype>(source.actualLength))) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB completion is invalid");
        }
        return false;
    }
    *stableData = source.data;
    std::memset(destination, 0, sizeof(*destination));
    destination->status = source.status;
    destination->actual_length = source.actualLength;
    destination->start_frame = source.startFrame;
    destination->error_count = source.errorCount;
    destination->data = stableData->isEmpty()
        ? nullptr
        : reinterpret_cast<const std::uint8_t *>(stableData->constData());
    destination->data_length = static_cast<std::size_t>(stableData->size());
    return true;
}

bool RemoteUsbSessionBinding::validateEndpoint(const TransferRequest &request,
                                               const Endpoint &endpoint,
                                               QString *error)
{
    if (endpoint.reserved != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB endpoint reserved bits are non-zero");
        }
        return false;
    }
    const quint8 expectedAddress = request.endpoint == 0
        ? 0
        : static_cast<quint8>((request.endpoint & 0x0fu) |
                              (request.direction == TransferDirection::In
                                   ? 0x80u : 0u));
    if (endpoint.address != expectedAddress ||
        (request.endpoint != 0 && endpoint.maxPacketSize == 0)) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB endpoint metadata does not match the request");
        }
        return false;
    }
    return true;
}

void RemoteUsbSessionBinding::queueCompletion(std::uint64_t requestToken,
                                              TransferCompletion completion)
{
    try {
        QPointer<RemoteUsbSessionBinding> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, requestToken, completion = std::move(completion)]() mutable {
                if (!guard || guard->m_session == nullptr || guard->m_stopped) {
                    return;
                }
                QByteArray stableData = completion.data;
                ml_remote_usb_executor_completion cCompletion {};
                cCompletion.status = completion.status;
                cCompletion.actual_length = completion.actualLength;
                cCompletion.start_frame = completion.startFrame;
                cCompletion.error_count = completion.errorCount;
                cCompletion.data = stableData.isEmpty()
                    ? nullptr
                    : reinterpret_cast<const std::uint8_t *>(stableData.constData());
                cCompletion.data_length = static_cast<std::size_t>(stableData.size());
                const ml_remote_usb_session_status status =
                    ml_remote_usb_session_complete(guard->m_session, requestToken,
                                                   &cCompletion);
                if (status != ML_REMOTE_USB_SESSION_OK &&
                    !(guard->m_stopping &&
                      status == ML_REMOTE_USB_SESSION_REQUEST_NOT_FOUND)) {
                    guard->failProtocol(cStatusMessage("complete", status));
                }
            },
            Qt::QueuedConnection);
    } catch (...) {
        notifyError(QStringLiteral("Remote USB completion queue failed"));
    }
}

void RemoteUsbSessionBinding::queueCancelCompletion(std::uint64_t requestToken,
                                                    std::int32_t status)
{
    try {
        QPointer<RemoteUsbSessionBinding> guard(this);
        QMetaObject::invokeMethod(
            this,
            [guard, requestToken, status]() {
                if (!guard || guard->m_session == nullptr || guard->m_stopped) {
                    return;
                }
                const ml_remote_usb_session_status result =
                    ml_remote_usb_session_cancel_complete(guard->m_session,
                                                          requestToken, status);
                if (result != ML_REMOTE_USB_SESSION_OK &&
                    !(guard->m_stopping &&
                      result == ML_REMOTE_USB_SESSION_REQUEST_NOT_FOUND)) {
                    guard->failProtocol(cStatusMessage("cancel complete", result));
                }
            },
            Qt::QueuedConnection);
    } catch (...) {
        notifyError(QStringLiteral("Remote USB cancel queue failed"));
    }
}

ml_remote_usb_executor_submit_result RemoteUsbSessionBinding::submitImpl(
    const ml_remote_usb_executor_transfer *transfer,
    ml_remote_usb_executor_completion *completionOut,
    bool control)
{
    if (completionOut != nullptr) {
        std::memset(completionOut, 0, sizeof(*completionOut));
        completionOut->status = kCallbackFailureStatus;
    }
    if (transfer == nullptr || completionOut == nullptr || m_platform == nullptr) {
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
    }

    TransferRequest request;
    QString conversionError;
    if (!copyTransfer(transfer, &request, &conversionError) ||
        (control != (request.endpoint == 0))) {
        notifyError(conversionError.isEmpty()
                        ? QStringLiteral("Remote USB transfer request was rejected")
                        : conversionError);
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
    }

    auto gate = std::make_shared<SubmitGate>();
    gate->binding = this;
    gate->requestToken = transfer->request_token;
    gate->request = request;
    TransferCompletionCallback callback =
        [gate](quint64 requestToken, TransferCompletion completion) {
            try {
                if (requestToken != gate->requestToken) {
                    if (gate->binding) {
                        gate->binding->notifyError(
                            QStringLiteral("Remote USB completion token mismatch"));
                    }
                    return;
                }
                /* Validate while the value is still on the callback stack;
                 * the queued path below retains an owned QByteArray copy. */
                ml_remote_usb_executor_completion ignored {};
                QByteArray stableData;
                QString validationError;
                const bool valid = RemoteUsbSessionBinding::copyCompletion(
                    gate->request, completion, &ignored, &stableData,
                    &validationError);
                if (!valid && gate->binding) {
                    gate->binding->notifyError(validationError);
                }
                bool queue = false;
                bool duplicate = false;
                {
                    std::lock_guard<std::mutex> lock(gate->mutex);
                    if (gate->callbackSeen) {
                        gate->duplicateCallback = true;
                        duplicate = true;
                    } else {
                        gate->callbackSeen = true;
                        if (gate->returned) {
                            gate->lateCallback = true;
                            queue = true;
                        } else {
                            gate->completion = std::move(completion);
                        }
                    }
                }
                if (duplicate && gate->binding) {
                    gate->binding->notifyError(
                        QStringLiteral("Remote USB submit completed more than once"));
                } else if (queue && gate->binding) {
                    gate->binding->queueCompletion(requestToken,
                                                   std::move(completion));
                }
            } catch (const std::exception &exception) {
                if (gate->binding) {
                    gate->binding->notifyError(QString::fromUtf8(exception.what()));
                }
            } catch (...) {
                if (gate->binding) {
                    gate->binding->notifyError(
                        QStringLiteral("Remote USB submit completion callback failed"));
                }
            }
        };

    SubmitDisposition disposition = SubmitDisposition::Rejected;
    QString adapterError;
    try {
        disposition = control
            ? m_platform->submitControl(request, std::move(callback), &adapterError)
            : m_platform->submitData(request, std::move(callback), &adapterError);
    } catch (const std::exception &exception) {
        adapterError = QString::fromUtf8(exception.what());
        disposition = SubmitDisposition::Rejected;
    } catch (...) {
        adapterError = QStringLiteral("Remote USB platform submit threw an exception");
        disposition = SubmitDisposition::Rejected;
    }

    TransferCompletion immediate;
    bool callbackSeen = false;
    bool lateCallback = false;
    bool duplicateCallback = false;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->returned = true;
        callbackSeen = gate->callbackSeen;
        lateCallback = gate->lateCallback;
        duplicateCallback = gate->duplicateCallback;
        if (callbackSeen) {
            immediate = gate->completion;
        }
    }
    if (duplicateCallback) {
        notifyError(QStringLiteral("Remote USB submit completion callback was duplicated"));
    }

    switch (disposition) {
    case SubmitDisposition::Completed: {
        if (!callbackSeen || lateCallback || duplicateCallback) {
            completionOut->status = kCallbackFailureStatus;
            return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
        }
        QByteArray stableData;
        QString completionError;
        if (!copyCompletion(request, immediate, completionOut, &stableData,
                            &completionError)) {
            completionOut->status = kCallbackFailureStatus;
            return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
        }
        /* invoke_submit_callback() consumes completionOut only after this
         * callback returns. A QByteArray local would dangle at that point,
         * so put synchronous IN data in the executor-owned transfer buffer.
         * The buffer is writable for IN requests and remains alive until the
         * executor has encoded the RET_SUBMIT packet. */
        if (!stableData.isEmpty()) {
            if (transfer->buffer == nullptr ||
                stableData.size() > static_cast<qsizetype>(transfer->buffer_length)) {
                completionOut->status = kCallbackFailureStatus;
                completionOut->data = nullptr;
                completionOut->data_length = 0;
                return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
            }
            std::memcpy(transfer->buffer, stableData.constData(),
                        static_cast<std::size_t>(stableData.size()));
            completionOut->data = transfer->buffer;
            completionOut->data_length = static_cast<std::size_t>(stableData.size());
        } else {
            completionOut->data = nullptr;
            completionOut->data_length = 0;
        }
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_COMPLETED;
    }
    case SubmitDisposition::Pending:
        if (callbackSeen && !lateCallback) {
            queueCompletion(transfer->request_token, std::move(immediate));
        }
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_PENDING;
    case SubmitDisposition::Rejected:
    default:
        completionOut->status = kRejectedStatus;
        if (gate->binding && !adapterError.isEmpty()) {
            gate->binding->notifyError(adapterError);
        }
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
    }
}

ml_remote_usb_executor_cancel_result RemoteUsbSessionBinding::cancelImpl(
    const ml_remote_usb_executor_transfer *transfer,
    std::int32_t *statusOut)
{
    if (statusOut != nullptr) {
        *statusOut = kCallbackFailureStatus;
    }
    if (transfer == nullptr || statusOut == nullptr || m_platform == nullptr) {
        return ML_REMOTE_USB_EXECUTOR_CANCEL_FAILED;
    }
    TransferRequest request;
    QString conversionError;
    if (!copyTransfer(transfer, &request, &conversionError)) {
        notifyError(conversionError.isEmpty()
                        ? QStringLiteral("Remote USB cancel request was rejected")
                        : conversionError);
        return ML_REMOTE_USB_EXECUTOR_CANCEL_FAILED;
    }

    auto gate = std::make_shared<CancelGate>();
    gate->binding = this;
    gate->requestToken = transfer->request_token;
    CancelCompletionCallback callback =
        [gate](quint64 requestToken, qint32 status) {
            try {
                if (requestToken != gate->requestToken) {
                    if (gate->binding) {
                        gate->binding->notifyError(
                            QStringLiteral("Remote USB cancel token mismatch"));
                    }
                    return;
                }
                bool queue = false;
                bool duplicate = false;
                {
                    std::lock_guard<std::mutex> lock(gate->mutex);
                    if (gate->callbackSeen) {
                        gate->duplicateCallback = true;
                        duplicate = true;
                    } else {
                        gate->callbackSeen = true;
                        if (gate->returned) {
                            gate->lateCallback = true;
                            queue = true;
                        } else {
                            gate->status = status;
                        }
                    }
                }
                if (duplicate && gate->binding) {
                    gate->binding->notifyError(
                        QStringLiteral("Remote USB cancel completed more than once"));
                } else if (queue && gate->binding) {
                    gate->binding->queueCancelCompletion(requestToken, status);
                }
            } catch (const std::exception &exception) {
                if (gate->binding) {
                    gate->binding->notifyError(QString::fromUtf8(exception.what()));
                }
            } catch (...) {
                if (gate->binding) {
                    gate->binding->notifyError(
                        QStringLiteral("Remote USB cancel callback failed"));
                }
            }
        };

    CancelDisposition disposition = CancelDisposition::Failed;
    QString adapterError;
    try {
        disposition = m_platform->cancel(request, std::move(callback), statusOut,
                                          &adapterError);
    } catch (const std::exception &exception) {
        adapterError = QString::fromUtf8(exception.what());
        disposition = CancelDisposition::Failed;
    } catch (...) {
        adapterError = QStringLiteral("Remote USB platform cancel threw an exception");
        disposition = CancelDisposition::Failed;
    }

    qint32 immediateStatus = *statusOut;
    bool callbackSeen = false;
    bool duplicateCallback = false;
    bool lateCallback = false;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->returned = true;
        callbackSeen = gate->callbackSeen;
        duplicateCallback = gate->duplicateCallback;
        lateCallback = gate->lateCallback;
        if (callbackSeen) {
            immediateStatus = gate->status;
        }
    }
    if (duplicateCallback) {
        notifyError(QStringLiteral("Remote USB cancel callback was duplicated"));
    }
    if (lateCallback && disposition != CancelDisposition::Pending) {
        *statusOut = kRejectedStatus;
        notifyError(QStringLiteral("Remote USB cancel callback was not synchronous"));
        return ML_REMOTE_USB_EXECUTOR_CANCEL_FAILED;
    }

    switch (disposition) {
    case CancelDisposition::Pending:
        if (callbackSeen && !lateCallback) {
            queueCancelCompletion(transfer->request_token, immediateStatus);
        }
        *statusOut = immediateStatus;
        return ML_REMOTE_USB_EXECUTOR_CANCEL_PENDING;
    case CancelDisposition::Completed:
    case CancelDisposition::NotFound:
        *statusOut = immediateStatus;
        return disposition == CancelDisposition::Completed
            ? ML_REMOTE_USB_EXECUTOR_CANCEL_COMPLETED
            : ML_REMOTE_USB_EXECUTOR_CANCEL_NOT_FOUND;
    case CancelDisposition::Failed:
    default:
        *statusOut = immediateStatus < 0 ? immediateStatus : kRejectedStatus;
        if (gate->binding && !adapterError.isEmpty()) {
            gate->binding->m_lastError = adapterError;
        }
        return ML_REMOTE_USB_EXECUTOR_CANCEL_FAILED;
    }
}

ml_remote_usb_executor_submit_result RemoteUsbSessionBinding::submitControl(
    void *context,
    ml_remote_usb_executor *executor,
    const ml_remote_usb_executor_transfer *transfer,
    ml_remote_usb_executor_completion *completionOut) noexcept
{
    Q_UNUSED(executor);
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (completionOut != nullptr) {
        std::memset(completionOut, 0, sizeof(*completionOut));
        completionOut->status = kCallbackFailureStatus;
    }
    if (binding == nullptr) {
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
    }
    try {
        return binding->submitImpl(transfer, completionOut, true);
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB control submit callback failed"));
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
    }
}

ml_remote_usb_executor_submit_result RemoteUsbSessionBinding::submitData(
    void *context,
    ml_remote_usb_executor *executor,
    const ml_remote_usb_executor_transfer *transfer,
    ml_remote_usb_executor_completion *completionOut) noexcept
{
    Q_UNUSED(executor);
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (completionOut != nullptr) {
        std::memset(completionOut, 0, sizeof(*completionOut));
        completionOut->status = kCallbackFailureStatus;
    }
    if (binding == nullptr) {
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
    }
    try {
        return binding->submitImpl(transfer, completionOut, false);
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB data submit callback failed"));
        return ML_REMOTE_USB_EXECUTOR_SUBMIT_FAILED;
    }
}

ml_remote_usb_executor_cancel_result RemoteUsbSessionBinding::cancel(
    void *context,
    ml_remote_usb_executor *executor,
    const ml_remote_usb_executor_transfer *transfer,
    std::int32_t *statusOut) noexcept
{
    Q_UNUSED(executor);
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (statusOut != nullptr) {
        *statusOut = kCallbackFailureStatus;
    }
    if (binding == nullptr) {
        return ML_REMOTE_USB_EXECUTOR_CANCEL_FAILED;
    }
    try {
        return binding->cancelImpl(transfer, statusOut);
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB cancel callback failed"));
        return ML_REMOTE_USB_EXECUTOR_CANCEL_FAILED;
    }
}

ml_remote_usb_executor_endpoint_result RemoteUsbSessionBinding::resolveEndpoint(
    void *context,
    const ml_remote_usb_pdu_request *request,
    ml_remote_usb_executor_endpoint *endpointOut) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr || request == nullptr || endpointOut == nullptr ||
        binding->m_platform == nullptr) {
        return ML_REMOTE_USB_EXECUTOR_ENDPOINT_REJECTED;
    }
    try {
        TransferRequest qtRequest;
        if (!copyPduRequest(request, &qtRequest, nullptr)) {
            return ML_REMOTE_USB_EXECUTOR_ENDPOINT_REJECTED;
        }
        Endpoint endpoint;
        QString error;
        const EndpointResolution resolution =
            binding->m_platform->resolveEndpoint(qtRequest, &endpoint, &error);
        switch (resolution) {
        case EndpointResolution::Found:
            if (!validateEndpoint(qtRequest, endpoint, &error)) {
                binding->notifyError(error);
                return ML_REMOTE_USB_EXECUTOR_ENDPOINT_REJECTED;
            }
            endpointOut->interface_number = endpoint.interfaceNumber;
            endpointOut->alternate_setting = endpoint.alternateSetting;
            endpointOut->address = endpoint.address;
            endpointOut->attributes = endpoint.attributes;
            endpointOut->max_packet_size = endpoint.maxPacketSize;
            endpointOut->interval = endpoint.interval;
            endpointOut->reserved = endpoint.reserved;
            return ML_REMOTE_USB_EXECUTOR_ENDPOINT_OK;
        case EndpointResolution::NotFound:
            return ML_REMOTE_USB_EXECUTOR_ENDPOINT_NOT_FOUND;
        case EndpointResolution::Rejected:
        default:
            return ML_REMOTE_USB_EXECUTOR_ENDPOINT_REJECTED;
        }
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB endpoint lookup failed"));
        return ML_REMOTE_USB_EXECUTOR_ENDPOINT_REJECTED;
    }
}

int RemoteUsbSessionBinding::sendHello(void *context,
                                       const std::uint8_t *wire,
                                       std::size_t wireSize) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    return binding != nullptr && binding->sendWire(wire, wireSize, "HELLO")
        ? 0 : -1;
}

int RemoteUsbSessionBinding::sendFrame(void *context,
                                       const std::uint8_t *wire,
                                       std::size_t wireSize) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    return binding != nullptr && binding->sendWire(wire, wireSize, "frame")
        ? 0 : -1;
}

int RemoteUsbSessionBinding::onCapability(
    void *context, const ml_remote_usb_wire_capability *capability) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr || capability == nullptr) {
        return -1;
    }
    try {
        DeviceSnapshot snapshot;
        if (!copyBytes(capability->bus_id, capability->bus_id_length,
                       &snapshot.busId) ||
            !copyBytes(capability->raw_descriptors,
                       capability->raw_descriptor_size,
                       &snapshot.rawDescriptors) ||
            snapshot.busId.isEmpty() || snapshot.busId.contains('\0') ||
            snapshot.busId.size() > static_cast<qsizetype>(kBusIdMaxBytes) ||
            snapshot.rawDescriptors.isEmpty() ||
            snapshot.rawDescriptors.size() >
                static_cast<qsizetype>(kRawDescriptorMaxBytes) ||
            capability->endpoint_count > kEndpointMaxCount) {
            binding->notifyError(QStringLiteral("Remote USB capability payload is invalid"));
            return -1;
        }
        snapshot.deviceId = snapshot.busId;
        snapshot.vendorId = capability->vendor_id;
        snapshot.productId = capability->product_id;
        snapshot.deviceBcd = capability->device_bcd;
        snapshot.deviceClass = capability->device_class;
        snapshot.deviceSubclass = capability->device_subclass;
        snapshot.deviceProtocol = capability->device_protocol;
        snapshot.endpoints.reserve(static_cast<qsizetype>(capability->endpoint_count));
        for (std::size_t index = 0; index < capability->endpoint_count; ++index) {
            ml_remote_usb_wire_endpoint endpointWire {};
            if (ml_remote_usb_wire_capability_get_endpoint(
                    capability, index, &endpointWire) != ML_REMOTE_USB_WIRE_OK) {
                binding->notifyError(QStringLiteral("Remote USB capability endpoint is invalid"));
                return -1;
            }
            Endpoint endpoint;
            endpoint.interfaceNumber = endpointWire.interface_number;
            endpoint.alternateSetting = endpointWire.alternate_setting;
            endpoint.address = endpointWire.address;
            endpoint.attributes = endpointWire.attributes;
            endpoint.maxPacketSize = endpointWire.max_packet_size;
            endpoint.interval = endpointWire.interval;
            endpoint.reserved = endpointWire.reserved;
            const quint8 endpointNumber = static_cast<quint8>(endpoint.address & 0x0fu);
            if ((endpointNumber == 0u && endpoint.address != 0u) ||
                (endpointNumber != 0u && endpoint.maxPacketSize == 0u)) {
                binding->notifyError(QStringLiteral("Remote USB capability endpoint address is invalid"));
                return -1;
            }
            snapshot.hasIsochronousEndpoints =
                snapshot.hasIsochronousEndpoints ||
                ((endpoint.attributes & 0x03u) == 0x01u);
            snapshot.endpoints.append(endpoint);
        }
        emit binding->capabilityReceived(std::move(snapshot));
        return 0;
    } catch (const std::exception &exception) {
        binding->notifyError(QString::fromUtf8(exception.what()));
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB capability callback failed"));
    }
    return -1;
}

int RemoteUsbSessionBinding::onOpen(void *context,
                                    const ml_remote_usb_wire_open *open) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr || open == nullptr || open->lease_token == 0 ||
        open->attachment_token == 0) {
        if (binding != nullptr) {
            binding->notifyError(QStringLiteral("Remote USB OPEN payload is invalid"));
        }
        return -1;
    }
    try {
        emit binding->openRequested(open->lease_token, open->attachment_token);
        return 0;
    } catch (const std::exception &exception) {
        binding->notifyError(QString::fromUtf8(exception.what()));
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB OPEN callback failed"));
    }
    return -1;
}

int RemoteUsbSessionBinding::onOpenOk(void *context) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr) {
        return -1;
    }
    try {
        emit binding->openAccepted();
        return 0;
    } catch (const std::exception &exception) {
        binding->notifyError(QString::fromUtf8(exception.what()));
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB OPEN_OK callback failed"));
    }
    return -1;
}

int RemoteUsbSessionBinding::onOpenReject(void *context,
                                          std::uint32_t status) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr) {
        return -1;
    }
    try {
        emit binding->openRejected(status);
        return 0;
    } catch (const std::exception &exception) {
        binding->notifyError(QString::fromUtf8(exception.what()));
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB OPEN_REJECT callback failed"));
    }
    return -1;
}

int RemoteUsbSessionBinding::onPdu(void *context, std::uint64_t pduId,
                                   const std::uint8_t *pdu,
                                   std::size_t pduSize) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr || pduId == 0 || pduSize == 0 ||
        pduSize > kMaxReassemblySize || pdu == nullptr) {
        if (binding != nullptr) {
            binding->notifyError(QStringLiteral("Remote USB PDU callback payload is invalid"));
        }
        return -1;
    }
    try {
        QByteArray copy;
        if (!copyBytes(pdu, pduSize, &copy)) {
            binding->notifyError(QStringLiteral("Remote USB PDU could not be copied"));
            return -1;
        }
        emit binding->pduReceived(pduId, std::move(copy));
        return 0;
    } catch (const std::exception &exception) {
        binding->notifyError(QString::fromUtf8(exception.what()));
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB PDU callback failed"));
    }
    return -1;
}

int RemoteUsbSessionBinding::onClose(void *context,
                                     std::uint64_t leaseToken) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr || leaseToken == 0) {
        if (binding != nullptr) {
            binding->notifyError(QStringLiteral("Remote USB CLOSE payload is invalid"));
        }
        return -1;
    }
    try {
        emit binding->peerClosed(leaseToken);
        return 0;
    } catch (const std::exception &exception) {
        binding->notifyError(QString::fromUtf8(exception.what()));
    } catch (...) {
        binding->notifyError(QStringLiteral("Remote USB CLOSE callback failed"));
    }
    return -1;
}

void RemoteUsbSessionBinding::onCoreError(
    void *context, ml_remote_usb_transport_status status) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr) {
        return;
    }
    try {
        if (QThread::currentThread() != binding->thread()) {
            QPointer<RemoteUsbSessionBinding> guard(binding);
            QMetaObject::invokeMethod(
                binding,
                [guard, status]() {
                    if (guard) {
                        RemoteUsbSessionBinding::onCoreError(guard.data(), status);
                    }
                },
                Qt::QueuedConnection);
            return;
        }

        const QString message = QStringLiteral("Remote USB transport error (status %1)")
                                    .arg(static_cast<int>(status));
        binding->notifyError(message);
        binding->m_failed = true;
        if (!binding->m_stopping) {
            binding->m_stopping = true;
            binding->m_started = false;
            if (binding->m_channel != nullptr &&
                !binding->m_channelCloseRequested) {
                binding->m_channelCloseRequested = true;
                binding->m_channel->close();
            }
            if (binding->m_channelClosed) {
                binding->finishStop();
            } else {
                binding->scheduleStopRetry();
            }
        }
    } catch (...) {
        /* Keep the error signal even if scheduling shutdown fails. */
    }
}

void RemoteUsbSessionBinding::onCoreStopped(void *context) noexcept
{
    RemoteUsbSessionBinding *binding = fromContext(context);
    if (binding == nullptr) {
        return;
    }
    try {
        if (QThread::currentThread() != binding->thread()) {
            QPointer<RemoteUsbSessionBinding> guard(binding);
            QMetaObject::invokeMethod(
                binding,
                [guard]() {
                    if (guard) {
                        guard->m_coreStopDone = true;
                    }
                },
                Qt::QueuedConnection);
            return;
        }
        binding->m_coreStopDone = true;
    } catch (...) {
        /* C callbacks are noexcept; a queued notification is best effort. */
    }
}

} // namespace RemoteUsb
