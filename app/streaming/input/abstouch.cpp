#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include <SDL_syswm.h>
#include "streaming/streamutils.h"
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
#include "streaming/waylandwindowmetrics.h"
#endif

#include <QtMath>

bool SdlInputHandler::isPenTouchDevice(SDL_TouchID touchId)
{
    if (touchId == SDL_PEN_TOUCHID) {
        return true;
    }

#if SDL_VERSION_ATLEAST(2, 0, 22)
    const int numTouchDevices = SDL_GetNumTouchDevices();
    for (int i = 0; i < numTouchDevices; i++) {
        if (touchId == SDL_GetTouchDevice(i)) {
            const char* touchName = SDL_GetTouchName(i);
            return touchName &&
                    (SDL_strcmp(touchName, "pen") == 0 ||
                     SDL_strcmp(touchName, "pen_input") == 0);
        }
    }
#endif

    return false;
}

// How long the fingers must be stationary to start a right click
#define LONG_PRESS_ACTIVATION_DELAY 650

// How far the finger can move before it cancels a right click
#define LONG_PRESS_ACTIVATION_DELTA 0.01f

// How long the double tap deadzone stays in effect between touch up and touch down
#define DOUBLE_TAP_DEAD_ZONE_DELAY 250

// How far the finger can move before it can override the double tap deadzone
#define DOUBLE_TAP_DEAD_ZONE_DELTA 0.025f

#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
WaylandWindowMetrics::Point
SdlInputHandler::getTouchWindowPoint(const SDL_TouchFingerEvent* event,
                                     WaylandWindowMetrics::Size windowSize) const
{
    WaylandWindowMetrics::CoordinateMetrics metrics = { windowSize, windowSize };
    if (m_WaylandCoordinateMetrics.has_value()) {
        metrics = *m_WaylandCoordinateMetrics;
    }

    return WaylandWindowMetrics::windowPointForNormalizedTouch(event->x, event->y, metrics);
}

float SdlInputHandler::getTouchDistance(const SDL_TouchFingerEvent* first,
                                        const SDL_TouchFingerEvent* second) const
{
    if (!m_WaylandCoordinateMetrics.has_value()) {
        // Preserve the established calculation exactly on every SDL-owned
        // window path. Only the Qt-owned Wayland wrapper needs correction for
        // SDL's physical touch normalization extent.
        return qSqrt(qPow(first->x - second->x, 2) + qPow(first->y - second->y, 2));
    }

    return WaylandWindowMetrics::logicalNormalizedTouchDistance(
        first->x, first->y, second->x, second->y, *m_WaylandCoordinateMetrics);
}
#endif

Uint32 SdlInputHandler::longPressTimerCallback(Uint32, void*)
{
    // Raise the left click and start a right click
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);

    return 0;
}

void SdlInputHandler::disableTouchFeedback()
{
    SDL_SysWMinfo info;

    SDL_VERSION(&info.version);
    SDL_GetWindowWMInfo(m_Window, &info);

#ifdef Q_OS_WIN32
    if (info.subsystem == SDL_SYSWM_WINDOWS) {
        constexpr FEEDBACK_TYPE feedbackTypes[] = {
            FEEDBACK_TOUCH_CONTACTVISUALIZATION,
            FEEDBACK_PEN_BARRELVISUALIZATION,
            FEEDBACK_PEN_TAP,
            FEEDBACK_PEN_DOUBLETAP,
            FEEDBACK_PEN_PRESSANDHOLD,
            FEEDBACK_PEN_RIGHTTAP,
            FEEDBACK_TOUCH_TAP,
            FEEDBACK_TOUCH_DOUBLETAP,
            FEEDBACK_TOUCH_PRESSANDHOLD,
            FEEDBACK_TOUCH_RIGHTTAP,
            FEEDBACK_GESTURE_PRESSANDTAP,
        };

        for (FEEDBACK_TYPE ft : feedbackTypes) {
            BOOL val = FALSE;
            SetWindowFeedbackSetting(info.info.win.window, ft, 0, sizeof(val), &val);
        }
    }
#endif
}

