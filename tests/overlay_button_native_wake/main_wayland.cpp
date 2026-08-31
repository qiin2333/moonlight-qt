#include "streaming/video/overlaymenubutton.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QtGui/qguiapplication_platform.h>

#include <atomic>
#include <cstdint>
#include <wayland-client.h>

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
            "Wayland overlay event processing must settle");
}

void drainSemaphore(QSemaphore& semaphore)
{
    while (semaphore.tryAcquire(1)) {
    }
}

struct SyncState
{
    bool completed = false;
};

void handleSyncDone(void* data, wl_callback* callback, std::uint32_t)
{
    static_cast<SyncState*>(data)->completed = true;
    wl_callback_destroy(callback);
}

const wl_callback_listener kSyncListener = {
    handleSyncDone
};

void requestDisplaySync(wl_display* display, SyncState& state)
{
    wl_callback* callback = wl_display_sync(display);
    require(callback != nullptr, "Wayland display sync callback must be created");
    require(wl_callback_add_listener(callback, &kSyncListener, &state) == 0,
            "Wayland display sync listener must be installed");
    require(wl_display_flush(display) >= 0,
            "Wayland display sync request must be flushed");
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
    require(QGuiApplication::platformName().startsWith(QStringLiteral("wayland")),
            "Wayland native wake test must run on the Wayland platform");

    auto* wayland = app.nativeInterface<QNativeInterface::QWaylandApplication>();
    require(wayland != nullptr, "Qt must expose its Wayland native interface");
    wl_display* display = wayland->display();
    require(display != nullptr, "Qt must expose its Wayland display connection");

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

    const int settledWakeCount = wakeCount.load(std::memory_order_acquire);
    SyncState firstSync;
    requestDisplaySync(display, firstSync);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "readable Qt Wayland connection must wake the SDL owner loop");
    settleQtEvents(button);
    require(firstSync.completed, "Qt must dispatch the Wayland sync response");
    require(wakeCount.load(std::memory_order_acquire) == settledWakeCount + 1,
            "one Wayland display batch must produce one wake edge");
    drainSemaphore(wakeSemaphore);

    button.hideButton();
    const int hiddenWakeCount = wakeCount.load(std::memory_order_acquire);
    SyncState hiddenSync;
    requestDisplaySync(display, hiddenSync);
    QThread::msleep(20);
    require(wakeCount.load(std::memory_order_acquire) == hiddenWakeCount,
            "hidden button must detach its Wayland display monitor");
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    require(hiddenSync.completed,
            "Qt must remain able to process Wayland events while monitor is detached");

    button.showButton(100, 100, 800, 600);
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);
    const int reattachedWakeCount = wakeCount.load(std::memory_order_acquire);
    SyncState reattachedSync;
    requestDisplaySync(display, reattachedSync);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "re-shown button must reattach its Wayland display monitor");
    settleQtEvents(button);
    require(reattachedSync.completed,
            "Qt must dispatch Wayland events after monitor reattachment");
    require(wakeCount.load(std::memory_order_acquire) == reattachedWakeCount + 1,
            "reattached Wayland monitor must produce a new wake edge");

    button.hideButton();
    button.setEventWakeCallback({});
    qunsetenv("MOONLIGHT_DEVICE_LOCAL_SETTINGS_DIR");
    return 0;
}
