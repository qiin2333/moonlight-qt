#include "input.h"
#include "hostcursormath.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include "streaming/streamutils.h"

#include <QImage>

#include <atomic>
#include <climits>

// Automatic mouse mode over cursor protocol v2 (Foundation Sunshine's
// docs/client-cursor-v2-design.md). The host reports its cursor's shape,
// visibility, clip and host-originated moves. While the host cursor is visible and
// free we use absolute input and draw it as the local OS cursor (the host leaves it
// out of the video); while a game hides or locks it we use relative input. See
// CursorModePolicy for the switching rules. Hosts without v2 keep the v1 path in
// input.cpp unchanged.

using CursorModePolicy::Mode;
using CursorModePolicy::Source;

// Shapes kept per session. Each unique shape is sent once per subscription.
#define HOST_CURSOR_MAX_SHAPES 64

// How often we may ask the host again for a shape a state names but we lack.
#define HOST_CURSOR_RESEND_INTERVAL_MS 1000

// After asking the host to stop drawing the cursor we show ours at once rather than
// wait a round trip for its confirmation. If the host still composites it this long
// afterwards, another client needs it in the video and ours is hidden again.
#define HOST_CURSOR_COMPOSITE_GRACE_MS 300

// Packed mouse mode for the stats overlay, which runs on the decoder thread.
enum MouseModeStatusBits {
    MMS_ABSOLUTE = 0x01,
    MMS_LOCAL_CURSOR = 0x02,
    MMS_SOURCE_SHIFT = 2,       // CursorModePolicy::Source
    MMS_REASON_SHIFT = 4,       // MouseModeFallbackReason when the source is Fallback
};

enum MouseModeFallbackReason {
    MMFR_HOST_UNSUPPORTED,
    MMFR_WAITING_FOR_HOST,
    MMFR_HOST_UNAVAILABLE,
};

static std::atomic<int> s_MouseModeStatus { -1 };

static HostCursorMath::Rect toMathRect(const SDL_Rect& rect)
{
    return { rect.x, rect.y, rect.w, rect.h };
}

static const char* modeName(Mode mode)
{
    switch (mode) {
    case Mode::Absolute:
        return "absolute";
    case Mode::Relative:
        return "relative";
    default:
        return "fallback";
    }
}

QString SdlInputHandler::getMouseModeStatus()
{
    int status = s_MouseModeStatus.load(std::memory_order_relaxed);
    if (status < 0) {
        return QString();
    }

    QString text = QString("Mouse mode: %1").arg((status & MMS_ABSOLUTE) ? "absolute" : "relative");
    if (status & MMS_LOCAL_CURSOR) {
        text += ", local cursor";
    }

    switch ((Source)((status >> MMS_SOURCE_SHIFT) & 0x3)) {
    case Source::Automatic:
        return text + " (automatic)";
    case Source::Locked:
        return text + " (locked)";
    default:
        break;
    }

    switch ((status >> MMS_REASON_SHIFT) & 0x3) {
    case MMFR_HOST_UNSUPPORTED:
        return text + " (fallback: host does not support automatic mode)";
    case MMFR_WAITING_FOR_HOST:
        return text + " (fallback: waiting for the host cursor)";
    default:
        return text + " (fallback: host cursor unavailable)";
    }
}

bool SdlInputHandler::isHostCursorV2Available() const
{
    return m_AutomaticMouseMode && (LiGetHostFeatureFlags() & LI_FF_CURSOR_STATE) != 0;
}

void SdlInputHandler::startHostCursor()
{
    // Also runs after a reconnect, which is a new host session: its subscription,
    // shapes and state start over, and nothing is trusted until its resync.
    m_HostCursorReady = false;
    m_HostCursorHasPosition = false;
    m_HostCursorControlSent = -1;
    m_HostCursorResendId = 0;
    m_HostCursorShapes.clear();
    m_HostCursorShapeOrder.clear();
    freeHostSdlCursors();
    SDL_zero(m_HostCursorState);
    m_CursorPolicy.setHostState(false, false, false, 0, 0, 0, 0);

    // With automatic mode off, or a host without cursor protocol v2, nothing here is
    // ever enabled and the mouse behaves exactly as before, v1 local cursor included.
    m_HostCursorSupported = isHostCursorV2Available();
    m_SpecialKeyCombos[KeyComboToggleMouseModeLock].enabled = m_HostCursorSupported;
    if (m_HostCursorSupported) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Host supports automatic mouse mode; subscribing to the host cursor");

        // The v1 local cursor is not used in a v2 session.
        m_LocalCursorMode.store(LI_CURSOR_MODE_VIDEO, std::memory_order_relaxed);
        resetRemoteCursor();

        updateHostCursorControl();
    }
    else if (m_AutomaticMouseMode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Host does not support automatic mouse mode; using the regular mouse mode");
    }

    evaluateCursorMode();
}

