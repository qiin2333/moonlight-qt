#pragma once

#include <QAbstractNativeEventFilter>

#include <vector>

struct SDL_Window;

// Qt and SDL read the same process-wide AppKit event queue. During a Qt overlay
// pass, this guard preserves keyboard input and pointer input targeting the SDL
// streaming window, then returns those events for SDL to process afterwards.
// Pointer input for Qt overlay windows remains owned by Qt.
class MacQtEventPumpInputGuard : public QAbstractNativeEventFilter
{
public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    using NativeEventResult = qintptr;
#else
    using NativeEventResult = long;
#endif

    explicit MacQtEventPumpInputGuard(SDL_Window* streamingWindow);
    ~MacQtEventPumpInputGuard() override;

    MacQtEventPumpInputGuard(const MacQtEventPumpInputGuard&) = delete;
    MacQtEventPumpInputGuard& operator=(const MacQtEventPumpInputGuard&) = delete;

    void beginEventProcessing();
    void finishEventProcessing();

    bool nativeEventFilter(const QByteArray& eventType,
                           void* message,
                           NativeEventResult* result) override;

private:
    std::vector<void*> m_DeferredInputEvents;
    void* m_StreamingNativeWindow = nullptr;
    bool m_InterceptingInputEvents = false;
    bool m_Installed = false;
};
