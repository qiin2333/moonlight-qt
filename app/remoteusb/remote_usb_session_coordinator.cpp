#include "remote_usb_session_coordinator.h"

#include "remote_usb_broker_client.h"
#include "remote_usb_libusb_adapter.h"
#include "remote_usb_session_binding.h"
#include "remote_usb_tls_channel.h"

#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"

#include <QMetaObject>
#include <QDeadlineTimer>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QReadLocker>
#include <QRandomGenerator>

#include <algorithm>
#include <array>
#include <memory>

namespace RemoteUsb {

namespace {

quint64 randomToken()
{
    quint64 token = 0;
    do {
        token = QRandomGenerator::system()->generate64();
    } while (token == 0);
    return token;
}

bool allZero(const QByteArray &value)
{
    return value.isEmpty() || std::all_of(value.cbegin(), value.cend(),
                                          [](char byte) { return byte == 0; });
}

} // namespace

class RemoteUsbSessionCoordinator::Worker final : public QObject
{
public:
    Worker(RemoteUsbSessionCoordinator *owner, NvComputer *computer)
        : m_owner(owner), m_computer(computer)
    {
    }

    /* Stop/shutdown requests can be delivered while fetch() is running a
     * nested QEventLoop.  Keep teardown deferred until the active worker
     * operation has returned, otherwise the request can destroy the network
     * manager (or binding) underneath its caller. */
    class OperationScope final
    {
    public:
        explicit OperationScope(Worker *worker) : m_worker(worker)
        {
            ++m_worker->m_operationDepth;
        }

        ~OperationScope()
        {
            m_worker->operationFinished();
        }

        Q_DISABLE_COPY(OperationScope)

    private:
        Worker *m_worker;
    };

    ~Worker() override
    {
        /* shutdown() drains the binding before the thread is joined. */
        m_binding = nullptr;
        m_channel = nullptr;
        m_brokerClient = nullptr;
        m_networkManager = nullptr;
    }

    void initialize()
    {
        try {
            if (m_networkManager == nullptr) {
                m_networkManager = new QNetworkAccessManager(this);
            }
            if (m_adapter == nullptr) {
                m_adapter = std::make_unique<RemoteUsbLibusbAdapter>();
            }
        } catch (...) {
            fail(0, QStringLiteral("Remote USB worker initialization failed"));
        }
    }

    void enumerateDevices()
    {
        if (m_shutdownRequested) {
            return;
        }
        if (m_operationDepth != 0 || m_binding != nullptr) {
            /* fetch() runs a nested event loop.  Do not re-enter the worker
             * state machine from that loop (or enumerate while a lease is
             * being torn down); let the current operation stop first. */
            m_pendingStart = false;
            m_enumeratePending = true;
            requestStop();
            return;
        }
        OperationScope operationScope(this);
        initialize();
        if (m_shutdownRequested) {
            return;
        }
        const quint64 operation = ++m_operation;
        m_owner->m_cancelRequested.store(false, std::memory_order_release);
        m_owner->postState(State::Enumerating);
        QString error;
        QVector<DeviceSnapshot> devices;
        if (m_adapter != nullptr) {
            devices = m_adapter->enumerate(&error);
        }
        if (!error.isEmpty()) {
            fail(operation, error);
            return;
        }
        m_owner->postDevices(std::move(devices));
        if (current(operation)) {
            m_owner->postState(State::Idle);
        }
    }

