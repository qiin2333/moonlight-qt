#include "macqteventpumpinputguard.h"

#import <AppKit/AppKit.h>

#include <QCoreApplication>

MacQtEventPumpInputGuard::MacQtEventPumpInputGuard()
{
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
}

void MacQtEventPumpInputGuard::beginEventProcessing()
{
    m_InterceptingKeyboardEvents = true;
}

void MacQtEventPumpInputGuard::finishEventProcessing()
{
    @autoreleasepool {
        m_InterceptingKeyboardEvents = false;

        // postEvent:atStart: inserts one event at the front. Reposting in
        // reverse order preserves the original keyboard sequence while also
        // keeping it ahead of events that arrived after the Qt pass.
        for (auto it = m_DeferredKeyboardEvents.rbegin();
                it != m_DeferredKeyboardEvents.rend(); ++it) {
            NSEvent* event = static_cast<NSEvent*>(*it);
            [NSApp postEvent:event atStart:YES];
            [event release];
        }
        m_DeferredKeyboardEvents.clear();
    }
}

bool MacQtEventPumpInputGuard::nativeEventFilter(
        const QByteArray& eventType,
        void* message,
        MacQtEventPumpInputGuard::NativeEventResult*)
{
    if (!m_InterceptingKeyboardEvents || message == nullptr ||
            eventType != QByteArrayLiteral("NSEvent")) {
        return false;
    }

    // QCocoaEventDispatcher uses "NSEvent" before NSApplication dispatch.
    // Do not intercept "mac_generic_NSEvent": SDL has already converted an
    // event before passing it to NSApplication, so requeueing it would produce
    // duplicate SDL keyboard input on the next pump.
    NSEvent* event = static_cast<NSEvent*>(message);
    switch (event.type) {
    case NSEventTypeKeyDown:
    case NSEventTypeKeyUp:
    case NSEventTypeFlagsChanged:
        [event retain];
        m_DeferredKeyboardEvents.push_back(event);
        return true;
    default:
        return false;
    }
}
