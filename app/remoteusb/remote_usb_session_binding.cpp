#include "remote_usb_session_binding.h"

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace RemoteUsb {

namespace {

constexpr qint32 kRejectedStatus = -95;
constexpr qint32 kNoDeviceStatus = -19;
constexpr qint32 kNotFoundStatus = -2;

QString statusMessage(const char *operation, quint32 status)
{
    return QStringLiteral("Remote USB %1 failed (status %2)")
        .arg(QString::fromLatin1(operation))
        .arg(status);
}

quint16 readLe16(const std::uint8_t *bytes)
{
    return static_cast<quint16>(bytes[0]) |
           static_cast<quint16>(static_cast<quint16>(bytes[1]) << 8u);
}

quint32 readLe32(const std::uint8_t *bytes)
{
    return static_cast<quint32>(bytes[0]) |
           (static_cast<quint32>(bytes[1]) << 8u) |
           (static_cast<quint32>(bytes[2]) << 16u) |
           (static_cast<quint32>(bytes[3]) << 24u);
}

void writeLe16(std::uint8_t *bytes, quint16 value)
{
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8u);
}

void writeLe32(std::uint8_t *bytes, quint32 value)
{
    for (unsigned int index = 0; index < 4u; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

void writeLe64(std::uint8_t *bytes, quint64 value)
{
    for (unsigned int index = 0; index < 8u; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

bool allZero(const std::array<std::uint8_t, kSetupPacketSize> &bytes)
{
    return std::all_of(bytes.cbegin(), bytes.cend(),
                       [](std::uint8_t byte) { return byte == 0u; });
}

bool sizeFitsQByteArray(std::size_t size)
{
    return size <= static_cast<std::size_t>(
        std::numeric_limits<qsizetype>::max());
}

} // namespace

struct RemoteUsbSessionBinding::SubmitGate {
    QPointer<RemoteUsbSessionBinding> binding;
    std::mutex mutex;
    bool callbackSeen = false;
    quint64 requestToken = 0;
    TransferRequest request;
};

struct RemoteUsbSessionBinding::CancelGate {
    QPointer<RemoteUsbSessionBinding> binding;
    std::mutex mutex;
    bool callbackSeen = false;
    quint64 requestToken = 0;
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
    m_payloadBuffer.reserve(static_cast<qsizetype>(kWireMaxPayload));
}

RemoteUsbSessionBinding::~RemoteUsbSessionBinding()
{
    if (QThread::currentThread() == thread() && !m_stopped) {
        stop();
    }
    if (m_channel != nullptr && m_channelCallbacksInstalled) {
        try {
            m_channel->setCallbacks({}, {}, {});
        } catch (...) {
            qWarning("Remote USB channel rejected callback cleanup");
        }
    }
    if (!m_releaseCalled && m_platform != nullptr) {
        m_platform->release();
        m_releaseCalled = true;
    }
    if (m_session != nullptr) {
        rusb_session_destroy(m_session);
        m_session = nullptr;
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
        if (QThread::currentThread() != thread()) {
            enqueueError(message);
            return;
        }
        m_lastError = message.isEmpty()
            ? QStringLiteral("Remote USB operation failed") : message;
        emit errorOccurred(m_lastError);
    } catch (...) {
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

bool RemoteUsbSessionBinding::checkStatus(quint32 status,
                                          const char *operation,
                                          QString *error)
{
    return status == RUSB_STATUS_OK
        ? true : setError(statusMessage(operation, status), error);
}

bool RemoteUsbSessionBinding::createSession(QString *error)
{
    if (m_session != nullptr) {
        return true;
    }
    const RemoteUsbBrokerHello &hello = m_options.brokerHello;
    rusb_session_config config {};
    config.size = sizeof(config);
    config.version = RUSB_CORE_ABI_VERSION;
    config.role = RUSB_ROLE_EXPORTER;
    std::copy(hello.clientUuid.cbegin(), hello.clientUuid.cend(),
              std::begin(config.client_uuid));
    config.stream_generation = hello.streamGeneration;
    config.session_token = hello.sessionToken;
    config.attachment_token = hello.attachmentToken;
    config.lease_token = hello.leaseToken;
    std::copy(hello.capabilityNonce.cbegin(), hello.capabilityNonce.cend(),
              std::begin(config.capability_nonce));
    config.max_pdu = hello.maxPdu;
    config.max_inflight = hello.maxInflight;
    config.isochronous = hello.isochronous ? 1u : 0u;
    config.tx_window_bytes = m_options.txWindowBytes;
    config.tx_window_pdus = m_options.txWindowPdus;
    config.rx_window_bytes = m_options.rxWindowBytes;
    config.rx_window_pdus = m_options.rxWindowPdus;
    config.max_reassembly_size = m_options.maxReassemblySize;
    config.max_fragments = m_options.maxFragments;
    config.max_transfer_size = m_options.maxTransferSize;
    return checkStatus(rusb_session_create(&config, &m_session),
                       "create", error);
}

void RemoteUsbSessionBinding::installChannelCallbacks()
{
    if (m_channelCallbacksInstalled || m_channel == nullptr) {
        return;
    }
    QPointer<RemoteUsbSessionBinding> guard(this);
    m_channel->setCallbacks(
        [guard](QByteArray bytes) {
            if (guard) {
                guard->enqueueBytes(std::move(bytes));
            }
        },
        [guard](QString message) {
            if (guard) {
                guard->enqueueError(std::move(message));
            }
        },
        [guard]() {
            if (guard) {
                guard->enqueueClosed();
            }
        });
    m_channelCallbacksInstalled = true;
}

bool RemoteUsbSessionBinding::start(QString *error)
{
    if (!onOwnerThread(error) || m_startAttempted || m_platform == nullptr ||
        m_channel == nullptr) {
        return setError(QStringLiteral("Remote USB session cannot be started"), error);
    }
    m_startAttempted = true;
    const ChannelCapabilities capabilities = m_channel->capabilities();
    if (!capabilities.usable() || rusb_core_abi_version() != RUSB_CORE_ABI_VERSION ||
        rusb_core_protocol_version() != kWireProtocolVersion) {
        return setError(QStringLiteral("Remote USB core/channel capabilities mismatch"),
                        error);
    }
    if (!createSession(error)) {
        return false;
    }
    installChannelCallbacks();
    if (!m_channel->start(error)) {
        rusb_session_destroy(m_session);
        m_session = nullptr;
        return setError(error != nullptr && !error->isEmpty()
                            ? *error : QStringLiteral("Remote USB channel start failed"),
                        error);
    }
    m_channelStarted = true;
    m_channelClosed = false;
    if (!checkStatus(rusb_session_start(m_session), "start", error) ||
        !drainEvents(error)) {
        stop();
        return false;
    }
    m_started = true;
    return true;
}

void RemoteUsbSessionBinding::enqueueBytes(QByteArray bytes)
{
    QPointer<RemoteUsbSessionBinding> guard(this);
    QMetaObject::invokeMethod(this, [guard, bytes = std::move(bytes)]() {
        if (guard) {
            guard->processBytes(bytes);
        }
    }, Qt::QueuedConnection);
}

void RemoteUsbSessionBinding::enqueueError(QString message)
{
    QPointer<RemoteUsbSessionBinding> guard(this);
    QMetaObject::invokeMethod(this, [guard, message = std::move(message)]() {
        if (guard) {
            guard->processChannelError(message);
        }
    }, Qt::QueuedConnection);
}

void RemoteUsbSessionBinding::enqueueClosed()
{
    QPointer<RemoteUsbSessionBinding> guard(this);
    QMetaObject::invokeMethod(this, [guard]() {
        if (guard) {
            guard->processChannelClosed();
        }
    }, Qt::QueuedConnection);
}

bool RemoteUsbSessionBinding::feedBytes(const QByteArray &bytes, QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    if (bytes.isEmpty()) {
        return true;
    }
    processBytes(bytes);
    if (m_failed) {
        if (error != nullptr) {
            *error = m_lastError;
        }
        return false;
    }
    return true;
}

void RemoteUsbSessionBinding::processBytes(const QByteArray &bytes)
{
    if (m_stopping || m_stopped || m_failed || m_session == nullptr) {
        return;
    }
    const auto *source = reinterpret_cast<const std::uint8_t *>(bytes.constData());
    std::size_t remaining = static_cast<std::size_t>(bytes.size());
    while (remaining != 0 && !m_failed) {
        if (m_parseStage == ParseStage::Hello) {
            const std::size_t count = std::min(remaining,
                m_helloBuffer.size() - m_helloOffset);
            std::memcpy(m_helloBuffer.data() + m_helloOffset, source, count);
            source += count;
            remaining -= count;
            m_helloOffset += count;
            if (m_helloOffset == m_helloBuffer.size()) {
                const quint32 status = rusb_session_accept_hello(
                    m_session, m_helloBuffer.data(), m_helloBuffer.size());
                if (status != RUSB_STATUS_OK) {
                    failProtocol(statusMessage("accept HELLO", status));
                    return;
                }
                m_helloAccepted = true;
                m_parseStage = ParseStage::Header;
                emit helloAccepted();
            }
            continue;
        }
        if (m_parseStage == ParseStage::Header) {
            const std::size_t count = std::min(remaining,
                m_headerBuffer.size() - m_headerOffset);
            std::memcpy(m_headerBuffer.data() + m_headerOffset, source, count);
            source += count;
            remaining -= count;
            m_headerOffset += count;
            if (m_headerOffset != m_headerBuffer.size()) {
                continue;
            }
            if (readLe32(m_headerBuffer.data()) != 0x42535552u ||
                m_headerBuffer[4] != 1u ||
                readLe16(m_headerBuffer.data() + 6u) != kWireHeaderSize) {
                failProtocol(QStringLiteral("Remote USB frame header is invalid"));
                return;
            }
            m_expectedPayload = readLe32(m_headerBuffer.data() + 12u);
            if (m_expectedPayload > kWireMaxPayload) {
                failProtocol(QStringLiteral("Remote USB frame payload is too large"));
                return;
            }
            m_payloadBuffer.resize(static_cast<qsizetype>(m_expectedPayload));
            m_payloadOffset = 0;
            m_parseStage = ParseStage::Payload;
        }
        if (m_parseStage == ParseStage::Payload) {
            const std::size_t count = std::min(
                remaining, static_cast<std::size_t>(m_expectedPayload) - m_payloadOffset);
            if (count != 0) {
                std::memcpy(m_payloadBuffer.data() + m_payloadOffset, source, count);
                source += count;
                remaining -= count;
                m_payloadOffset += count;
            }
            if (m_payloadOffset != m_expectedPayload) {
                continue;
            }
            QByteArray frame;
            frame.reserve(static_cast<qsizetype>(kWireHeaderSize + m_expectedPayload));
            frame.append(reinterpret_cast<const char *>(m_headerBuffer.data()),
                         static_cast<qsizetype>(m_headerBuffer.size()));
            frame.append(m_payloadBuffer);
            const quint32 status = rusb_session_accept_frame(
                m_session,
                reinterpret_cast<const std::uint8_t *>(frame.constData()),
                static_cast<std::size_t>(frame.size()));
            m_headerOffset = 0;
            m_payloadOffset = 0;
            m_expectedPayload = 0;
            m_payloadBuffer.clear();
            m_parseStage = ParseStage::Header;
            if (status != RUSB_STATUS_OK) {
                failProtocol(statusMessage("accept frame", status));
                return;
            }
            if (!drainEvents()) {
                failProtocol(m_lastError);
                return;
            }
        }
    }
}

bool RemoteUsbSessionBinding::drainEvents(QString *error)
{
    while (m_session != nullptr) {
        rusb_event event {};
        const quint32 status = rusb_session_next_event(m_session, &event);
        if (status != RUSB_STATUS_OK) {
            return setError(statusMessage("read event", status), error);
        }
        if (event.kind == RUSB_EVENT_NONE) {
            return true;
        }
        if (!handleEvent(event, error)) {
            return false;
        }
    }
    return true;
}

bool RemoteUsbSessionBinding::handleEvent(const rusb_event &event,
                                          QString *error)
{
    switch (event.kind) {
    case RUSB_EVENT_OUTPUT_HELLO:
        return sendWire(event.data, event.data_length, "HELLO", error);
    case RUSB_EVENT_OUTPUT_FRAME:
        if (!sendWire(event.data, event.data_length, "frame", error)) {
            return false;
        }
        if (event.reservation_id != 0 && (event.flags & 1u) == 0u) {
            return checkStatus(rusb_session_ack_output(
                                   m_session, event.reservation_id),
                               "ack output", error);
        }
        return true;
    case RUSB_EVENT_CAPABILITY: {
        DeviceSnapshot snapshot;
        if (!decodeCapability(event.data, event.data_length, &snapshot, error)) {
            return false;
        }
        emit capabilityReceived(snapshot);
        return true;
    }
    case RUSB_EVENT_OPEN:
        emit openRequested(event.reservation_id, event.pdu_id);
        return true;
    case RUSB_EVENT_OPENED:
        emit openAccepted();
        return true;
    case RUSB_EVENT_OPEN_REJECTED:
        emit openRejected(static_cast<quint32>(event.status));
        return true;
    case RUSB_EVENT_SUBMIT:
        return dispatchSubmit(event, error);
    case RUSB_EVENT_CANCEL:
        return dispatchCancel(event, error);
    case RUSB_EVENT_OPAQUE_PDU:
        if (!sizeFitsQByteArray(event.data_length)) {
            return setError(QStringLiteral("Remote USB PDU is too large"), error);
        }
        emit pduReceived(event.pdu_id,
            QByteArray(reinterpret_cast<const char *>(event.data),
                       static_cast<qsizetype>(event.data_length)));
        return true;
    case RUSB_EVENT_CLOSED:
        emit peerClosed(m_options.brokerHello.leaseToken);
        return true;
    default:
        return setError(QStringLiteral("Remote USB core emitted an unknown event"),
                        error);
    }
}

bool RemoteUsbSessionBinding::sendWire(const std::uint8_t *wire,
                                       std::size_t wireSize,
                                       const char *kind,
                                       QString *error)
{
    if (m_channel == nullptr || wire == nullptr || wireSize == 0 ||
        !sizeFitsQByteArray(wireSize)) {
        return setError(QStringLiteral("Remote USB %1 output is invalid")
                            .arg(QString::fromLatin1(kind)), error);
    }
    const QByteArray bytes(reinterpret_cast<const char *>(wire),
                           static_cast<qsizetype>(wireSize));
    QString channelError;
    if (!m_channel->send(bytes, &channelError)) {
        return setError(channelError.isEmpty()
                            ? QStringLiteral("Remote USB channel rejected %1")
                                  .arg(QString::fromLatin1(kind))
                            : channelError,
                        error);
    }
    return true;
}

bool RemoteUsbSessionBinding::sendCapability(const DeviceSnapshot &device,
                                             QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    const QByteArray payload = encodeCapability(
        device, m_options.brokerHello.leaseToken,
        m_options.brokerHello.attachmentToken, error);
    if (payload.isEmpty()) {
        return false;
    }
    if (!checkStatus(rusb_session_send_capability(
                         m_session,
                         reinterpret_cast<const std::uint8_t *>(payload.constData()),
                         static_cast<std::size_t>(payload.size())),
                     "send capability", error)) {
        return false;
    }
    return drainEvents(error);
}

bool RemoteUsbSessionBinding::sendOpen(quint64 leaseToken,
                                       quint64 attachmentToken,
                                       QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error)) {
        return false;
    }
    if (leaseToken != m_options.brokerHello.leaseToken ||
        attachmentToken != m_options.brokerHello.attachmentToken) {
        return setError(QStringLiteral("Remote USB OPEN tokens do not match"), error);
    }
    if (!checkStatus(rusb_session_send_open(m_session), "send OPEN", error)) {
        return false;
    }
    return drainEvents(error);
}

bool RemoteUsbSessionBinding::sendOpenOk(QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error) ||
        !checkStatus(rusb_session_send_open_result(m_session, 0),
                     "send OPEN_OK", error)) {
        return false;
    }
    return drainEvents(error);
}

bool RemoteUsbSessionBinding::sendOpenReject(quint32 status, QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error) || status == 0 ||
        !checkStatus(rusb_session_send_open_result(m_session, status),
                     "send OPEN_REJECT", error)) {
        return false;
    }
    return drainEvents(error);
}