    void startLease(QByteArray preferredDeviceId)
    {
        if (m_shutdownRequested) {
            return;
        }
        if (m_operationDepth != 0) {
            /* A queued start can be delivered by the capability request's
             * nested QEventLoop.  Record the latest selection and cancel the
             * active operation; it will be restarted after its stack unwinds. */
            m_pendingDeviceId = std::move(preferredDeviceId);
            m_pendingStart = true;
            m_enumeratePending = false;
            requestStop();
            return;
        }
        OperationScope operationScope(this);
        initialize();
        if (m_shutdownRequested) {
            return;
        }
        if (m_binding != nullptr) {
            m_pendingDeviceId = std::move(preferredDeviceId);
            m_pendingStart = true;
            m_enumeratePending = false;
            requestStop();
            return;
        }

        const quint64 operation = ++m_operation;
        m_owner->postState(State::Opening);
        m_owner->m_cancelRequested.store(false, std::memory_order_release);

        QString error;
        if (m_adapter == nullptr || !m_adapter->isAvailable()) {
            fail(operation, QStringLiteral("Remote USB backend is unavailable"));
            return;
        }

        QVector<DeviceSnapshot> devices = m_adapter->enumerate(&error);
        if (!error.isEmpty()) {
            fail(operation, error);
            return;
        }
        m_owner->postDevices(devices);

        QVector<DeviceSnapshot> supported;
        supported.reserve(devices.size());
        for (const DeviceSnapshot &device : devices) {
            if (!device.hasIsochronousEndpoints) {
                supported.append(device);
            }
        }

        DeviceSnapshot selected;
        if (!preferredDeviceId.isEmpty()) {
            for (const DeviceSnapshot &device : devices) {
                if (device.deviceId == preferredDeviceId ||
                    device.busId == preferredDeviceId) {
                    selected = device;
                    break;
                }
            }
            if (selected.deviceId.isEmpty()) {
                fail(operation, QStringLiteral("Requested USB device is not present"));
                return;
            }
            if (selected.hasIsochronousEndpoints) {
                fail(operation, QStringLiteral("Isochronous USB devices are not supported"));
                return;
            }
        } else if (supported.size() == 1) {
            selected = supported.first();
        } else {
            fail(operation, supported.isEmpty()
                              ? QStringLiteral("No supported USB device is available")
                              : QStringLiteral("Select a USB device before starting Remote USB"));
            return;
        }

        if (!current(operation)) {
            return;
        }

        IdentityManager *identity = IdentityManager::get();
        if (identity == nullptr || m_computer == nullptr) {
            fail(operation, QStringLiteral("Remote USB client identity is unavailable"));
            return;
        }

        QSslCertificate serverCertificate;
        QString hostAddress;
        QString hostUuid;
        quint16 httpsPort = 0;
        bool useTrueUid = true;
        {
            QReadLocker locker(&m_computer->lock);
            serverCertificate = m_computer->serverCert;
            hostAddress = m_computer->activeAddress.address();
            hostUuid = m_computer->uuid;
            httpsPort = m_computer->activeHttpsPort;
            useTrueUid = !m_computer->isNvidiaServerSoftware;
        }
        const QString identityText = RemoteUsbBrokerClient::canonicalIdentity(
            useTrueUid ? identity->getUniqueId()
                       : QStringLiteral("0123456789ABCDEF"));
        const QByteArray wireIdentity =
            RemoteUsbBrokerClient::wireIdentity(identityText);
        if (wireIdentity.size() != 16 || allZero(wireIdentity)) {
            fail(operation, QStringLiteral("Remote USB client identity is invalid"));
            return;
        }

        BrokerHostConfig hostConfig;
        hostConfig.host = hostAddress;
        hostConfig.httpsPort = httpsPort;
        hostConfig.serverCertificate = serverCertificate;
        hostConfig.clientIdentity = identityText;
        hostConfig.clientName = NvComputer::getPairname(hostUuid);
        if (hostConfig.clientName.trimmed().isEmpty()) {
            hostConfig.clientName = QHostInfo::localHostName();
        }
        hostConfig.sslConfiguration = identity->getSslConfig();
        if (!hostConfig.valid()) {
            fail(operation, QStringLiteral("Remote USB paired host is not ready"));
            return;
        }

        BrokerCapabilityRequest request;
        request.streamGeneration = ++m_streamGeneration;
        if (request.streamGeneration == 0) {
            request.streamGeneration = ++m_streamGeneration;
        }
        request.sessionToken = randomToken();
        request.attachmentToken = randomToken();
        request.leaseToken = randomToken();

        if (m_brokerClient != nullptr) {
            delete m_brokerClient;
        }
        m_brokerClient = new RemoteUsbBrokerClient(
            std::move(hostConfig), m_networkManager, this);
        auto capability = m_brokerClient->fetch(request, 5000, &error);
        if (!capability || !current(operation)) {
            if (!current(operation)) {
                return;
            }
            fail(operation, error.isEmpty()
                              ? QStringLiteral("Remote USB capability request failed")
                              : error);
            return;
        }

        RemoteUsbBrokerHello hello;
        std::copy_n(reinterpret_cast<const std::uint8_t *>(wireIdentity.constData()),
                    hello.clientUuid.size(), hello.clientUuid.begin());
        hello.streamGeneration = request.streamGeneration;
        hello.sessionToken = request.sessionToken;
        hello.attachmentToken = request.attachmentToken;
        hello.leaseToken = request.leaseToken;
        if (capability->nonce.size() != 16) {
            fail(operation, QStringLiteral("Remote USB capability nonce is invalid"));
            return;
        }
        std::copy_n(reinterpret_cast<const std::uint8_t *>(capability->nonce.constData()),
                    hello.capabilityNonce.size(), hello.capabilityNonce.begin());
        hello.maxPdu = capability->maxUrb;
        hello.maxInflight = capability->maxInflight;
        hello.isochronous = false;

        RemoteUsbTlsChannelConfig channelConfig;
        channelConfig.host = capability->host;
        channelConfig.port = capability->port;
        channelConfig.pinnedServerCertificate = serverCertificate;
        channelConfig.sslConfiguration = identity->getSslConfig();

        RemoteUsbSessionBindingOptions bindingOptions;
        bindingOptions.brokerHello = hello;
        bindingOptions.txWindowBytes = capability->txWindowBytes;
        bindingOptions.txWindowPdus = capability->txWindowPdus;
        bindingOptions.rxWindowBytes = capability->rxWindowBytes;
        bindingOptions.rxWindowPdus = capability->rxWindowPdus;
        bindingOptions.maxReassemblySize = capability->maxReassemblySize;
        bindingOptions.maxFragments = capability->maxFragments;
        bindingOptions.maxInflight = capability->maxInflight;
        bindingOptions.maxTransferSize = capability->maxUrb - kPduHeaderSize;

        m_selected = std::move(selected);
        m_request = request;
        m_binding = nullptr;
        m_channel = new RemoteUsbTlsChannel(std::move(channelConfig), this);
        m_binding = new RemoteUsbSessionBinding(m_adapter.get(), m_channel,
                                                bindingOptions, this);

        QPointer<Worker> guard(this);
        connect(m_binding, &RemoteUsbSessionBinding::helloAccepted,
                this,
                [guard, operation]() {
                    if (!guard || !guard->current(operation) ||
                        guard->m_binding == nullptr) {
                        return;
                    }
                    QString sendError;
                    if (!guard->m_binding->sendCapability(guard->m_selected,
                                                          &sendError)) {
                        guard->fail(operation,
                                    sendError.isEmpty()
                                        ? QStringLiteral("Remote USB capability send failed")
                                        : sendError);
                    }
                },
                Qt::QueuedConnection);
        connect(m_binding, &RemoteUsbSessionBinding::openRequested,
                this,
                [guard, operation](quint64 lease, quint64 attachment) {
                    if (!guard || !guard->current(operation) ||
                        guard->m_binding == nullptr ||
                        lease != guard->m_request.leaseToken ||
                        attachment != guard->m_request.attachmentToken) {
                        return;
                    }
                    QString claimError;
                    if (!guard->m_adapter->claim(guard->m_selected, &claimError)) {
                        guard->m_binding->sendOpenReject(1, nullptr);
                        guard->fail(operation,
                                    claimError.isEmpty()
                                        ? QStringLiteral("Remote USB device claim failed")
                                        : claimError);
                        return;
                    }
                    QString openError;
                    if (!guard->m_binding->sendOpenOk(&openError)) {
                        guard->fail(operation,
                                    openError.isEmpty()
                                        ? QStringLiteral("Remote USB OPEN_OK failed")
                                        : openError);
                        return;
                    }
                    guard->m_owner->postOpened(guard->m_selected.deviceId);
                    guard->m_owner->postState(State::Open);
                },
                Qt::QueuedConnection);
        connect(m_binding, &RemoteUsbSessionBinding::openRejected,
                this,
                [guard, operation](quint32) {
                    if (guard && guard->current(operation)) {
                        guard->fail(operation,
                                    QStringLiteral("Remote USB host rejected OPEN"));
                    }
                },
                Qt::QueuedConnection);
        connect(m_binding, &RemoteUsbSessionBinding::peerClosed,
                this,
                [guard, operation](quint64) {
                    if (guard && guard->current(operation)) {
                        guard->requestStop();
                    }
                },
                Qt::QueuedConnection);
        connect(m_binding, &RemoteUsbSessionBinding::errorOccurred,
                this,
                [guard, operation](const QString &message) {
                    if (guard && guard->current(operation)) {
                        guard->fail(operation, message);
                    }
                },
                Qt::QueuedConnection);
        connect(m_binding, &RemoteUsbSessionBinding::stopped,
                this,
                [guard, operation]() {
                    if (guard) {
                        guard->bindingStopped(operation);
                    }
                },
                /* Let RemoteUsbSessionBinding::finishStop() return before
                 * bindingStopped() deletes the sender. */
                Qt::QueuedConnection);

        if (!m_binding->start(&error)) {
            fail(operation, error.isEmpty()
                              ? QStringLiteral("Remote USB session start failed")
                              : error);
        }
    }

