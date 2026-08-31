#include "macqteventpumpinputguard.h"

#import <AppKit/AppKit.h>

#include <QCoreApplication>
#include <QDebug>

#include <SDL.h>
#include <SDL_syswm.h>

namespace {
NSWindow* nativeWindowForSdlWindow(SDL_Window* streamingWindow)
{
    if (!streamingWindow) {
        return nil;
    }

    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (!SDL_GetWindowWMInfo(streamingWindow, &windowInfo) ||
            windowInfo.subsystem != SDL_SYSWM_COCOA) {
        return nil;
    }
    return windowInfo.info.cocoa.window;
}

bool isKeyboardEvent(NSEventType type)
{
    return type == NSEventTypeKeyDown ||
            type == NSEventTypeKeyUp ||
            type == NSEventTypeFlagsChanged;
}

bool isMouseEventHandledBySdl(NSEventType type)
{
    switch (type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
    case NSEventTypeMouseMoved:
    case NSEventTypeScrollWheel:
        return true;
    default:
        return false;
    }
}
}

MacQtEventPumpInputGuard::MacQtEventPumpInputGuard(SDL_Window* streamingWindow)
{
    NSWindow* nativeWindow = nativeWindowForSdlWindow(streamingWindow);
    if (nativeWindow) {
        m_StreamingNativeWindow = [nativeWindow retain];
    }
    else {
        qWarning("Unable to identify the macOS SDL streaming window; pointer input protection is unavailable");
    }

    if (QCoreApplication* app = QCoreApplication::instance()) {
        app->installNativeEventFilter(this);
        m_Installed = true;
    }
}

MacQtEventPumpInputGuard::~MacQtEventPumpInputGuard()
{
    finishEventProcessing();

    if (m_Installed) {
        if (QCoreApplication* app = QCoreApplication::instance()) {
            app->removeNativeEventFilter(this);
        }
        m_Installed = false;
    }

    if (m_StreamingNativeWindow) {
        [static_cast<NSWindow*>(m_StreamingNativeWindow) release];
        m_StreamingNativeWindow = nullptr;
    }
}

void MacQtEventPumpInputGuard::beginEventProcessing()
{
    m_InterceptingInputEvents = true;
}

void MacQtEventPumpInputGuard::finishEventProcessing()
{
    @autoreleasepool {
        m_InterceptingInputEvents = false;

        // postEvent:atStart: inserts one event at the front. Reposting in
        // reverse order preserves the original input sequence while also
        // keeping it ahead of events that arrived after the Qt pass.
        for (auto it = m_DeferredInputEvents.rbegin();
                it != m_DeferredInputEvents.rend(); ++it) {
            NSEvent* event = static_cast<NSEvent*>(*it);
            [NSApp postEvent:event atStart:YES];
            [event release];
        }
        m_DeferredInputEvents.clear();
    }
}

bool MacQtEventPumpInputGuard::nativeEventFilter(
        const QByteArray& eventType,
        void* message,
        MacQtEventPumpInputGuard::NativeEventResult*)
{
    if (!m_InterceptingInputEvents || message == nullptr ||
            eventType != QByteArrayLiteral("NSEvent")) {
        return false;
    }

    // QCocoaEventDispatcher uses "NSEvent" before NSApplication dispatch.
    // Do not intercept "mac_generic_NSEvent": SDL has already converted an
    // event before passing it to NSApplication, so requeueing it would produce
    // duplicate SDL input on the next pump.
    NSEvent* event = static_cast<NSEvent*>(message);
    const bool belongsToStreamingWindow = m_StreamingNativeWindow &&
            event.window == static_cast<NSWindow*>(m_StreamingNativeWindow);
    if (isKeyboardEvent(event.type) ||
            (belongsToStreamingWindow && isMouseEventHandledBySdl(event.type))) {
        [event retain];
        m_DeferredInputEvents.push_back(event);
        return true;
    }
    return false;
}