bool RemoteUsbSessionBinding::sendClose(QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error) ||
        !checkStatus(rusb_session_close(m_session), "send CLOSE", error)) {
        return false;
    }
    return drainEvents(error);
}

bool RemoteUsbSessionBinding::sendPdu(quint64 pduId, const QByteArray &pdu,
                                      QString *error)
{
    if (!onOwnerThread(error) || !checkSession(error) || pdu.isEmpty()) {
        return false;
    }
    if (!checkStatus(rusb_session_send_pdu(
                         m_session, pduId,
                         reinterpret_cast<const std::uint8_t *>(pdu.constData()),
                         static_cast<std::size_t>(pdu.size())),
                     "send PDU", error)) {
        return false;
    }
    return drainEvents(error);
}

bool RemoteUsbSessionBinding::dispatchSubmit(const rusb_event &event,
                                             QString *error)
{
    if (m_platform == nullptr || event.request_token == 0 ||
        event.sequence == 0 || event.direction > 1u || event.endpoint > 15u ||
        event.transfer_buffer_length > kMaxTransferSize ||
        !sizeFitsQByteArray(event.data_length) ||
        (event.data_length != 0 && event.data == nullptr)) {
        return setError(QStringLiteral("Remote USB submit event is invalid"), error);
    }
    TransferRequest request;
    request.requestToken = event.request_token;
    request.sequence = event.sequence;
    request.deviceId = event.device_id;
    request.direction = event.direction == 1u
        ? TransferDirection::In : TransferDirection::Out;
    request.endpoint = event.endpoint;
    request.transferFlags = event.transfer_flags;
    request.transferBufferLength = static_cast<qint32>(event.transfer_buffer_length);
    request.startFrame = event.start_frame;
    request.numberOfPackets = 0;
    request.interval = event.interval;
    std::copy(std::begin(event.setup), std::end(event.setup), request.setup.begin());
    if (event.data_length != 0) {
        request.data = QByteArray(reinterpret_cast<const char *>(event.data),
                                  static_cast<qsizetype>(event.data_length));
    }
    request.kind = request.endpoint == 0
        ? TransferKind::Control : TransferKind::Data;
    if (!validateTransfer(request, error)) {
        return false;
    }

    Endpoint endpoint;
    QString adapterError;
    EndpointResolution resolution = EndpointResolution::Rejected;
    try {
        resolution = m_platform->resolveEndpoint(request, &endpoint, &adapterError);
    } catch (...) {
        adapterError = QStringLiteral("Remote USB endpoint lookup failed");
    }
    if (resolution != EndpointResolution::Found ||
        !validateEndpoint(request, endpoint, &adapterError)) {
        TransferCompletion completion;
        completion.status = resolution == EndpointResolution::NotFound
            ? kNoDeviceStatus : kRejectedStatus;
        queueCompletion(request.requestToken, request, completion);
        return true;
    }
    request.endpointMetadata = endpoint;
    request.endpointMetadataValid = true;
    m_requests.insert(request.requestToken, request);

    auto gate = std::make_shared<SubmitGate>();
    gate->binding = this;
    gate->requestToken = request.requestToken;
    gate->request = request;
    TransferCompletionCallback callback =
        [gate](quint64 token, TransferCompletion completion) {
            bool duplicate = false;
            {
                std::lock_guard<std::mutex> lock(gate->mutex);
                duplicate = gate->callbackSeen;
                gate->callbackSeen = true;
            }
            if (!gate->binding) {
                return;
            }
            if (duplicate || token != gate->requestToken) {
                gate->binding->notifyError(
                    QStringLiteral("Remote USB completion callback is invalid"));
                return;
            }
            gate->binding->queueCompletion(token, gate->request,
                                           std::move(completion));
        };

    SubmitDisposition disposition = SubmitDisposition::Rejected;
    try {
        disposition = request.kind == TransferKind::Control
            ? m_platform->submitControl(request, std::move(callback), &adapterError)
            : m_platform->submitData(request, std::move(callback), &adapterError);
    } catch (const std::exception &exception) {
        adapterError = QString::fromUtf8(exception.what());
    } catch (...) {
        adapterError = QStringLiteral("Remote USB platform submit failed");
    }
    bool callbackSeen = false;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        callbackSeen = gate->callbackSeen;
    }
    if (disposition == SubmitDisposition::Completed && !callbackSeen) {
        adapterError = QStringLiteral("Remote USB synchronous submit omitted completion");
        disposition = SubmitDisposition::Rejected;
    }
    if (disposition == SubmitDisposition::Rejected && !callbackSeen) {
        TransferCompletion completion;
        completion.status = kRejectedStatus;
        queueCompletion(request.requestToken, request, completion);
    }
    if (disposition == SubmitDisposition::Rejected && !adapterError.isEmpty()) {
        m_lastError = adapterError;
    }
    return true;
}

