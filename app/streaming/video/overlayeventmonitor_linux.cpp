#include "overlayeventmonitor_linux.h"

#include <QGuiApplication>
#include <QThread>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/qguiapplication_platform.h>
#else
#include <qpa/qplatformnativeinterface.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

#if defined(HAVE_XCB_DISPLAY_MONITOR)
#define USE_XCB_DISPLAY_MONITOR
#endif

#ifdef USE_XCB_DISPLAY_MONITOR
#include <xcb/xcb.h>
#endif

namespace {
struct DisplaySource
{
    int fd = -1;
};

DisplaySource qtDisplaySource()
{
    auto* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!guiApp) {
        return {};
    }

#ifdef USE_XCB_DISPLAY_MONITOR
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
#if QT_CONFIG(xcb)
    if (auto* x11 = guiApp->nativeInterface<QNativeInterface::QX11Application>()) {
        if (xcb_connection_t* connection = x11->connection()) {
            return { xcb_get_file_descriptor(connection) };
        }
    }
#endif
#endif
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QPlatformNativeInterface* nativeInterface =
            QGuiApplication::platformNativeInterface();
    if (!nativeInterface) {
        return {};
    }

#ifdef USE_XCB_DISPLAY_MONITOR
    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
        auto* connection = static_cast<xcb_connection_t*>(
                nativeInterface->nativeResourceForIntegration("connection"));
        if (connection) {
            return { xcb_get_file_descriptor(connection) };
        }
    }
#endif
#endif

    return {};
}

void signalControlFileDescriptor(int fd)
{
    const std::uint64_t value = 1;
    ssize_t result;
    do {
        result = write(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}

void drainControlFileDescriptor(int fd)
{
    std::uint64_t value;
    ssize_t result;
    do {
        result = read(fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
}

void invokeWakeCallback(const std::function<void()>& callback)
{
    if (callback) {
        callback();
    }
}
}

struct LinuxDisplayEventMonitor::State
{
    State(int displayFd,
          int controlFd,
          std::function<void()> wakeCallback)
        : displayFd(displayFd),
          controlFd(controlFd),
          wakeCallback(std::move(wakeCallback))
    {
    }

    int displayFd;
    int controlFd;
    std::function<void()> wakeCallback;
    std::atomic_bool stopping{false};
    std::atomic_bool attached{true};
    std::atomic_bool wakeOutstanding{false};
    QThread* thread = nullptr;
};

LinuxDisplayEventMonitor::LinuxDisplayEventMonitor(
        std::function<void()> wakeCallback,
        DisplayFdProvider displayFdProvider)
    : m_WakeCallback(std::move(wakeCallback)),
      m_DisplayFdProvider(std::move(displayFdProvider))
{
}

LinuxDisplayEventMonitor::~LinuxDisplayEventMonitor()
{
    detach();
}

bool LinuxDisplayEventMonitor::attach(std::uintptr_t nativeWindow)
{
    Q_UNUSED(nativeWindow);

    if (isAttached()) {
        return true;
    }

    detach();

    DisplaySource displaySource;
    displaySource.fd = m_DisplayFdProvider ?
            m_DisplayFdProvider() : -1;
    if (!m_DisplayFdProvider) {
        displaySource = qtDisplaySource();
    }
    if (displaySource.fd < 0) {
        return false;
    }

    const int controlFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (controlFd < 0) {
        return false;
    }

    m_State = std::make_unique<State>(displaySource.fd, controlFd, m_WakeCallback);
    State* state = m_State.get();
    state->thread = QThread::create([state]() {
        while (!state->stopping.load(std::memory_order_acquire)) {
            // The display connection belongs to Qt. The monitor only observes
            // readability and never consumes native events from this thread.
            // Disable polling until the Qt pass is acknowledged so the unread
            // descriptor cannot cause a busy loop.
            pollfd descriptors[2] = {
                {
                    state->displayFd,
                    static_cast<short>(state->wakeOutstanding.load(
                            std::memory_order_acquire) ? 0 : POLLIN),
                    0
                },
                { state->controlFd, POLLIN, 0 }
            };

            int result;
            do {
                result = poll(descriptors, 2, -1);
            } while (result < 0 && errno == EINTR);

            if (result < 0) {
                state->attached.store(false, std::memory_order_release);
                invokeWakeCallback(state->wakeCallback);
                break;
            }

            if (descriptors[1].revents & POLLIN) {
                drainControlFileDescriptor(state->controlFd);
            }
            if (state->stopping.load(std::memory_order_acquire)) {
                break;
            }

            if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                state->attached.store(false, std::memory_order_release);
                invokeWakeCallback(state->wakeCallback);
                break;
            }

            const bool receivedDisplayEvent = descriptors[0].revents & POLLIN;
            if (receivedDisplayEvent &&
                    !state->wakeOutstanding.exchange(true, std::memory_order_acq_rel)) {
                invokeWakeCallback(state->wakeCallback);
            }
        }
    });
    state->thread->setObjectName(QStringLiteral("Linux overlay event monitor"));
    state->thread->start();
    return true;
}

void LinuxDisplayEventMonitor::detach()
{
    if (!m_State) {
        return;
    }

    std::unique_ptr<State> state = std::move(m_State);
    state->attached.store(false, std::memory_order_release);
    state->stopping.store(true, std::memory_order_release);
    signalControlFileDescriptor(state->controlFd);
    state->thread->wait();
    delete state->thread;
    close(state->controlFd);
}

bool LinuxDisplayEventMonitor::isAttached() const
{
    return m_State && m_State->attached.load(std::memory_order_acquire);
}

void LinuxDisplayEventMonitor::finishEventProcessing()
{
    if (!m_State) {
        return;
    }

    // A display disconnect or poll failure wakes the owner once so it can
    // switch to continuous Qt processing. Release the stopped monitor here
    // instead of retaining its display connection until the button is hidden.
    if (!m_State->attached.load(std::memory_order_acquire)) {
        detach();
        return;
    }

    if (m_State->wakeOutstanding.exchange(false, std::memory_order_acq_rel)) {
        signalControlFileDescriptor(m_State->controlFd);
    }
}