SDL_Rect SdlInputHandler::getVideoRegion()
{
    SDL_Rect src, dst;

    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;

    dst.x = dst.y = 0;
    getWindowCoordinateSize(&dst.w, &dst.h);

    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
    return dst;
}

bool SdlInputHandler::isAutomaticMouseModeActive()
{
    return m_CursorDecision.mode != Mode::Fallback;
}

bool SdlInputHandler::isHostCursorDrawingWanted()
{
    // We draw the cursor whenever automatic mode has us positioning it with absolute
    // input, unless Ctrl+Alt+Shift+C handed it back to the video for this session.
    return m_HostCursorSupported && m_CursorDecision.mode == Mode::Absolute && m_AbsoluteMouseMode &&
           isCaptureActive() && !m_HostCursorPreferVideo;
}

bool SdlInputHandler::isHostCursorDrawnLocally()
{
    if (!isHostCursorDrawingWanted() || !(m_HostCursorState.flags & LI_CURSOR_STATE_FLAG_VISIBLE)) {
        return false;
    }
    if (!(m_HostCursorState.flags & LI_CURSOR_STATE_FLAG_COMPOSITED)) {
        return true;
    }

    // The host still composites the cursor: it has not processed our request yet
    // (show ours anyway for the grace period), or another client needs it in the video.
    return m_ClientDrawsGraceActive &&
           !SDL_TICKS_PASSED(SDL_GetTicks(), m_ClientDrawsSinceMs + HOST_CURSOR_COMPOSITE_GRACE_MS);
}

void SdlInputHandler::updateHostCursorControl()
{
    if (!m_HostCursorSupported) {
        return;
    }

    int flags = LI_CURSOR_CONTROL_SUBSCRIBE;
    if (isHostCursorDrawingWanted()) {
        flags |= LI_CURSOR_CONTROL_CLIENT_DRAWS;
    }

    if (flags != m_HostCursorControlSent && LiSendCursorControl((uint8_t)flags, 0) == LI_CURSOR_MODE_OK) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Host cursor: %s draws the cursor",
                    (flags & LI_CURSOR_CONTROL_CLIENT_DRAWS) ? "client" : "host");
        m_HostCursorControlSent = flags;

        m_ClientDrawsGraceActive = (flags & LI_CURSOR_CONTROL_CLIENT_DRAWS) != 0;
        if (m_ClientDrawsGraceActive) {
            m_ClientDrawsSinceMs = SDL_GetTicks();
            scheduleCursorModeTimer();
        }
    }
}

void SdlInputHandler::updateLocalCursor(int mouseInVideoRegion)
{
    if (!m_HostCursorSupported || !m_AbsoluteMouseMode || !isCaptureActive() || SDL_GetRelativeMouseMode()) {
        return;
    }

    if (mouseInVideoRegion < 0) {
        int mouseX, mouseY;
        SDL_GetMouseState(&mouseX, &mouseY);
        mouseInVideoRegion = SDL_GetMouseFocus() == m_Window && isMouseInVideoRegion(mouseX, mouseY);
    }

    if (isHostCursorDrawnLocally()) {
        SDL_Cursor* cursor = mouseInVideoRegion ? getHostSdlCursor() : nullptr;
        SDL_SetCursor(cursor != nullptr ? cursor : SDL_GetDefaultCursor());
        SDL_ShowCursor(SDL_ENABLE);
    }
    else {
        // In automatic mode the host's cursor (ours or the video's) is the only one
        // over the video. In fallback, "Show local cursor" decides, as on other hosts.
        const bool showOverVideo = !isAutomaticMouseModeActive() && m_MouseCursorCapturedVisibilityState == SDL_ENABLE;
        SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_ShowCursor((mouseInVideoRegion && !showOverVideo) ? SDL_DISABLE : SDL_ENABLE);
    }
}