bool RemoteUsbSessionBinding::dispatchCancel(const rusb_event &event,
                                             QString *error)
{
    const auto requestIt = m_requests.constFind(event.request_token);
    if (requestIt == m_requests.cend()) {
        queueCancelCompletion(event.request_token, kNotFoundStatus);
        return true;
    }
    const TransferRequest request = *requestIt;
    auto gate = std::make_shared<CancelGate>();
    gate->binding = this;
    gate->requestToken = event.request_token;
    CancelCompletionCallback callback = [gate](quint64 token, qint32 status) {
        bool duplicate = false;
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            duplicate = gate->callbackSeen;
            gate->callbackSeen = true;
        }
        if (!gate->binding) {
            return;
        }
        if (duplicate || token != gate->requestToken) {
            gate->binding->notifyError(
                QStringLiteral("Remote USB cancel callback is invalid"));
            return;
        }
        gate->binding->queueCancelCompletion(token, status);
    };

    qint32 status = kRejectedStatus;
    QString adapterError;
    CancelDisposition disposition = CancelDisposition::Failed;
    try {
        disposition = m_platform->cancel(request, std::move(callback),
                                         &status, &adapterError);
    } catch (const std::exception &exception) {
        adapterError = QString::fromUtf8(exception.what());
    } catch (...) {
        adapterError = QStringLiteral("Remote USB platform cancel failed");
    }
    bool callbackSeen = false;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        callbackSeen = gate->callbackSeen;
    }
    if (disposition != CancelDisposition::Pending && !callbackSeen) {
        if (disposition == CancelDisposition::NotFound) {
            status = kNotFoundStatus;
        } else if (disposition == CancelDisposition::Failed && status >= 0) {
            status = kRejectedStatus;
        }
        queueCancelCompletion(event.request_token, status);
    }
    if (disposition == CancelDisposition::Failed && !adapterError.isEmpty()) {
        m_lastError = adapterError;
    }
    Q_UNUSED(error);
    return true;
}