    void stopWorker()
    {
        m_pendingStart = false;
        m_enumeratePending = false;
        requestStop();
    }

    void shutdown()
    {
        m_shutdownRequested = true;
        m_pendingStart = false;
        m_enumeratePending = false;
        requestStop();
    }

private:
    void operationFinished()
    {
        if (m_operationDepth == 0) {
            return;
        }
        --m_operationDepth;
        if (m_operationDepth == 0 && m_stopPending) {
            m_stopPending = false;
            requestStop();
        }
    }

    bool current(quint64 operation) const
    {
        return operation != 0 && operation == m_operation &&
               !m_shutdownRequested &&
               !m_owner->m_cancelRequested.load(std::memory_order_acquire);
    }

    void fail(quint64 operation, const QString &message)
    {
        if (operation != 0 && operation != m_operation) {
            return;
        }
        m_owner->postFailure(message);
        m_owner->postState(State::Failed);
        requestStop();
    }

    void requestStop()
    {
        m_owner->m_cancelRequested.store(true, std::memory_order_release);
        m_owner->postState(State::Stopping);
        if (m_operationDepth != 0) {
            m_stopPending = true;
            return;
        }
        if (m_binding != nullptr) {
            m_binding->stop();
        } else {
            cleanup(m_operation);
            if (m_pendingStart && !m_shutdownRequested) {
                const QByteArray pending = std::move(m_pendingDeviceId);
                m_pendingStart = false;
                startLease(pending);
            } else if (m_enumeratePending && !m_shutdownRequested) {
                m_enumeratePending = false;
                enumerateDevices();
            } else if (m_shutdownRequested) {
                emitShutdownComplete();
            } else {
                m_owner->postState(State::Idle);
                m_owner->postStopped();
                m_owner->notifyStopComplete();
            }
        }
    }

