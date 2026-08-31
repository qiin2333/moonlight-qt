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
#include <cstdlib>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

#if defined(HAVE_XCB_DISPLAY_MONITOR)
#define USE_XCB_DISPLAY_MONITOR
#endif

#if defined(HAVE_WAYLAND_DISPLAY_MONITOR)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#if QT_CONFIG(wayland)
#define USE_WAYLAND_DISPLAY_MONITOR
#endif
#else
#define USE_WAYLAND_DISPLAY_MONITOR
#endif
#endif

#ifdef USE_XCB_DISPLAY_MONITOR
#include <xcb/xcb.h>
#endif

#ifdef USE_WAYLAND_DISPLAY_MONITOR
#include <wayland-client.h>
#endif

namespace {
struct DisplaySource
{
    int fd = -1;
    std::function<bool()> drainEvents;
    std::function<void()> close;
};

DisplaySource qtDisplaySource(std::uintptr_t nativeWindow)
{
    auto* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!guiApp) {
        return {};
    }

#ifdef USE_XCB_DISPLAY_MONITOR
    if (QGuiApplication::platformName() == QStringLiteral("xcb") &&
            nativeWindow != 0) {
        xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
        if (!connection || xcb_connection_has_error(connection)) {
            if (connection) {
                xcb_disconnect(connection);
            }
            return {};
        }

        const std::uint32_t eventMask =
                // ButtonPress is exclusive on X11 and is already selected by
                // Qt. A release still wakes taps, while motion wakes drags.
                XCB_EVENT_MASK_BUTTON_RELEASE |
                XCB_EVENT_MASK_POINTER_MOTION |
                XCB_EVENT_MASK_BUTTON_MOTION |
                XCB_EVENT_MASK_ENTER_WINDOW |
                XCB_EVENT_MASK_LEAVE_WINDOW |
                XCB_EVENT_MASK_EXPOSURE |
                XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                XCB_EVENT_MASK_VISIBILITY_CHANGE |
                XCB_EVENT_MASK_FOCUS_CHANGE;
        const xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(
                connection,
                static_cast<xcb_window_t>(nativeWindow),
                XCB_CW_EVENT_MASK,
                &eventMask);
        if (xcb_generic_error_t* error = xcb_request_check(connection, cookie)) {
            std::free(error);
            xcb_disconnect(connection);
            return {};
        }
        if (xcb_flush(connection) <= 0) {
            xcb_disconnect(connection);
            return {};
        }

        DisplaySource source;
        source.fd = xcb_get_file_descriptor(connection);
        if (source.fd < 0) {
            xcb_disconnect(connection);
            return {};
        }
        source.drainEvents = [connection]() {
            bool receivedEvent = false;
            while (xcb_generic_event_t* event = xcb_poll_for_event(connection)) {
                receivedEvent = true;
                std::free(event);
            }
            return receivedEvent;
        };
        source.close = [connection]() { xcb_disconnect(connection); };
        return source;
    }
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#if defined(USE_WAYLAND_DISPLAY_MONITOR) && \
        QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (auto* wayland = guiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
        if (wl_display* display = wayland->display()) {
            return { wl_display_get_fd(display), {}, {} };
        }
    }
#endif
#else
    QPlatformNativeInterface* nativeInterface =
            QGuiApplication::platformNativeInterface();
    if (!nativeInterface) {
        return {};
    }

#ifdef USE_WAYLAND_DISPLAY_MONITOR
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        auto* display = static_cast<wl_display*>(
                nativeInterface->nativeResourceForIntegration("display"));
        if (display) {
            return { wl_display_get_fd(display), {}, {} };
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
    State(DisplaySource displaySource,
          int controlFd,
          std::function<void()> wakeCallback)
        : displayFd(displaySource.fd),
          controlFd(controlFd),
          drainEvents(std::move(displaySource.drainEvents)),
          closeDisplaySource(std::move(displaySource.close)),
          wakeCallback(std::move(wakeCallback))
    {
    }

    int displayFd;
    int controlFd;
    std::function<bool()> drainEvents;
    std::function<void()> closeDisplaySource;
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
    if (isAttached()) {
        return true;
    }

    detach();

    DisplaySource displaySource;
    displaySource.fd = m_DisplayFdProvider ?
            m_DisplayFdProvider() : -1;
    if (!m_DisplayFdProvider) {
        displaySource = qtDisplaySource(nativeWindow);
    }
    if (displaySource.fd < 0) {
        return false;
    }

    const int controlFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (controlFd < 0) {
        return false;
    }

    m_State = std::make_unique<State>(
            std::move(displaySource), controlFd, m_WakeCallback);
    State* state = m_State.get();
    state->thread = QThread::create([state]() {
        while (!state->stopping.load(std::memory_order_acquire)) {
            // Wayland keeps the Qt-owned connection untouched and only uses
            // poll() as a wake hint. X11 uses an independent connection and
            // drains only its duplicate monitor events. Disable display
            // polling until the Qt pass is acknowledged so an unread Wayland
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

            const bool receivedDisplayEvent = (descriptors[0].revents & POLLIN) &&
                    (!state->drainEvents || state->drainEvents());
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
    if (state->closeDisplaySource) {
        state->closeDisplaySource();
    }
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

    if (m_State->wakeOutstanding.exchange(false, std::memory_order_acq_rel)) {
        signalControlFileDescriptor(m_State->controlFd);
    }
}
