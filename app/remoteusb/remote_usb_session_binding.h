#pragma once

/*
 * Qt owner-loop binding for the Rust Remote USB core.
 *
 * The Rust core owns protocol state only. This class owns the adaptation to
 * Qt byte channels and platform USB callbacks; no QObject, socket, USB handle,
 * or platform callback crosses the C ABI.
 */

#include "remote_usb_platform_adapter.h"

#include <QObject>
#include <QHash>

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "remoteusb.h"
}

namespace RemoteUsb {

struct RemoteUsbBrokerHello {
    std::array<std::uint8_t, 16> clientUuid {};
    quint64 streamGeneration = 0;
    quint64 sessionToken = 0;
    quint64 attachmentToken = 0;
    quint64 leaseToken = 0;
    std::array<std::uint8_t, 16> capabilityNonce {};
    quint32 maxPdu = 0;
    quint32 maxInflight = 0;
    bool isochronous = false;
};

struct RemoteUsbSessionBindingOptions {
    RemoteUsbBrokerHello brokerHello;
    quint64 txWindowBytes = 0;
    quint32 txWindowPdus = 0;
    quint64 rxWindowBytes = 0;
    quint32 rxWindowPdus = 0;
    quint32 maxReassemblySize = 0;
    quint32 maxFragments = 0;
    quint32 maxInflight = 0;
    quint32 maxTransferSize = 0;
};

class RemoteUsbSessionBinding final : public QObject
{
    Q_OBJECT

public:
    RemoteUsbSessionBinding(RemoteUsbPlatformAdapter *platform,
                            RemoteUsbByteChannel *channel,
                            const RemoteUsbSessionBindingOptions &options,
                            QObject *parent = nullptr);
    ~RemoteUsbSessionBinding() override;

    Q_DISABLE_COPY(RemoteUsbSessionBinding)

    bool start(QString *error = nullptr);
    void stop() noexcept;

    bool isStarted() const noexcept { return m_started; }
    bool isStopping() const noexcept { return m_stopping; }
    bool isStopped() const noexcept { return m_stopped; }
    quint32 state() const noexcept;
    QString lastError() const { return m_lastError; }

    bool feedBytes(const QByteArray &bytes, QString *error = nullptr);
    bool sendCapability(const DeviceSnapshot &device, QString *error = nullptr);
    bool sendOpen(quint64 leaseToken, quint64 attachmentToken,
                  QString *error = nullptr);
    bool sendOpenOk(QString *error = nullptr);
    bool sendOpenReject(quint32 status, QString *error = nullptr);
    bool sendClose(QString *error = nullptr);
    bool sendPdu(quint64 pduId, const QByteArray &pdu,
                 QString *error = nullptr);

signals:
    void capabilityReceived(RemoteUsb::DeviceSnapshot capability);
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
    bool checkStatus(quint32 status, const char *operation,
                     QString *error = nullptr);
    void failProtocol(const QString &message);

    bool createSession(QString *error);
    void installChannelCallbacks();
    void enqueueBytes(QByteArray bytes);
    void enqueueError(QString message);
    void enqueueClosed();
    void processBytes(const QByteArray &bytes);
    void processChannelError(const QString &message);
    void processChannelClosed();
    void finishStop();

    bool drainEvents(QString *error = nullptr);
    bool handleEvent(const rusb_event &event, QString *error);
    bool sendWire(const std::uint8_t *wire, std::size_t wireSize,
                  const char *kind, QString *error = nullptr);
    bool dispatchSubmit(const rusb_event &event, QString *error);
    bool dispatchCancel(const rusb_event &event, QString *error);

    void queueCompletion(quint64 requestToken, const TransferRequest &request,
                         TransferCompletion completion);
    void queueCancelCompletion(quint64 requestToken, qint32 status);

    static bool validateTransfer(const TransferRequest &request,
                                 QString *error = nullptr);
    static bool validateEndpoint(const TransferRequest &request,
                                 const Endpoint &endpoint,
                                 QString *error = nullptr);
    static bool validateCompletion(const TransferRequest &request,
                                   const TransferCompletion &completion,
                                   QString *error = nullptr);
    static bool decodeCapability(const std::uint8_t *payload,
                                 std::size_t payloadSize,
                                 DeviceSnapshot *snapshot,
                                 QString *error = nullptr);
    static QByteArray encodeCapability(const DeviceSnapshot &device,
                                       quint64 leaseToken,
                                       quint64 attachmentToken,
                                       QString *error = nullptr);

    RemoteUsbPlatformAdapter *m_platform = nullptr;
    RemoteUsbByteChannel *m_channel = nullptr;
    RemoteUsbSessionBindingOptions m_options;
    rusb_session *m_session = nullptr;
    QHash<quint64, TransferRequest> m_requests;

    bool m_started = false;
    bool m_startAttempted = false;
    bool m_stopping = false;
    bool m_stopped = false;
    bool m_channelStarted = false;
    bool m_channelClosed = true;
    bool m_channelCloseRequested = false;
    bool m_channelCallbacksInstalled = false;
    bool m_releaseCalled = false;
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