void RemoteUsbSessionBinding::queueCompletion(
    quint64 requestToken, const TransferRequest &request,
    TransferCompletion completion)
{
    QPointer<RemoteUsbSessionBinding> guard(this);
    QMetaObject::invokeMethod(this,
        [guard, requestToken, request, completion = std::move(completion)]() mutable {
            if (!guard || guard->m_session == nullptr || guard->m_stopped) {
                return;
            }
            if (!validateCompletion(request, completion, nullptr)) {
                completion = {};
                completion.status = kNoDeviceStatus;
            }
            rusb_completion value {};
            value.size = sizeof(value);
            value.version = RUSB_CORE_ABI_VERSION;
            value.status = completion.status;
            value.actual_length = static_cast<quint32>(completion.actualLength);
            value.start_frame = completion.startFrame;
            value.error_count = completion.errorCount;
            value.data = completion.data.isEmpty()
                ? nullptr
                : reinterpret_cast<const std::uint8_t *>(completion.data.constData());
            value.data_length = static_cast<std::size_t>(completion.data.size());
            const quint32 status = rusb_session_complete(
                guard->m_session, requestToken, &value);
            guard->m_requests.remove(requestToken);
            if (status != RUSB_STATUS_OK && status != RUSB_STATUS_NOT_FOUND) {
                guard->failProtocol(statusMessage("complete", status));
                return;
            }
            if (!guard->drainEvents()) {
                guard->failProtocol(guard->m_lastError);
            }
        }, Qt::QueuedConnection);
}