void SdlInputHandler::handleAbsoluteFingerEvent(SDL_TouchFingerEvent* event)
{
    SDL_Rect src, dst;
    int windowWidth, windowHeight;

    getWindowCoordinateSize(&windowWidth, &windowHeight);
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    const WaylandWindowMetrics::Size windowSize = { windowWidth, windowHeight };
#endif

    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;

    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;

    // Scale window-relative events to be video-relative and clamp to video region
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    const WaylandWindowMetrics::Point touchPoint = getTouchWindowPoint(event, windowSize);
    float vidrelx = qMin(qMax(touchPoint.x, dst.x), dst.x + dst.w) - dst.x;
    float vidrely = qMin(qMax(touchPoint.y, dst.y), dst.y + dst.h) - dst.y;
#else
    float vidrelx = qMin(qMax((int)(event->x * windowWidth), dst.x), dst.x + dst.w) - dst.x;
    float vidrely = qMin(qMax((int)(event->y * windowHeight), dst.y), dst.y + dst.h) - dst.y;
#endif

    uint8_t eventType;
    switch (event->type) {
    case SDL_FINGERDOWN:
        eventType = LI_TOUCH_EVENT_DOWN;
        break;
    case SDL_FINGERMOTION:
        eventType = LI_TOUCH_EVENT_MOVE;
        break;
    case SDL_FINGERUP:
        eventType = LI_TOUCH_EVENT_UP;
        break;
    default:
        return;
    }

    uint32_t pointerId;

    // If the pointer ID is larger than we can fit, just CRC it and use that as the ID.
    if ((uint64_t)event->fingerId > UINT32_MAX) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QByteArrayView bav((char*)&event->fingerId, sizeof(event->fingerId));
        pointerId = qChecksum(bav);
#else
        pointerId = qChecksum((char*)&event->fingerId, sizeof(event->fingerId));
#endif
    }
    else {
        pointerId = (uint32_t)event->fingerId;
    }

    // Try to send it as a native pen/touch event, otherwise fall back to our touch emulation
    if (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) {
        if (isPenTouchDevice(event->touchId)) {
            LiSendPenEvent(eventType, LI_TOOL_TYPE_PEN, 0, vidrelx / dst.w, vidrely / dst.h, event->pressure,
                           0.0f, 0.0f, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
        }
        else
        {
            LiSendTouchEvent(eventType, pointerId, vidrelx / dst.w, vidrely / dst.h, event->pressure,
                             0.0f, 0.0f, LI_ROT_UNKNOWN);
        }

        if (!m_DisabledTouchFeedback) {
            // Disable touch feedback when passing touch natively
            disableTouchFeedback();
            m_DisabledTouchFeedback = true;
        }
    }
    else {
        emulateAbsoluteFingerEvent(event);
    }
}

void SdlInputHandler::emulateAbsoluteFingerEvent(SDL_TouchFingerEvent* event)
{
    // Observations on Windows 10: x and y appear to be relative to 0,0 of the window client area.
    // Although SDL documentation states they are 0.0 - 1.0 float values, they can actually be higher
    // or lower than those values as touch events continue for touches started within the client area that
    // leave the client area during a drag motion.
    // dx and dy are deltas from the last touch event, not the first touch down.

    // Ignore touch down events with more than one finger
    if (event->type == SDL_FINGERDOWN && SDL_GetNumTouchFingers(event->touchId) > 1) {
        return;
    }

    // Ignore touch move and touch up events from the non-primary finger
    if (event->type != SDL_FINGERDOWN && event->fingerId != m_LastTouchDownEvent.fingerId) {
        return;
    }

    SDL_Rect src, dst;
    int windowWidth, windowHeight;

    getWindowCoordinateSize(&windowWidth, &windowHeight);
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    const WaylandWindowMetrics::Size windowSize = { windowWidth, windowHeight };
#endif

    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;

    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;

    // Use the stream and window sizes to determine the video region
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    const float touchDownDistance = getTouchDistance(event, &m_LastTouchDownEvent);
#else
    const float touchDownDistance = qSqrt(qPow(event->x - m_LastTouchDownEvent.x, 2) +
                                          qPow(event->y - m_LastTouchDownEvent.y, 2));
#endif
    if (touchDownDistance > LONG_PRESS_ACTIVATION_DELTA) {
        // Moved too far since touch down. Cancel the long press timer.
        SDL_RemoveTimer(m_LongPressTimer);
        m_LongPressTimer = 0;
    }

    // Don't reposition for finger down events within the deadzone. This makes double-clicking easier.
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    const float touchUpDistance = getTouchDistance(event, &m_LastTouchUpEvent);
#else
    const float touchUpDistance =
        qSqrt(qPow(event->x - m_LastTouchUpEvent.x, 2) + qPow(event->y - m_LastTouchUpEvent.y, 2));
#endif
    if (event->type != SDL_FINGERDOWN ||
        event->timestamp - m_LastTouchUpEvent.timestamp > DOUBLE_TAP_DEAD_ZONE_DELAY ||
        touchUpDistance > DOUBLE_TAP_DEAD_ZONE_DELTA) {
        // Scale window-relative events to be video-relative and clamp to video region
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
        const WaylandWindowMetrics::Point touchPoint = getTouchWindowPoint(event, windowSize);
        short x = qMin(qMax(touchPoint.x, dst.x), dst.x + dst.w);
        short y = qMin(qMax(touchPoint.y, dst.y), dst.y + dst.h);
#else
        short x = qMin(qMax((int)(event->x * windowWidth), dst.x), dst.x + dst.w);
        short y = qMin(qMax((int)(event->y * windowHeight), dst.y), dst.y + dst.h);
#endif

        // Update the cursor position relative to the video region
        LiSendMousePositionEvent(x - dst.x, y - dst.y, dst.w, dst.h);
    }

    if (event->type == SDL_FINGERDOWN) {
        m_LastTouchDownEvent = *event;

        // Start/restart the long press timer
        SDL_RemoveTimer(m_LongPressTimer);
        m_LongPressTimer = SDL_AddTimer(LONG_PRESS_ACTIVATION_DELAY,
                                        longPressTimerCallback,
                                        nullptr);

        // Left button down on finger down
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    }
    else if (event->type == SDL_FINGERUP) {
        m_LastTouchUpEvent = *event;

        // Cancel the long press timer
        SDL_RemoveTimer(m_LongPressTimer);
        m_LongPressTimer = 0;

        // Left button up on finger up
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);

        // Raise right button too in case we triggered a long press gesture
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    }
}