void SdlInputHandler::warpToHostCursor(bool modeSwitch)
{
    // Only move the user's cursor when it is ours to move: the window is focused and,
    // outside a switch back from relative mode, the cursor is over the video.
    if (!m_HostCursorHasPosition || !(SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS) ||
            SDL_GetMouseFocus() != m_Window) {
        return;
    }

    const LI_CURSOR_STATE& state = m_HostCursorState;
    if (m_HostCursorX < 0 || m_HostCursorY < 0 ||
            m_HostCursorX >= state.referenceWidth || m_HostCursorY >= state.referenceHeight) {
        // On another host monitor; absolute input cannot follow it there.
        return;
    }

    int mouseX, mouseY;
    SDL_GetMouseState(&mouseX, &mouseY);
    if (!modeSwitch && !isMouseInVideoRegion(mouseX, mouseY)) {
        return;
    }

    HostCursorMath::Point target = HostCursorMath::streamToWindow(toMathRect(getVideoRegion()),
                                                                  state.referenceWidth, state.referenceHeight,
                                                                  m_HostCursorX, m_HostCursorY);
    if (modeSwitch) {
        // setCaptureActive() syncs the host to this point rather than reading the
        // cursor back, which macOS may not have updated yet.
        m_AbsoluteSyncPoint = { target.x, target.y };
        m_HasAbsoluteSyncPoint = true;
    }
    if (qAbs(target.x - mouseX) <= 1 && qAbs(target.y - mouseY) <= 1) {
        return;
    }

    SDL_WarpMouseInWindow(m_Window, target.x, target.y);
}

bool SdlInputHandler::getHostCursorClipRect(SDL_Rect* rect)
{
    const LI_CURSOR_STATE& state = m_HostCursorState;
    if (!isHostCursorDrawingWanted() || !(state.flags & LI_CURSOR_STATE_FLAG_HAS_CLIP) ||
            !m_CursorPolicy.hostClipConfinesCursor(SDL_GetTicks())) {
        return false;
    }

    HostCursorMath::Rect clip = HostCursorMath::streamRectToWindow(toMathRect(getVideoRegion()),
                                                                   state.referenceWidth, state.referenceHeight,
                                                                   state.clipLeft, state.clipTop,
                                                                   state.clipRight, state.clipBottom);
    if (clip.w <= 0 || clip.h <= 0) {
        return false;
    }

    rect->x = clip.x;
    rect->y = clip.y;
    rect->w = clip.w;
    rect->h = clip.h;
    return true;
}

SDL_Cursor* SdlInputHandler::getHostSdlCursor()
{
    const uint32_t shapeId = m_HostCursorState.shapeId;
    auto shape = m_HostCursorShapes.constFind(shapeId);
    if (shapeId == 0 || shape == m_HostCursorShapes.constEnd()) {
        return nullptr;
    }

    // Match the cursor's size in the stream: host shape pixels -> stream pixels ->
    // window units. A new scale (resize, fullscreen, another display) rebuilds them.
    double scale = HostCursorMath::cursorScale(m_HostCursorState.scaleQ16, toMathRect(getVideoRegion()),
                                               m_HostCursorState.referenceWidth);
    if (scale != m_HostSdlCursorScale) {
        freeHostSdlCursors();
        m_HostSdlCursorScale = scale;
    }

    auto existing = m_HostSdlCursors.constFind(shapeId);
    if (existing != m_HostSdlCursors.constEnd()) {
        return existing.value();
    }

    SDL_Cursor* cursor = createHostSdlCursor(shape.value(), scale);
    if (cursor != nullptr) {
        m_HostSdlCursors.insert(shapeId, cursor);
    }
    return cursor;
}