    void bindingStopped(quint64 operation)
    {
        Q_UNUSED(operation);
        cleanup(m_operation);
        if (m_pendingStart && !m_shutdownRequested) {
            const QByteArray pending = std::move(m_pendingDeviceId);
            m_pendingStart = false;
            m_enumeratePending = false;
            m_owner->m_cancelRequested.store(false, std::memory_order_release);
            startLease(pending);
            return;
        }
        if (m_enumeratePending && !m_shutdownRequested) {
            m_enumeratePending = false;
            m_owner->m_cancelRequested.store(false, std::memory_order_release);
            enumerateDevices();
            return;
        }
        if (m_shutdownRequested) {
            emitShutdownComplete();
            return;
        }
        m_owner->postState(State::Idle);
        m_owner->postStopped();
        m_owner->notifyStopComplete();
    }

    void cleanup(quint64)
    {
        if (m_adapter != nullptr) {
            /* The binding releases the adapter on its normal stop path, but
             * startup can fail before a binding owns a C session.  Keep the
             * adapter boundary idempotently released for both paths. */
            m_adapter->release();
        }
        if (m_binding != nullptr) {
            /* bindingStopped() is queued, so the stopped() signal has
             * returned before this direct deletion.  Keeping destruction on
             * the worker thread also guarantees that its borrowed channel and
             * C callbacks are already quiescent. */
            delete m_binding;
            m_binding = nullptr;
        }
        if (m_channel != nullptr) {
            delete m_channel;
            m_channel = nullptr;
        }
        if (m_brokerClient != nullptr) {
            delete m_brokerClient;
            m_brokerClient = nullptr;
        }
        m_selected = {};
        m_request = {};
    }

