#include "streaming/video/overlayeventmonitor_linux.h"

#include <QCoreApplication>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        qFatal("%s", message);
    }
}

void signalEventFd(int fd)
{
    const std::uint64_t value = 1;
    ssize_t result;
    do {
        result = write(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    require(result == static_cast<ssize_t>(sizeof(value)),
            "display eventfd must accept a wake signal");
}

void drainEventFd(int fd)
{
    std::uint64_t value;
    ssize_t result;
    do {
        result = read(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    require(result == static_cast<ssize_t>(sizeof(value)),
            "display eventfd must contain a wake signal");
}
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    const int displayFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    require(displayFd >= 0, "display eventfd must be available");

    std::atomic_int wakeCount{0};
    QSemaphore wakeSemaphore;
    LinuxDisplayEventMonitor monitor(
            [&wakeCount, &wakeSemaphore]() {
                wakeCount.fetch_add(1, std::memory_order_acq_rel);
                wakeSemaphore.release();
            },
            [displayFd]() { return displayFd; });

    require(monitor.attach(), "display event monitor must attach to a valid fd");
    require(monitor.isAttached(), "attached monitor must report active state");

    signalEventFd(displayFd);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "readable display fd must wake the owner loop");

    for (int i = 0; i < 100; ++i) {
        signalEventFd(displayFd);
    }
    QThread::msleep(20);
    require(wakeCount.load(std::memory_order_acquire) == 1,
            "display event bursts must coalesce until Qt finishes processing");

    drainEventFd(displayFd);
    monitor.finishEventProcessing();
    QThread::msleep(20);
    require(wakeCount.load(std::memory_order_acquire) == 1,
            "drained display fd must remain idle after re-arming");

    signalEventFd(displayFd);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "re-armed monitor must wake for the next display event");
    require(wakeCount.load(std::memory_order_acquire) == 2,
            "each drained display batch must produce one wake edge");

    monitor.detach();
    require(!monitor.isAttached(), "detached monitor must report inactive state");
    signalEventFd(displayFd);
    QThread::msleep(20);
    require(wakeCount.load(std::memory_order_acquire) == 2,
            "detached monitor must ignore later display events");

    drainEventFd(displayFd);
    require(monitor.attach(), "detached monitor must support reattachment");
    signalEventFd(displayFd);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "reattached monitor must wake for display events");
    require(wakeCount.load(std::memory_order_acquire) == 3,
            "reattached monitor must produce a new wake edge");
    monitor.detach();

    // Qt's native reader may have drained the display socket while leaving
    // input queued for the owner thread. No POLLIN is available in that case.
    drainEventFd(displayFd);
    require(monitor.attach(), "queued-event fallback monitor must attach");
    const int queuedWakeCount = wakeCount.load(std::memory_order_acquire);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "Qt-buffered input must get a bounded wake without display readability");
    require(wakeCount.load(std::memory_order_acquire) == queuedWakeCount + 1,
            "queued-event fallback must produce one wake edge");
    require(!wakeSemaphore.tryAcquire(1, 250),
            "fallback wakes must coalesce until the owner acknowledges processing");
    monitor.finishEventProcessing();
    require(wakeSemaphore.tryAcquire(1, 1000),
            "queued-event fallback must re-arm after the owner drains Qt");
    monitor.detach();
    require(!wakeSemaphore.tryAcquire(1, 250), "detaching must stop queued-event fallback wakes");

    close(displayFd);

    LinuxDisplayEventMonitor unavailableMonitor([]() {}, []() { return -1; });
    require(!unavailableMonitor.attach(),
            "monitor must reject an unavailable display connection");
    require(!unavailableMonitor.isAttached(),
            "failed attachment must preserve the continuous-pump fallback");
    return 0;
}
