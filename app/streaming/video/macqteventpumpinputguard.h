#pragma once

#include <QAbstractNativeEventFilter>

#include <vector>

// Streaming overlays do not accept focus, so SDL remains the keyboard owner
// while Qt is pumped only for transient overlay UI. On macOS both frameworks
// read the same AppKit queue, so this guard preserves keyboard events that
// arrive during a Qt pass and returns them for SDL to process afterwards.
class MacQtEventPumpInputGuard : public QAbstractNativeEventFilter
{
public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    using NativeEventResult = qintptr;
#else
    using NativeEventResult = long;
#endif

    MacQtEventPumpInputGuard();
    ~MacQtEventPumpInputGuard() override;

    MacQtEventPumpInputGuard(const MacQtEventPumpInputGuard&) = delete;
    MacQtEventPumpInputGuard& operator=(const MacQtEventPumpInputGuard&) = delete;

    void beginEventProcessing();
    void finishEventProcessing();

    bool nativeEventFilter(const QByteArray& eventType,
                           void* message,
                           NativeEventResult* result) override;

private:
    std::vector<void*> m_DeferredKeyboardEvents;
    bool m_InterceptingKeyboardEvents = false;
    bool m_Installed = false;
};