    void emitShutdownComplete()
    {
        if (m_shutdownComplete) {
            return;
        }
        m_shutdownComplete = true;
        cleanup(m_operation);
        /* These objects are children of Worker.  Delete them while this
         * method is still running on the worker thread; deleting them after
         * QThread::wait() would violate QObject thread affinity. */
        if (m_brokerClient != nullptr) {
            delete m_brokerClient;
            m_brokerClient = nullptr;
        }
        if (m_networkManager != nullptr) {
            delete m_networkManager;
            m_networkManager = nullptr;
        }
        m_adapter.reset();
        m_owner->postState(State::Idle);
        m_owner->notifyStopComplete();
        if (thread() != nullptr) {
            thread()->quit();
        }
    }

    RemoteUsbSessionCoordinator *m_owner = nullptr;
    NvComputer *m_computer = nullptr;
    QNetworkAccessManager *m_networkManager = nullptr;
    RemoteUsbBrokerClient *m_brokerClient = nullptr;
    std::unique_ptr<RemoteUsbLibusbAdapter> m_adapter;
    RemoteUsbTlsChannel *m_channel = nullptr;
    RemoteUsbSessionBinding *m_binding = nullptr;
    DeviceSnapshot m_selected;
    BrokerCapabilityRequest m_request;
    QByteArray m_pendingDeviceId;
    quint64 m_operation = 0;
    quint64 m_streamGeneration = 0;
    bool m_pendingStart = false;
    bool m_enumeratePending = false;
    bool m_shutdownRequested = false;
    quint32 m_operationDepth = 0;
    bool m_stopPending = false;
    bool m_shutdownComplete = false;
};

RemoteUsbSessionCoordinator::RemoteUsbSessionCoordinator(NvComputer *computer,
                                                         QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<DeviceSnapshot>();
    qRegisterMetaType<QVector<DeviceSnapshot>>();
    qRegisterMetaType<State>();
    m_worker = new Worker(this, computer);
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::started, m_worker,
            [worker = m_worker] { worker->initialize(); },
            Qt::QueuedConnection);
    m_workerThread.start();
}

RemoteUsbSessionCoordinator::~RemoteUsbSessionCoordinator()
{
    if (m_worker != nullptr) {
        m_cancelRequested.store(true, std::memory_order_release);
        m_stopAcknowledged.store(false, std::memory_order_release);
        const bool invoked = QMetaObject::invokeMethod(
            m_worker,
            [worker = m_worker] { worker->shutdown(); },
            Qt::QueuedConnection);
        if (!invoked) {
            m_stopAcknowledged.store(true, std::memory_order_release);
        }
        m_workerThread.wait();
        /* Worker::emitShutdownComplete() synchronously destroys all of its
         * children before quitting the thread, so the remaining QObject has
         * no affinity-bound resources and can be released here safely. */
        delete m_worker;
        m_worker = nullptr;
    }
}

void RemoteUsbSessionCoordinator::enumerate()
{
    if (m_worker == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] {
        worker->enumerateDevices();
    }, Qt::QueuedConnection);
}

