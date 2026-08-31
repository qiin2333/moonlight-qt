#include "streaming/video/overlaymenubutton.h"
#include "streaming/video/macqteventpumpinputguard.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QTemporaryDir>

#import <AppKit/AppKit.h>

#include <SDL.h>
#include <SDL_syswm.h>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        qFatal("%s", message);
    }
}

void settleQtEvents(OverlayMenuButton& button)
{
    for (int pass = 0; pass < 16 && button.needsEventProcessing(); ++pass) {
        button.beginEventProcessing();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    require(!button.needsEventProcessing(),
            "overlay button event processing must settle");
}

void sendPointerEvent(NSInteger windowNumber)
{
    NSEvent* event = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
                                        location:NSMakePoint(1, 1)
                                   modifierFlags:0
                                       timestamp:NSProcessInfo.processInfo.systemUptime
                                    windowNumber:windowNumber
                                         context:nil
                                     eventNumber:0
                                      clickCount:0
                                        pressure:0];
    [NSApp sendEvent:event];
}

void postKeyEvent(NSEventType type,
                  NSInteger windowNumber,
                  NSString* characters,
                  NSEventModifierFlags modifiers,
                  unsigned short keyCode)
{
    NSEvent* event = [NSEvent keyEventWithType:type
                                      location:NSZeroPoint
                                 modifierFlags:modifiers
                                     timestamp:NSProcessInfo.processInfo.systemUptime
                                  windowNumber:windowNumber
                                       context:nil
                                    characters:characters
                   charactersIgnoringModifiers:characters
                                      isARepeat:NO
                                        keyCode:keyCode];
    [NSApp postEvent:event atStart:NO];
}

void postMouseEvent(NSEventType type, NSInteger windowNumber)
{
    NSEvent* event = [NSEvent mouseEventWithType:type
                                        location:NSMakePoint(20, 20)
                                   modifierFlags:0
                                       timestamp:NSProcessInfo.processInfo.systemUptime
                                    windowNumber:windowNumber
                                         context:nil
                                     eventNumber:1
                                      clickCount:1
                                        pressure:type == NSEventTypeLeftMouseDown ? 1.0 : 0.0];
    [NSApp postEvent:event atStart:NO];
}

NSWindow* nativeWindowForSdlWindow(SDL_Window* window)
{
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    require(SDL_GetWindowWMInfo(window, &windowInfo),
            "SDL streaming window must expose native information");
    require(windowInfo.subsystem == SDL_SYSWM_COCOA,
            "SDL streaming window must use Cocoa");
    return windowInfo.info.cocoa.window;
}

void flushSdlEvents()
{
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}
}