SDL_Cursor* SdlInputHandler::createHostSdlCursor(const HostCursorShape& shape, double scale)
{
    const QByteArray bgra = QByteArray::fromRawData(reinterpret_cast<const char*>(shape.pixels.data()),
                                                    (int)shape.pixels.size());

    // As on the v1 path, a recognised standard shape becomes the client OS cursor
    // (MOONLIGHT_NATIVE_CURSOR=0 draws the host's bitmap instead).
    if (nativeCursorSubstitutionEnabled()) {
        SDL_SystemCursor systemCursor;
        const NativeCursorShape nativeShape =
            classifyCursorShape(measureCursorShape(shape.width, shape.height, shape.hotspotX, shape.hotspotY, bgra));
        if (toSdlSystemCursor(nativeShape, systemCursor)) {
            SDL_Cursor* cursor = SDL_CreateSystemCursor(systemCursor);
            if (cursor != nullptr) {
                return cursor;
            }
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to create system cursor %d: %s",
                        (int)systemCursor, SDL_GetError());
        }
    }

    const int width = HostCursorMath::scaledDimension(shape.width, scale);
    const int height = HostCursorMath::scaledDimension(shape.height, scale);
    const int hotspotX = qBound(0, qRound((double)shape.hotspotX * width / shape.width), width - 1);
    const int hotspotY = qBound(0, qRound((double)shape.hotspotY * height / shape.height), height - 1);

    // Scale in premultiplied space, as the v1 path does, so transparent neighbours do
    // not darken the edges. BGRA bytes are QImage::Format_ARGB32 on little-endian.
    QImage image(reinterpret_cast<const uchar*>(bgra.constData()), shape.width, shape.height,
                 shape.width * 4, QImage::Format_ARGB32);
    if (width != shape.width || height != shape.height) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                    .scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_ARGB32);
    }
    else {
        image = image.copy();
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uchar*>(image.constBits()),
                                                              image.width(), image.height(), 32,
                                                              image.bytesPerLine(), SDL_PIXELFORMAT_BGRA32);
    if (surface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateRGBSurfaceWithFormatFrom() failed: %s",
                    SDL_GetError());
        return nullptr;
    }

    SDL_Cursor* cursor = SDL_CreateColorCursor(surface, hotspotX, hotspotY);
    SDL_FreeSurface(surface);
    if (cursor == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateColorCursor() failed: %s",
                    SDL_GetError());
    }
    return cursor;
}

void SdlInputHandler::freeHostSdlCursor(uint32_t shapeId)
{
    auto it = m_HostSdlCursors.find(shapeId);
    if (it == m_HostSdlCursors.end()) {
        return;
    }

    if (SDL_GetCursor() == it.value()) {
        SDL_SetCursor(SDL_GetDefaultCursor());
    }
    SDL_FreeCursor(it.value());
    m_HostSdlCursors.erase(it);
}

void SdlInputHandler::freeHostSdlCursors()
{
    while (!m_HostSdlCursors.isEmpty()) {
        freeHostSdlCursor(m_HostSdlCursors.constBegin().key());
    }
}

Uint32 SdlInputHandler::cursorModeTimerCallback(Uint32, void*)
{
    // Runs on SDL's timer thread: only hand the work to the main thread.
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_HOST_CURSOR_TICK;
    if (SDL_PushEvent(&event) < 0) {
        // The queue is full. Retry rather than stop: m_CursorModeTimer would keep this
        // expired ID, and scheduleCursorModeTimer() wouldn't re-arm for the same due time.
        return 10;
    }
    return 0;
}

void SdlInputHandler::noteMouseButtons()
{
    if (!m_HostCursorSupported) {
        return;
    }

    // A return to absolute input waits until every button is up, so an MMO camera
    // drag ends as a drag rather than a jump. Runs after the button event was sent,
    // so the release reaches the host before any switch.
    const bool held = SDL_GetMouseState(nullptr, nullptr) != 0;
    m_CursorPolicy.setMouseButtonsHeld(held);
    if (!held) {
        evaluateCursorMode();
    }
}

void SdlInputHandler::handleHostCursorTick()
{
    m_CursorModeTimer = 0;
    if (m_ClientDrawsGraceActive && SDL_TICKS_PASSED(SDL_GetTicks(), m_ClientDrawsSinceMs + HOST_CURSOR_COMPOSITE_GRACE_MS)) {
        m_ClientDrawsGraceActive = false;
    }
    evaluateCursorMode();

    // Recentring may have just expired, which can confine the cursor again, or the
    // composite grace period may have just ended
    updatePointerRegionLock();
}

bool SdlInputHandler::isAwaitingRelativeMouse()
{
    return m_HostCursorSupported && m_CursorPolicy.awaitingRelative(SDL_GetTicks());
}

void SdlInputHandler::scheduleCursorModeTimer()
{
    // One timer for the earliest of: recentring expiring, and the end of the grace
    // period while the host still reports compositing a cursor we already show.
    // It never delays a mode switch.
    const Uint32 now = SDL_GetTicks();
    uint32_t due = m_CursorDecision.nextUpdateMs;
    if (m_ClientDrawsGraceActive && (m_HostCursorState.flags & LI_CURSOR_STATE_FLAG_COMPOSITED)) {
        const uint32_t graceEnd = m_ClientDrawsSinceMs + HOST_CURSOR_COMPOSITE_GRACE_MS;
        if (!SDL_TICKS_PASSED(now, graceEnd) && (due == 0 || SDL_TICKS_PASSED(due, graceEnd))) {
            due = graceEnd;
        }
    }

    if (due != 0) {
        if (m_CursorModeTimer == 0 || m_CursorModeTimerDue != due) {
            if (m_CursorModeTimer != 0) {
                SDL_RemoveTimer(m_CursorModeTimer);
            }
            Uint32 delay = (Uint32)qMax<int32_t>(1, (int32_t)(due - now));
            m_CursorModeTimer = SDL_AddTimer(delay, cursorModeTimerCallback, nullptr);
            m_CursorModeTimerDue = due;
        }
    }
    else if (m_CursorModeTimer != 0) {
        SDL_RemoveTimer(m_CursorModeTimer);
        m_CursorModeTimer = 0;
    }
}