void RemoteUsbSessionBinding::queueCancelCompletion(quint64 requestToken,
                                                    qint32 status)
{
    QPointer<RemoteUsbSessionBinding> guard(this);
    QMetaObject::invokeMethod(this, [guard, requestToken, status]() {
        if (!guard || guard->m_session == nullptr || guard->m_stopped) {
            return;
        }
        const quint32 result = rusb_session_complete_cancel(
            guard->m_session, requestToken, status);
        guard->m_requests.remove(requestToken);
        if (result != RUSB_STATUS_OK && result != RUSB_STATUS_NOT_FOUND) {
            guard->failProtocol(statusMessage("cancel complete", result));
            return;
        }
        if (!guard->drainEvents()) {
            guard->failProtocol(guard->m_lastError);
        }
    }, Qt::QueuedConnection);
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
    if (request.endpoint > 15u || request.transferBufferLength < 0 ||
        static_cast<std::size_t>(request.transferBufferLength) > kMaxTransferSize ||
        request.startFrame < 0 || request.interval < 0 ||
        request.numberOfPackets != 0) {
        return reject(QStringLiteral("Remote USB transfer fields are invalid"));
    }
    const qsizetype expected = static_cast<qsizetype>(request.transferBufferLength);
    if ((request.direction == TransferDirection::Out && request.data.size() != expected) ||
        (request.direction == TransferDirection::In && !request.data.isEmpty())) {
        return reject(QStringLiteral("Remote USB transfer payload is invalid"));
    }
    if (request.endpoint == 0) {
        const bool setupIn = (request.setup[0] & 0x80u) != 0u;
        if (setupIn != (request.direction == TransferDirection::In) ||
            request.transferBufferLength > std::numeric_limits<quint16>::max() ||
            readLe16(request.setup.data() + 6u) !=
                static_cast<quint16>(request.transferBufferLength)) {
            return reject(QStringLiteral("Remote USB control setup is inconsistent"));
        }
    } else if (!allZero(request.setup)) {
        return reject(QStringLiteral("Remote USB data transfer has a setup packet"));
    }
    return true;
}

