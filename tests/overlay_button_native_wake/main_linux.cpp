#include "streaming/video/overlaymenubutton.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <xcb/xcb.h>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        qFatal("%s", message);
    }
}

void settleQtEvents(OverlayMenuButton& button)
{
    int quietPasses = 0;
    for (int pass = 0; pass < 64 && quietPasses < 3; ++pass) {
        if (button.needsEventProcessing()) {
            quietPasses = 0;
            button.beginEventProcessing();
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            button.finishEventProcessing();
        }
        else {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            quietPasses++;
        }
        QThread::msleep(2);
    }
    require(!button.needsEventProcessing(),
            "Linux overlay event processing must settle");
}

void drainSemaphore(QSemaphore& semaphore)
{
    while (semaphore.tryAcquire(1)) {
    }
}

xcb_screen_t* defaultScreen(xcb_connection_t* connection)
{
    xcb_screen_iterator_t iterator =
            xcb_setup_roots_iterator(xcb_get_setup(connection));
    return iterator.data;
}

void sendButtonRelease(xcb_connection_t* connection,
                       xcb_window_t window,
                       int count = 1)
{
    xcb_screen_t* screen = defaultScreen(connection);
    require(screen != nullptr, "X11 test connection must have a default screen");

    for (int i = 0; i < count; ++i) {
        xcb_button_release_event_t event = {};
        event.response_type = XCB_BUTTON_RELEASE;
        event.detail = 1;
        event.root = screen->root;
        event.event = window;
        event.root_x = 1;
        event.root_y = 1;
        event.event_x = 1;
        event.event_y = 1;
        event.same_screen = 1;
        const xcb_void_cookie_t cookie = xcb_send_event(
                connection,
                false,
                window,
                XCB_EVENT_MASK_BUTTON_RELEASE,
                reinterpret_cast<const char*>(&event));
        require(cookie.sequence != 0,
                "X11 button release must be queued");
    }
    require(xcb_flush(connection) > 0, "X11 button release must be flushed");
}
}

int main(int argc, char* argv[])
{
    QTemporaryDir settingsDirectory;
    require(settingsDirectory.isValid(),
            "temporary settings directory must be available");
    qputenv("MOONLIGHT_DEVICE_LOCAL_SETTINGS_DIR",
            settingsDirectory.path().toLocal8Bit());

    QGuiApplication app(argc, argv);
    require(QGuiApplication::platformName() == QStringLiteral("xcb"),
            "Linux native wake test must run on the XCB platform");

    xcb_connection_t* sender = xcb_connect(nullptr, nullptr);
    require(sender != nullptr && !xcb_connection_has_error(sender),
            "X11 sender connection must be available");

    OverlayMenuButton button;
    std::atomic_int wakeCount{0};
    QSemaphore wakeSemaphore;
    button.setEventWakeCallback([&wakeCount, &wakeSemaphore]() {
        wakeCount.fetch_add(1, std::memory_order_acq_rel);
        wakeSemaphore.release();
    });
    button.showButton(100, 100, 800, 600);
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);

    const auto window = static_cast<xcb_window_t>(button.winId());
    require(window != XCB_WINDOW_NONE, "overlay button must have an X11 window");

    const int settledWakeCount = wakeCount.load(std::memory_order_acquire);
    sendButtonRelease(sender, window);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "X11 button input must wake the SDL owner loop");
    require(button.needsEventProcessing(),
            "X11 button input must request Qt event processing");
    require(wakeCount.load(std::memory_order_acquire) == settledWakeCount + 1,
            "one X11 input batch must produce one wake edge");

    sendButtonRelease(sender, window, 1000);
    QThread::msleep(20);
    require(wakeCount.load(std::memory_order_acquire) == settledWakeCount + 1,
            "X11 input pressure must coalesce while a Qt pass is pending");
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);

    button.hideButton();
    const int hiddenWakeCount = wakeCount.load(std::memory_order_acquire);
    sendButtonRelease(sender, window);
    QThread::msleep(20);
    require(wakeCount.load(std::memory_order_acquire) == hiddenWakeCount,
            "hidden button must detach its X11 event monitor");

    button.showButton(100, 100, 800, 600);
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);
    const int reattachedWakeCount = wakeCount.load(std::memory_order_acquire);
    sendButtonRelease(sender, static_cast<xcb_window_t>(button.winId()));
    require(wakeSemaphore.tryAcquire(1, 1000),
            "re-shown button must reattach its X11 event monitor");
    require(wakeCount.load(std::memory_order_acquire) == reattachedWakeCount + 1,
            "reattached X11 monitor must produce a new wake edge");

    button.hideButton();
    button.setEventWakeCallback({});
    xcb_disconnect(sender);
    qunsetenv("MOONLIGHT_DEVICE_LOCAL_SETTINGS_DIR");
    return 0;
}