void RemoteUsbSessionCoordinator::start(const QByteArray &deviceId)
{
    if (m_worker == nullptr) {
        return;
    }
    m_cancelRequested.store(false, std::memory_order_release);
    const QByteArray copy = deviceId;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, copy] {
        worker->startLease(copy);
    }, Qt::QueuedConnection);
}

void RemoteUsbSessionCoordinator::stop()
{
    m_cancelRequested.store(true, std::memory_order_release);
    m_stopAcknowledged.store(false, std::memory_order_release);
    if (m_worker == nullptr) {
        m_stopAcknowledged.store(true, std::memory_order_release);
        return;
    }
    if (QThread::currentThread() == &m_workerThread) {
        /* Avoid a self-deadlock for embedders that invoke stop() from a
         * worker callback. */
        m_worker->stopWorker();
        return;
    }
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker] { worker->stopWorker(); },
        Qt::QueuedConnection);
    if (!invoked) {
        m_stopAcknowledged.store(true, std::memory_order_release);
        m_stoppedCondition.wakeAll();
    }
}

bool RemoteUsbSessionCoordinator::stopAndWait(int timeoutMs)
{
    if (m_worker == nullptr || !m_workerThread.isRunning()) {
        return true;
    }
    if (QThread::currentThread() == &m_workerThread) {
        /* The worker cannot make progress while this thread is blocked.  The
        * caller is already on the serialized owner loop, so issue the stop
         * directly and let the queued stopped path finish asynchronously. */
        stop();
        return m_stopAcknowledged.load(std::memory_order_acquire);
    }
    m_stopAcknowledged.store(false, std::memory_order_release);
    stop();
    QMutexLocker locker(&m_waitMutex);
    const qint64 waitMs = qMax<qint64>(1, timeoutMs);
    QDeadlineTimer deadline(waitMs);
    while (!m_stopAcknowledged.load(std::memory_order_acquire)) {
        if (!m_stoppedCondition.wait(&m_waitMutex, deadline)) {
            break;
        }
    }
    return m_stopAcknowledged.load(std::memory_order_acquire);
}

void RemoteUsbSessionCoordinator::notifyStopComplete() noexcept
{
    m_stopAcknowledged.store(true, std::memory_order_release);
    m_stoppedCondition.wakeAll();
}

void RemoteUsbSessionCoordinator::postState(State state)
{
    m_state.store(state, std::memory_order_release);
    QPointer<RemoteUsbSessionCoordinator> guard(this);
    QMetaObject::invokeMethod(this, [guard, state] {
        if (guard) {
            emit guard->stateChanged(state);
        }
    }, Qt::QueuedConnection);
}

void RemoteUsbSessionCoordinator::postDevices(QVector<DeviceSnapshot> devices)
{
    QPointer<RemoteUsbSessionCoordinator> guard(this);
    QMetaObject::invokeMethod(this, [guard, devices = std::move(devices)]() mutable {
        if (guard) {
            emit guard->devicesChanged(std::move(devices));
        }
    }, Qt::QueuedConnection);
}

void RemoteUsbSessionCoordinator::postOpened(QByteArray deviceId)
{
    QPointer<RemoteUsbSessionCoordinator> guard(this);
    QMetaObject::invokeMethod(this, [guard, deviceId = std::move(deviceId)]() mutable {
        if (guard) {
            emit guard->opened(std::move(deviceId));
        }
    }, Qt::QueuedConnection);
}

void RemoteUsbSessionCoordinator::postStopped()
{
    QPointer<RemoteUsbSessionCoordinator> guard(this);
    QMetaObject::invokeMethod(this, [guard] {
        if (guard) {
            emit guard->stopped();
            guard->m_stoppedCondition.wakeAll();
        }
    }, Qt::QueuedConnection);
}

void RemoteUsbSessionCoordinator::postFailure(QString message)
{
    QPointer<RemoteUsbSessionCoordinator> guard(this);
    QMetaObject::invokeMethod(this, [guard, message = std::move(message)]() mutable {
        if (guard) {
            emit guard->failed(std::move(message));
        }
    }, Qt::QueuedConnection);
}

} // namespace RemoteUsb