bool RemoteUsbSessionBinding::validateEndpoint(const TransferRequest &request,
                                               const Endpoint &endpoint,
                                               QString *error)
{
    const quint8 expectedAddress = request.endpoint == 0
        ? 0u : static_cast<quint8>((request.endpoint & 0x0fu) |
              (request.direction == TransferDirection::In ? 0x80u : 0u));
    if (endpoint.reserved != 0 || endpoint.address != expectedAddress ||
        (request.endpoint != 0 && endpoint.maxPacketSize == 0)) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB endpoint metadata is invalid");
        }
        return false;
    }
    return true;
}

bool RemoteUsbSessionBinding::validateCompletion(
    const TransferRequest &request, const TransferCompletion &completion,
    QString *error)
{
    if (completion.actualLength < 0 || completion.startFrame < 0 ||
        completion.errorCount < 0 ||
        completion.actualLength > request.transferBufferLength ||
        (request.direction == TransferDirection::Out && !completion.data.isEmpty()) ||
        (request.direction == TransferDirection::In &&
         completion.data.size() != completion.actualLength)) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB completion is invalid");
        }
        return false;
    }
    return true;
}

QByteArray RemoteUsbSessionBinding::encodeCapability(
    const DeviceSnapshot &device, quint64 leaseToken, quint64 attachmentToken,
    QString *error)
{
    if (leaseToken == 0 || attachmentToken == 0 || device.busId.isEmpty() ||
        device.busId.size() > static_cast<qsizetype>(kBusIdMaxBytes) ||
        device.busId.contains('\0') || device.rawDescriptors.isEmpty() ||
        device.rawDescriptors.size() > static_cast<qsizetype>(kRawDescriptorMaxBytes) ||
        device.endpoints.size() > static_cast<qsizetype>(kEndpointMaxCount)) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB capability is invalid");
        }
        return {};
    }
    const qsizetype size = 34 + device.busId.size() +
        device.rawDescriptors.size() + device.endpoints.size() * 8;
    QByteArray payload(size, 0);
    auto *wire = reinterpret_cast<std::uint8_t *>(payload.data());
    writeLe64(wire, leaseToken);
    writeLe64(wire + 8u, attachmentToken);
    writeLe16(wire + 16u, device.vendorId);
    writeLe16(wire + 18u, device.productId);
    writeLe16(wire + 20u, device.deviceBcd);
    wire[22] = device.deviceClass;
    wire[23] = device.deviceSubclass;
    wire[24] = device.deviceProtocol;
    wire[25] = static_cast<std::uint8_t>(device.busId.size());
    writeLe16(wire + 26u, static_cast<quint16>(device.endpoints.size()));
    writeLe32(wire + 30u, static_cast<quint32>(device.rawDescriptors.size()));
    qsizetype offset = 34;
    std::memcpy(payload.data() + offset, device.busId.constData(),
                static_cast<std::size_t>(device.busId.size()));
    offset += device.busId.size();
    std::memcpy(payload.data() + offset, device.rawDescriptors.constData(),
                static_cast<std::size_t>(device.rawDescriptors.size()));
    offset += device.rawDescriptors.size();
    for (const Endpoint &endpoint : device.endpoints) {
        if (endpoint.reserved != 0) {
            if (error != nullptr) {
                *error = QStringLiteral("Remote USB endpoint reserved byte is non-zero");
            }
            return {};
        }
        auto *record = reinterpret_cast<std::uint8_t *>(payload.data() + offset);
        record[0] = endpoint.interfaceNumber;
        record[1] = endpoint.alternateSetting;
        record[2] = endpoint.address;
        record[3] = endpoint.attributes;
        writeLe16(record + 4u, endpoint.maxPacketSize);
        record[6] = endpoint.interval;
        offset += 8;
    }
    return payload;
}

