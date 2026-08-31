#pragma once

/*
 * High-level Qt lifecycle for one client-side Remote USB lease.
 *
 * The coordinator is the only object a session/UI needs to know about.  It
 * owns a persistent worker thread containing the HTTPS control request, TLS
 * byte channel, shared C binding, and native USB adapter.  No socket, native
 * USB handle, or token is exposed to callers.
 */

#include "remote_usb_platform_adapter.h"

#include <QObject>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>

#include <atomic>

class NvComputer;

namespace RemoteUsb {

class RemoteUsbSessionCoordinator final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Enumerating,
        Opening,
        Open,
        Stopping,
        Failed,
    };
    Q_ENUM(State)

    explicit RemoteUsbSessionCoordinator(NvComputer *computer,
                                         QObject *parent = nullptr);
    ~RemoteUsbSessionCoordinator() override;

    Q_DISABLE_COPY(RemoteUsbSessionCoordinator)

    State state() const noexcept
    {
        return m_state.load(std::memory_order_acquire);
    }
    bool isOpen() const noexcept { return state() == State::Open; }

    /* All public operations are asynchronous and safe from any Qt thread. */
    Q_INVOKABLE void enumerate();
    /* An empty device id is accepted only when exactly one supported device is
     * present.  This avoids silently claiming a keyboard among several USB
     * devices while keeping the one-device case low-friction. */
    Q_INVOKABLE void start(const QByteArray &deviceId = {});
    Q_INVOKABLE void stop();

    /* Used by Session cleanup before handing the object to deletion. */
    bool stopAndWait(int timeoutMs = 7000);

signals:
    void devicesChanged(QVector<RemoteUsb::DeviceSnapshot> devices);
    void opened(QByteArray deviceId);
    void stopped();
    void failed(QString message);
    void stateChanged(RemoteUsb::RemoteUsbSessionCoordinator::State state);

private:
    class Worker;

    void postState(State state);
    void postDevices(QVector<DeviceSnapshot> devices);
    void postOpened(QByteArray deviceId);
    void postStopped();
    void postFailure(QString message);
    void notifyStopComplete() noexcept;

    QThread m_workerThread;
    Worker *m_worker = nullptr;
    std::atomic<State> m_state { State::Idle };
    QMutex m_waitMutex;
    QWaitCondition m_stoppedCondition;
    std::atomic_bool m_cancelRequested { false };
    /* A stop request is acknowledged by the worker after its binding,
     * channel, and adapter have quiesced.  It is intentionally independent of
     * the public state: Idle can be observed while a queued stop is still
     * waiting to run. */
    std::atomic_bool m_stopAcknowledged { true };
};

} // namespace RemoteUsb

Q_DECLARE_METATYPE(RemoteUsb::DeviceSnapshot)
Q_DECLARE_METATYPE(QVector<RemoteUsb::DeviceSnapshot>)
Q_DECLARE_METATYPE(RemoteUsb::RemoteUsbSessionCoordinator::State)
