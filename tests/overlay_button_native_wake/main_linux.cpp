#include "streaming/video/overlaymenubutton.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

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

void sendPointerMotion(Display* display, const QPoint& globalPosition)
{
    require(XTestFakeMotionEvent(display,
                                 DefaultScreen(display),
                                 globalPosition.x(),
                                 globalPosition.y(),
                                 CurrentTime),
            "XTest pointer motion must be accepted");
    XSync(display, False);

    Window root = None;
    Window child = None;
    int rootX = 0;
    int rootY = 0;
    int windowX = 0;
    int windowY = 0;
    unsigned int state = 0;
    require(XQueryPointer(display,
                          DefaultRootWindow(display),
                          &root,
                          &child,
                          &rootX,
                          &rootY,
                          &windowX,
                          &windowY,
                          &state),
            "X11 pointer position must be queryable");
    require(rootX == globalPosition.x() && rootY == globalPosition.y(),
            "XTest pointer motion must reach the requested root coordinates");
}

void sendClick(Display* display, int count = 1)
{
    for (int i = 0; i < count; ++i) {
        require(XTestFakeButtonEvent(display, Button1, True, CurrentTime),
                "XTest button press must be accepted");
        require(XTestFakeButtonEvent(display, Button1, False, CurrentTime),
                "XTest button release must be accepted");
    }
    XSync(display, False);
}

QPoint nativeWindowCenter(Display* display, WId windowId)
{
    const Window window = static_cast<Window>(windowId);
    XWindowAttributes attributes = {};
    require(XGetWindowAttributes(display, window, &attributes),
            "overlay button X11 attributes must be available");
    require(attributes.map_state == IsViewable,
            "overlay button X11 window must be viewable");

    int rootX = 0;
    int rootY = 0;
    Window child = None;
    require(XTranslateCoordinates(display,
                                  window,
                                  DefaultRootWindow(display),
                                  attributes.width / 2,
                                  attributes.height / 2,
                                  &rootX,
                                  &rootY,
                                  &child),
            "overlay button center must translate to root coordinates");
    return QPoint(rootX, rootY);
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

    Display* display = XOpenDisplay(nullptr);
    require(display != nullptr, "X11 test display must be available");
    sendPointerMotion(display, QPoint(0, 0));

    OverlayMenuButton button;
    std::atomic_int wakeCount{0};
    std::atomic_int clickCount{0};
    QSemaphore wakeSemaphore;
    button.setEventWakeCallback([&wakeCount, &wakeSemaphore]() {
        wakeCount.fetch_add(1, std::memory_order_acq_rel);
        wakeSemaphore.release();
    });
    button.setClickCallback([&clickCount](const QPoint&, bool) {
        clickCount.fetch_add(1, std::memory_order_acq_rel);
    });
    button.showButton(100, 100, 800, 600);
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);

    require(button.winId() != 0, "overlay button must have an X11 window");
    const QPoint buttonCenter = nativeWindowCenter(display, button.winId());

    // Move onto the button and finish that Qt pass first. The click below is
    // then delivered while the pointer is stationary, which exercises X11's
    // implicit pointer grab instead of relying on an EnterNotify wakeup.
    sendPointerMotion(display, buttonCenter);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "real X11 pointer motion must wake the SDL owner loop");
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);

    const int settledWakeCount = wakeCount.load(std::memory_order_acquire);
    const int settledClickCount = clickCount.load(std::memory_order_acquire);
    sendClick(display);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "real X11 click must wake the SDL owner loop");
    require(button.needsEventProcessing(),
            "X11 button input must request Qt event processing");
    require(wakeCount.load(std::memory_order_acquire) == settledWakeCount + 1,
            "one X11 input batch must produce one wake edge");
    settleQtEvents(button);
    require(clickCount.load(std::memory_order_acquire) == settledClickCount + 1,
            "stationary X11 click must activate the overlay button");
    drainSemaphore(wakeSemaphore);

    const int pressureWakeCount = wakeCount.load(std::memory_order_acquire);
    sendClick(display, 100);
    QThread::msleep(20);
    require(wakeCount.load(std::memory_order_acquire) == pressureWakeCount + 1,
            "X11 input pressure must coalesce while a Qt pass is pending");
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);

    button.hideButton();
    const int hiddenWakeCount = wakeCount.load(std::memory_order_acquire);
    sendClick(display);
    QThread::msleep(20);
    require(wakeCount.load(std::memory_order_acquire) == hiddenWakeCount,
            "hidden button must detach its X11 event monitor");
    sendPointerMotion(display, QPoint(0, 0));

    button.showButton(100, 100, 800, 600);
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);
    const QPoint reattachedButtonCenter = nativeWindowCenter(display, button.winId());
    sendPointerMotion(display, reattachedButtonCenter);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "pointer motion must wake a re-shown overlay button");
    settleQtEvents(button);
    drainSemaphore(wakeSemaphore);
    const int reattachedWakeCount = wakeCount.load(std::memory_order_acquire);
    const int reattachedClickCount = clickCount.load(std::memory_order_acquire);
    sendClick(display);
    require(wakeSemaphore.tryAcquire(1, 1000),
            "re-shown button must reattach its X11 event monitor");
    require(wakeCount.load(std::memory_order_acquire) == reattachedWakeCount + 1,
            "one reattached X11 click batch must produce one wake edge");
    settleQtEvents(button);
    require(clickCount.load(std::memory_order_acquire) == reattachedClickCount + 1,
            "reattached X11 button must handle a real click");

    button.hideButton();
    button.setEventWakeCallback({});
    XCloseDisplay(display);
    qunsetenv("MOONLIGHT_DEVICE_LOCAL_SETTINGS_DIR");
    return 0;
}