bool RemoteUsbSessionBinding::decodeCapability(
    const std::uint8_t *payload, std::size_t payloadSize,
    DeviceSnapshot *snapshot, QString *error)
{
    if (payload == nullptr || snapshot == nullptr || payloadSize < 34u ||
        readLe16(payload + 28u) != 0u) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB capability payload is invalid");
        }
        return false;
    }
    const std::size_t busSize = payload[25];
    const std::size_t endpointCount = readLe16(payload + 26u);
    const std::size_t descriptorSize = readLe32(payload + 30u);
    const std::size_t expected = 34u + busSize + descriptorSize + endpointCount * 8u;
    if (busSize == 0 || busSize > kBusIdMaxBytes || descriptorSize == 0 ||
        descriptorSize > kRawDescriptorMaxBytes || endpointCount > kEndpointMaxCount ||
        expected != payloadSize ||
        std::find(payload + 34u, payload + 34u + busSize, 0u) !=
            payload + 34u + busSize) {
        if (error != nullptr) {
            *error = QStringLiteral("Remote USB capability lengths are invalid");
        }
        return false;
    }
    *snapshot = {};
    snapshot->vendorId = readLe16(payload + 16u);
    snapshot->productId = readLe16(payload + 18u);
    snapshot->deviceBcd = readLe16(payload + 20u);
    snapshot->deviceClass = payload[22];
    snapshot->deviceSubclass = payload[23];
    snapshot->deviceProtocol = payload[24];
    snapshot->busId = QByteArray(reinterpret_cast<const char *>(payload + 34u),
                                 static_cast<qsizetype>(busSize));
    snapshot->deviceId = snapshot->busId;
    const std::size_t descriptorsOffset = 34u + busSize;
    snapshot->rawDescriptors = QByteArray(
        reinterpret_cast<const char *>(payload + descriptorsOffset),
        static_cast<qsizetype>(descriptorSize));
    const std::size_t endpointsOffset = descriptorsOffset + descriptorSize;
    snapshot->endpoints.reserve(static_cast<qsizetype>(endpointCount));
    for (std::size_t index = 0; index < endpointCount; ++index) {
        const std::uint8_t *record = payload + endpointsOffset + index * 8u;
        if (record[7] != 0u) {
            if (error != nullptr) {
                *error = QStringLiteral("Remote USB endpoint payload is invalid");
            }
            return false;
        }
        Endpoint endpoint;
        endpoint.interfaceNumber = record[0];
        endpoint.alternateSetting = record[1];
        endpoint.address = record[2];
        endpoint.attributes = record[3];
        endpoint.maxPacketSize = readLe16(record + 4u);
        endpoint.interval = record[6];
        snapshot->endpoints.append(endpoint);
    }
    return true;
}