int main(int argc, char* argv[])
{
    QTemporaryDir settingsDirectory;
    require(settingsDirectory.isValid(), "temporary settings directory must be available");
    qputenv("MOONLIGHT_DEVICE_LOCAL_SETTINGS_DIR",
            settingsDirectory.path().toLocal8Bit());

    QGuiApplication app(argc, argv);
    require(SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0,
            SDL_GetError());
    SDL_Window* streamingWindow = SDL_CreateWindow(
            "overlay input ownership test",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            320,
            240,
            SDL_WINDOW_SHOWN);
    require(streamingWindow != nullptr, SDL_GetError());
    NSWindow* streamingNativeWindow = nativeWindowForSdlWindow(streamingWindow);
    require(streamingNativeWindow != nil,
            "SDL streaming window must have a native Cocoa window");

    OverlayMenuButton button;
    int wakeCount = 0;
    button.setEventWakeCallback([&wakeCount]() { wakeCount++; });
    button.showButton(100, 100, 800, 600);
    settleQtEvents(button);

    NSView* nativeView = static_cast<NSView*>(reinterpret_cast<void*>(button.winId()));
    require(nativeView.window != nil, "overlay button must have a native macOS window");
    const NSInteger windowNumber = nativeView.window.windowNumber;

    const int settledWakeCount = wakeCount;
    sendPointerEvent(0);
    require(!button.needsEventProcessing(),
            "pointer input for another macOS window must not wake the overlay");
    require(wakeCount == settledWakeCount,
            "unrelated native pointer input must leave the owner loop idle");

    sendPointerEvent(windowNumber);
    require(button.needsEventProcessing(),
            "native macOS pointer input must request Qt event processing");
    require(wakeCount == settledWakeCount + 1,
            "the first native pointer event must wake the owner loop");

    constexpr int nativeEventCount = 100000;
    for (int i = 0; i < nativeEventCount; ++i) {
        sendPointerEvent(windowNumber);
    }
    require(wakeCount == settledWakeCount + 1,
            "native macOS pointer bursts must be coalesced while pending");

    settleQtEvents(button);
    require(wakeCount == settledWakeCount + 1,
            "settling native pointer input must not create an idle wake loop");

    button.hideButton();
    sendPointerEvent(windowNumber);
    require(!button.needsEventProcessing(),
            "hidden button must ignore native macOS pointer events");
    require(wakeCount == settledWakeCount + 1,
            "hidden button must detach its native event monitor");

    button.showButton(100, 100, 800, 600);
    settleQtEvents(button);
    nativeView = static_cast<NSView*>(reinterpret_cast<void*>(button.winId()));
    require(nativeView.window != nil,
            "re-shown overlay button must have a native macOS window");
    const int reattachedWakeCount = wakeCount;
    sendPointerEvent(nativeView.window.windowNumber);
    require(button.needsEventProcessing(),
            "re-shown button must reattach its macOS event monitor");
    require(wakeCount == reattachedWakeCount + 1,
            "reattached monitor must wake for native pointer input");

    {
        MacQtEventPumpInputGuard inputGuard(streamingWindow);
        flushSdlEvents();

        postKeyEvent(NSEventTypeKeyDown,
                     streamingNativeWindow.windowNumber,
                     @"a", 0, 0);
        postKeyEvent(NSEventTypeKeyUp,
                     streamingNativeWindow.windowNumber,
                     @"a", 0, 0);
        postKeyEvent(NSEventTypeFlagsChanged,
                     streamingNativeWindow.windowNumber,
                     @"",
                     NSEventModifierFlagShift,
                     56);
        inputGuard.beginEventProcessing();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        inputGuard.finishEventProcessing();

        SDL_PumpEvents();
        int aKeyDownCount = 0;
        int aKeyUpCount = 0;
        int shiftDownCount = 0;
        SDL_Event sdlEvent;
        while (SDL_PollEvent(&sdlEvent)) {
            if (sdlEvent.type == SDL_KEYDOWN &&
                    sdlEvent.key.keysym.scancode == SDL_SCANCODE_A) {
                aKeyDownCount++;
            }
            else if (sdlEvent.type == SDL_KEYUP &&
                     sdlEvent.key.keysym.scancode == SDL_SCANCODE_A) {
                aKeyUpCount++;
            }
            else if (sdlEvent.type == SDL_KEYDOWN &&
                     sdlEvent.key.keysym.scancode == SDL_SCANCODE_LSHIFT) {
                shiftDownCount++;
            }
        }
        require(aKeyDownCount == 1 && aKeyUpCount == 1,
                "Qt event processing must preserve one ordered SDL key press");
        require(shiftDownCount == 1,
                "Qt event processing must preserve SDL modifier changes");

        postMouseEvent(NSEventTypeMouseMoved,
                       streamingNativeWindow.windowNumber);
        postMouseEvent(NSEventTypeLeftMouseDown,
                       streamingNativeWindow.windowNumber);
        postMouseEvent(NSEventTypeLeftMouseUp,
                       streamingNativeWindow.windowNumber);
        inputGuard.beginEventProcessing();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        inputGuard.finishEventProcessing();

        SDL_PumpEvents();
        int motionCount = 0;
        int buttonDownCount = 0;
        int buttonUpCount = 0;
        while (SDL_PollEvent(&sdlEvent)) {
            if (sdlEvent.type == SDL_MOUSEMOTION) {
                motionCount++;
            }
            else if (sdlEvent.type == SDL_MOUSEBUTTONDOWN &&
                     sdlEvent.button.button == SDL_BUTTON_LEFT) {
                buttonDownCount++;
            }
            else if (sdlEvent.type == SDL_MOUSEBUTTONUP &&
                     sdlEvent.button.button == SDL_BUTTON_LEFT) {
                buttonUpCount++;
            }
        }
        require(motionCount == 1 && buttonDownCount == 1 && buttonUpCount == 1,
                "Qt event processing must preserve SDL window mouse input");

        // Pointer events for the Qt overlay are not owned by SDL and must not
        // be requeued into its event pump.
        postMouseEvent(NSEventTypeMouseMoved, nativeView.window.windowNumber);
        inputGuard.beginEventProcessing();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        inputGuard.finishEventProcessing();
        NSEvent* requeuedOverlayEvent = [NSApp
                nextEventMatchingMask:NSEventMaskMouseMoved
                untilDate:[NSDate distantPast]
                inMode:NSDefaultRunLoopMode
                dequeue:YES];
        require(requeuedOverlayEvent == nil,
                "Qt overlay pointer input must not be requeued for SDL");
    }

    SDL_DestroyWindow(streamingWindow);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

    qunsetenv("MOONLIGHT_DEVICE_LOCAL_SETTINGS_DIR");
    return 0;
}