void SdlInputHandler::evaluateCursorMode()
{
    const Uint32 now = SDL_GetTicks();
    const Mode previous = m_CursorDecision.mode;
    m_CursorDecision = m_CursorPolicy.update(now);

    if (m_CursorDecision.mode != previous) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Mouse mode: %s -> %s (%s)",
                    modeName(previous), modeName(m_CursorDecision.mode),
                    m_CursorDecision.source == Source::Locked ? "locked" : "automatic");
        applyCursorMode(m_CursorDecision.mode, previous);
    }

    scheduleCursorModeTimer();

    publishMouseModeStatus();
}

void SdlInputHandler::clearMouseModeStatus()
{
    s_MouseModeStatus.store(-1, std::memory_order_relaxed);
}

void SdlInputHandler::publishMouseModeStatus()
{
    // With the setting off the overlay is unchanged.
    int status = -1;
    if (m_AutomaticMouseMode) {
        status = (m_AbsoluteMouseMode ? MMS_ABSOLUTE : 0) |
                 (isHostCursorDrawnLocally() ? MMS_LOCAL_CURSOR : 0) |
                 ((int)m_CursorDecision.source << MMS_SOURCE_SHIFT);
        if (m_CursorDecision.source == Source::Fallback) {
            int reason = !m_HostCursorSupported ? MMFR_HOST_UNSUPPORTED :
                         !m_HostCursorReady ? MMFR_WAITING_FOR_HOST : MMFR_HOST_UNAVAILABLE;
            status |= reason << MMS_REASON_SHIFT;
        }
    }
    s_MouseModeStatus.store(status, std::memory_order_relaxed);
}

void SdlInputHandler::applyCursorMode(Mode mode, Mode previous)
{
    const bool absolute = mode == Mode::Absolute ? true :
                          mode == Mode::Relative ? false : m_FallbackAbsoluteMouseMode;
    const bool focused = (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS) != 0;

    if (absolute != m_AbsoluteMouseMode) {
        const bool captured = isCaptureActive();
        if (captured) {
            setCaptureActive(false);
        }

        m_AbsoluteMouseMode = absolute;

        // Back to absolute: put the local cursor where the host cursor is before
        // capture resumes, so absolute input picks up from there.
        if (mode == Mode::Absolute && captured && focused) {
            warpToHostCursor(true);
        }

        // Leaving relative mode and warping queue mouse motion carrying positions
        // from before the switch. The first absolute update would send the last of
        // them, jumping the host cursor (and, mid-drag in a game, its camera).
        if (absolute) {
            SDL_PumpEvents();
            SDL_FlushEvent(SDL_MOUSEMOTION);
        }

        // Unfocused, relative input waits for a click (as game mode always has);
        // absolute input needs no grab.
        if (captured && (focused || absolute)) {
            setCaptureActive(true);
        }
        m_HasAbsoluteSyncPoint = false;
    }

    // The host's first state puts a focused window straight into absolute mode,
    // without the click relative mode needs to capture.
    if (!m_AutomaticCaptureDone && previous == Mode::Fallback && mode == Mode::Absolute) {
        m_AutomaticCaptureDone = true;
        if (focused && !isCaptureActive()) {
            setCaptureActive(true);
        }
    }

    updateHostCursorControl();
    updatePointerRegionLock();
}

void SdlInputHandler::handleHostCursorShape(const LI_CURSOR_SHAPE* shape)
{
    if (!m_HostCursorSupported) {
        return;
    }

    HostCursorShape copy;
    copy.width = shape->width;
    copy.height = shape->height;
    copy.hotspotX = shape->hotspotX;
    copy.hotspotY = shape->hotspotY;
    copy.pixels.assign(shape->pixels, shape->pixels + (size_t)shape->width * shape->height * 4);

    if (!m_HostCursorShapes.contains(shape->shapeId)) {
        m_HostCursorShapeOrder.append(shape->shapeId);
        while (m_HostCursorShapeOrder.size() > HOST_CURSOR_MAX_SHAPES) {
            uint32_t oldest = m_HostCursorShapeOrder.takeFirst();
            m_HostCursorShapes.remove(oldest);
            freeHostSdlCursor(oldest);
        }
    }
    m_HostCursorShapes.insert(shape->shapeId, std::move(copy));
    freeHostSdlCursor(shape->shapeId);

    if (shape->shapeId == m_HostCursorState.shapeId) {
        updateLocalCursor();
    }
}