void RemoteUsbSessionBinding::processChannelError(const QString &message)
{
    if (!m_stopping && !m_stopped) {
        failProtocol(message.isEmpty()
                         ? QStringLiteral("Remote USB channel failed") : message);
    }
}

void RemoteUsbSessionBinding::processChannelClosed()
{
    m_channelClosed = true;
    m_channelStarted = false;
    if (!m_stopping && !m_stopped) {
        m_stopping = true;
    }
    finishStop();
}

void RemoteUsbSessionBinding::failProtocol(const QString &message)
{
    if (m_failed || m_stopped) {
        return;
    }
    m_failed = true;
    notifyError(message.isEmpty()
                    ? QStringLiteral("Remote USB protocol failed") : message);
    stop();
}

void RemoteUsbSessionBinding::stop() noexcept
{
    if (QThread::currentThread() != thread()) {
        QPointer<RemoteUsbSessionBinding> guard(this);
        QMetaObject::invokeMethod(this, [guard]() {
            if (guard) {
                guard->stop();
            }
        }, Qt::QueuedConnection);
        return;
    }
    if (m_stopped || m_stopping) {
        return;
    }
    m_stopping = true;
    if (m_session != nullptr) {
        const quint32 status = rusb_session_close(m_session);
        if (status == RUSB_STATUS_OK) {
            drainEvents();
        }
    }
    if (m_channel != nullptr && m_channelStarted && !m_channelCloseRequested) {
        m_channelCloseRequested = true;
        m_channel->close();
    } else {
        m_channelClosed = true;
    }
    finishStop();
}

void RemoteUsbSessionBinding::finishStop()
{
    if (!m_stopping || m_stopped || !m_channelClosed) {
        return;
    }
    if (!m_releaseCalled && m_platform != nullptr) {
        m_platform->release();
        m_releaseCalled = true;
    }
    m_requests.clear();
    if (m_session != nullptr) {
        rusb_session_destroy(m_session);
        m_session = nullptr;
    }
    m_started = false;
    m_stopped = true;
    if (!m_stoppedSignalEmitted) {
        m_stoppedSignalEmitted = true;
        emit stopped();
    }
}

quint32 RemoteUsbSessionBinding::state() const noexcept
{
    return m_session == nullptr ? 7u : rusb_session_state(m_session);
}

} // namespace RemoteUsb
