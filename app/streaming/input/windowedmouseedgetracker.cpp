#include "windowedmouseedgetracker.h"

#include <algorithm>
#include <cstdint>

namespace {
/**
 * Returns whether a client area is large enough to have distinct edges.
 */
bool hasUsableBounds(int width, int height)
{
    return width > 1 && height > 1;
}

/**
 * Clamps a possibly accumulated 64-bit coordinate to the client area.
 */
int clampCoordinate(std::int64_t value, int extent)
{
    return static_cast<int>(
            std::clamp<std::int64_t>(value, 0, extent - 1));
}
} // namespace

void WindowedMouseEdgeTracker::reset(int x,
                                     int y,
                                     int windowWidth,
                                     int windowHeight)
{
    if (!hasUsableBounds(windowWidth, windowHeight)) {
        deactivate();
        return;
    }

    // The supplied position can briefly be outside the client area while a
    // window is moving or resizing, so always normalize it before tracking.
    m_X = clampCoordinate(x, windowWidth);
    m_Y = clampCoordinate(y, windowHeight);
    m_Active = true;
}

void WindowedMouseEdgeTracker::deactivate()
{
    m_Active = false;
}

WindowedMouseEdgeTracker::ReleaseDecision
WindowedMouseEdgeTracker::update(int deltaX,
                                 int deltaY,
                                 int windowWidth,
                                 int windowHeight,
                                 bool releaseAllowed)
{
    ReleaseDecision decision;
    if (!hasUsableBounds(windowWidth, windowHeight)) {
        deactivate();
        return decision;
    }

    // A mode switch can begin relative input without a usable absolute cursor
    // position. Starting at the center prevents an immediate false release.
    if (!m_Active) {
        reset(windowWidth / 2,
              windowHeight / 2,
              windowWidth,
              windowHeight);
    }

    // Use 64-bit accumulation so malformed or unusually large input deltas
    // cannot overflow before the values are clamped to the client area.
    const std::int64_t nextX =
            static_cast<std::int64_t>(m_X) + deltaX;
    const std::int64_t nextY =
            static_cast<std::int64_t>(m_Y) + deltaY;

    const bool exitsLeft = deltaX < 0 && nextX <= 0;
    const bool exitsRight =
            deltaX > 0 && nextX >= windowWidth - 1;
    const bool exitsTop = deltaY < 0 && nextY <= 0;
    const bool exitsBottom =
            deltaY > 0 && nextY >= windowHeight - 1;

    m_X = clampCoordinate(nextX, windowWidth);
    m_Y = clampCoordinate(nextY, windowHeight);

    // Keep tracking at the edge while a button is down. The next outward
    // movement after the drag ends will then release the cursor cleanly.
    if (releaseAllowed &&
        (exitsLeft || exitsRight || exitsTop || exitsBottom)) {
        decision.shouldRelease = true;
        decision.x = m_X;
        decision.y = m_Y;
        m_Active = false;
    }

    return decision;
}