void SdlInputHandler::handleHostCursorState(const LI_CURSOR_STATE* state)
{
    if (!m_HostCursorSupported) {
        return;
    }

    if (state->flags & LI_CURSOR_STATE_FLAG_RESYNC) {
        if (!m_HostCursorReady) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Host cursor: received the initial state");
        }
        m_HostCursorReady = true;
    }
    else if (!m_HostCursorReady) {
        // Nothing is trusted before the host's full resync.
        return;
    }

    // Confinement depends on the clip and on whether the cursor is visible.
    const bool confinementMayChange =
        (state->flags & LI_CURSOR_STATE_FLAG_HAS_CLIP) != (m_HostCursorState.flags & LI_CURSOR_STATE_FLAG_HAS_CLIP) ||
        (state->flags & LI_CURSOR_STATE_FLAG_VISIBLE) != (m_HostCursorState.flags & LI_CURSOR_STATE_FLAG_VISIBLE) ||
        state->clipLeft != m_HostCursorState.clipLeft || state->clipTop != m_HostCursorState.clipTop ||
        state->clipRight != m_HostCursorState.clipRight || state->clipBottom != m_HostCursorState.clipBottom;
    m_HostCursorState = *state;
    if (state->flags & LI_CURSOR_STATE_FLAG_HAS_POSITION) {
        m_HostCursorHasPosition = true;
        m_HostCursorX = state->x;
        m_HostCursorY = state->y;
    }

    const Uint32 now = SDL_GetTicks();
    const bool visible = state->flags & LI_CURSOR_STATE_FLAG_VISIBLE;
    // The parser guarantees an ordered clip, not a small one: subtract in 64 bits.
    const int clipWidth = (int)std::clamp<int64_t>((int64_t)state->clipRight - state->clipLeft, 0, INT_MAX);
    const int clipHeight = (int)std::clamp<int64_t>((int64_t)state->clipBottom - state->clipTop, 0, INT_MAX);
    m_CursorPolicy.setHostState(!(state->flags & LI_CURSOR_STATE_FLAG_FALLBACK), visible,
                                state->flags & LI_CURSOR_STATE_FLAG_HAS_CLIP, clipWidth, clipHeight,
                                state->referenceWidth, state->referenceHeight);
    if (visible && (state->flags & LI_CURSOR_STATE_FLAG_HAS_POSITION) && !(state->flags & LI_CURSOR_STATE_FLAG_RESYNC)) {
        m_CursorPolicy.noteHostPosition(now, state->x, state->y);
    }
    evaluateCursorMode();

    // A state may name a shape we never received (or evicted); ask for it again.
    if (state->shapeId != 0 && !m_HostCursorShapes.contains(state->shapeId)) {
        if (state->shapeId != m_HostCursorResendId ||
                SDL_TICKS_PASSED(now, m_HostCursorResendTime + HOST_CURSOR_RESEND_INTERVAL_MS)) {
            int flags = m_HostCursorControlSent >= 0 ? m_HostCursorControlSent : LI_CURSOR_CONTROL_SUBSCRIBE;
            LiSendCursorControl((uint8_t)flags, state->shapeId);
            m_HostCursorResendId = state->shapeId;
            m_HostCursorResendTime = now;
        }
    }

    // The host confirmed it stopped compositing; the grace period is over.
    if (!(state->flags & LI_CURSOR_STATE_FLAG_COMPOSITED)) {
        m_ClientDrawsGraceActive = false;
    }

    updateHostCursorControl();

    // Not while the host wants relative input (a visible cursor locked or recentred):
    // the warp's motion event would be the first motion, switching to relative and
    // sending the warp distance to the game as a mouse move.
    if ((state->flags & LI_CURSOR_STATE_FLAG_HAS_POSITION) && isHostCursorDrawnLocally() &&
            !m_CursorPolicy.hostWantsRelative(now)) {
        warpToHostCursor();
    }

    if (confinementMayChange) {
        updatePointerRegionLock();
    }
    else {
        updateLocalCursor();
    }
}
