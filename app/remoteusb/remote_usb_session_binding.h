#pragma once

/*
 * Qt owner-loop binding for the platform-neutral Remote USB C session.
 *
 * This class deliberately owns no socket or USB handle.  The caller supplies
 * a platform adapter and an already-authenticated byte channel.  All C core
 * calls are made on the QObject's thread; callbacks arriving from an I/O or
 * USB worker are queued back to that thread before entering the C core.
 */

#include "remote_usb_platform_adapter.h"

#include <QObject>

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "remote_usb_broker.h"
#include "remote_usb_executor.h"
#include "remote_usb_session.h"
#include "remote_usb_wire.h"
}

namespace RemoteUsb {

struct RemoteUsbSessionBindingOptions {
    ml_remote_usb_broker_hello brokerHello {};
    quint64 txWindowBytes = 0;
    quint32 txWindowPdus = 0;
    quint64 rxWindowBytes = 0;
    quint32 rxWindowPdus = 0;
    quint32 maxReassemblySize = 0;
    quint32 maxFragments = 0;
    quint32 maxInflight = 0;
    quint32 maxTransferSize = 0;
};

/*
 * A minimal vertical slice around ml_remote_usb_session.  The object is
 * owner-thread affine: public methods, including stop(), must be called from
 * the thread where the object was constructed.  The supplied adapter and
 * channel are borrowed and must outlive this object (and all callbacks).
 * A binding is single-use: after the first start attempt, create a new
 * binding/channel pair for reconnect so delayed callbacks cannot cross
 * session generations.
 */
class RemoteUsbSessionBinding final : public QObject
{
    Q_OBJECT

public:
    RemoteUsbSessionBinding(RemoteUsbPlatformAdapter *platform,
                            RemoteUsbByteChannel *channel,
                            const RemoteUsbSessionBindingOptions &options,
                            QObject *parent = nullptr);
    ~RemoteUsbSessionBinding() override;

    RemoteUsbSessionBinding(const RemoteUsbSessionBinding &) = delete;
    RemoteUsbSessionBinding &operator=(const RemoteUsbSessionBinding &) = delete;

    /* Starts the authenticated channel, then emits the broker HELLO. */
    bool start(QString *error = nullptr);

    /* Idempotent owner-loop shutdown.  Completion is observable via stopped(). */
    void stop() noexcept;

    bool isStarted() const noexcept { return m_started; }
    bool isStopping() const noexcept { return m_stopping; }
    bool isStopped() const noexcept { return m_stopped; }
    ml_remote_usb_session_state state() const noexcept;
    QString lastError() const { return m_lastError; }

    /* Feed arbitrary byte-stream data.  This is public for deterministic
     * loopback tests; production callers normally use the channel callback. */
    bool feedBytes(const QByteArray &bytes, QString *error = nullptr);

    bool sendCapability(const DeviceSnapshot &device, QString *error = nullptr);
    bool sendOpen(quint64 leaseToken, quint64 attachmentToken,
                  QString *error = nullptr);
    bool sendOpenOk(QString *error = nullptr);
    bool sendOpenReject(quint32 status, QString *error = nullptr);
    bool sendClose(QString *error = nullptr);
    bool sendPdu(quint64 pduId, const QByteArray &pdu,
                 QString *error = nullptr);
    bool ackTx(quint64 pduId, QString *error = nullptr);

signals:
    /* All signals are emitted on the binding's owner Qt thread.  Payloads are
     * copied before emission because the C callback views are ephemeral. */
    void capabilityReceived(RemoteUsb::DeviceSnapshot capability);
    /* Emitted after the peer's exact 84-byte HELLO has been accepted. */
    void helloAccepted();
    void openRequested(quint64 leaseToken, quint64 attachmentToken);
    void openAccepted();
    void openRejected(quint32 status);
    void pduReceived(quint64 pduId, QByteArray pdu);
    void peerClosed(quint64 leaseToken);
    void errorOccurred(QString message);
    void stopped();

private:
    enum class ParseStage {
        Hello,
        Header,
        Payload,
    };

    struct SubmitGate;
    struct CancelGate;

    bool onOwnerThread(QString *error = nullptr) const;
    bool setError(const QString &message, QString *error = nullptr);
    void notifyError(const QString &message) noexcept;
    bool checkSession(QString *error = nullptr) const;
    bool checkStatus(ml_remote_usb_session_status status,
                     const char *operation,
                     QString *error = nullptr);
    void failProtocol(const QString &message);

    void installChannelCallbacks();
    void enqueueBytes(QByteArray bytes);
    void enqueueError(QString message);
    void enqueueClosed();
    void processBytes(const QByteArray &bytes);
    void processChannelError(const QString &message);
    void processChannelClosed();
    void finishStop();
    void scheduleStopRetry();

    bool createSession(QString *error);
    void destroySessionBestEffort() noexcept;

    bool sendWire(const std::uint8_t *wire, std::size_t wireSize,
                  const char *kind);

    static RemoteUsbSessionBinding *fromContext(void *context) noexcept;

    static ml_remote_usb_executor_submit_result submitControl(
        void *context,
        ml_remote_usb_executor *executor,
        const ml_remote_usb_executor_transfer *transfer,
        ml_remote_usb_executor_completion *completionOut) noexcept;
    static ml_remote_usb_executor_submit_result submitData(
        void *context,
        ml_remote_usb_executor *executor,
        const ml_remote_usb_executor_transfer *transfer,
        ml_remote_usb_executor_completion *completionOut) noexcept;
    static ml_remote_usb_executor_cancel_result cancel(
        void *context,
        ml_remote_usb_executor *executor,
        const ml_remote_usb_executor_transfer *transfer,
        std::int32_t *statusOut) noexcept;
    static ml_remote_usb_executor_endpoint_result resolveEndpoint(
        void *context,
        const ml_remote_usb_pdu_request *request,
        ml_remote_usb_executor_endpoint *endpointOut) noexcept;

    static int sendHello(void *context, const std::uint8_t *wire,
                         std::size_t wireSize) noexcept;
    static int sendFrame(void *context, const std::uint8_t *wire,
                         std::size_t wireSize) noexcept;
    static int onCapability(void *context,
                            const ml_remote_usb_wire_capability *capability) noexcept;
    static int onOpen(void *context,
                      const ml_remote_usb_wire_open *open) noexcept;
    static int onOpenOk(void *context) noexcept;
    static int onOpenReject(void *context, std::uint32_t status) noexcept;
    static int onPdu(void *context, std::uint64_t pduId,
                     const std::uint8_t *pdu, std::size_t pduSize) noexcept;
    static int onClose(void *context, std::uint64_t leaseToken) noexcept;
    static void onCoreError(void *context,
                            ml_remote_usb_transport_status status) noexcept;
    static void onCoreStopped(void *context) noexcept;

    static bool copyTransfer(const ml_remote_usb_executor_transfer *source,
                             TransferRequest *destination,
                             QString *error = nullptr);
    static bool copyPduRequest(const ml_remote_usb_pdu_request *source,
                               TransferRequest *destination,
                               QString *error = nullptr);
    static bool copyCompletion(const TransferRequest &request,
                               const TransferCompletion &source,
                               ml_remote_usb_executor_completion *destination,
                               QByteArray *stableData,
                               QString *error = nullptr);
    static bool validateTransfer(const TransferRequest &request,
                                 QString *error = nullptr);
    static bool validateEndpoint(const TransferRequest &request,
                                 const Endpoint &endpoint,
                                 QString *error = nullptr);

    ml_remote_usb_executor_submit_result submitImpl(
        const ml_remote_usb_executor_transfer *transfer,
        ml_remote_usb_executor_completion *completionOut,
        bool control);
    ml_remote_usb_executor_cancel_result cancelImpl(
        const ml_remote_usb_executor_transfer *transfer,
        std::int32_t *statusOut);

    void queueCompletion(std::uint64_t requestToken,
                         TransferCompletion completion);
    void queueCancelCompletion(std::uint64_t requestToken, std::int32_t status);

    RemoteUsbPlatformAdapter *m_platform = nullptr;
    RemoteUsbByteChannel *m_channel = nullptr;
    RemoteUsbSessionBindingOptions m_options;
    ml_remote_usb_session *m_session = nullptr;

    bool m_started = false;
    /* A binding owns one C session and one callback generation.  Reconnects
     * create a fresh binding so queued completions from the old generation
     * can never be delivered to a new session with a reused request token. */
    bool m_startAttempted = false;
    bool m_stopping = false;
    bool m_stopped = false;
    bool m_channelStarted = false;
    bool m_channelClosed = true;
    bool m_channelCloseRequested = false;
    bool m_channelCallbacksInstalled = false;
    bool m_coreStopDone = false;
    bool m_releaseCalled = false;
    bool m_stopRetryScheduled = false;
    bool m_stoppedSignalEmitted = false;
    bool m_helloAccepted = false;
    bool m_failed = false;
    QString m_lastError;

    ParseStage m_parseStage = ParseStage::Hello;
    std::array<std::uint8_t, kBrokerHelloSize> m_helloBuffer {};
    std::size_t m_helloOffset = 0;
    std::array<std::uint8_t, kWireHeaderSize> m_headerBuffer {};
    std::size_t m_headerOffset = 0;
    QByteArray m_payloadBuffer;
    std::size_t m_payloadOffset = 0;
    std::uint32_t m_expectedPayload = 0;
};

} // namespace RemoteUsb
