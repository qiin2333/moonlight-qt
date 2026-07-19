#pragma once

/**
 * Tracks a virtual cursor while SDL relative mouse mode is active.
 *
 * SDL reports only movement deltas in relative mode and keeps the real cursor
 * captured, so window-edge detection cannot use the event's absolute position
 * reliably. This helper integrates those deltas into a bounded virtual cursor
 * and reports when the user moves outward through an edge.
 */
class WindowedMouseEdgeTracker
{
public:
    struct ReleaseDecision {
        bool shouldRelease = false;
        int x = 0;
        int y = 0;
    };

    /**
     * Starts tracking from a known position in the current client area.
     */
    void reset(int x, int y, int windowWidth, int windowHeight);

    /**
     * Stops tracking until reset() or the next valid update().
     */
    void deactivate();

    /**
     * Applies relative movement and returns a release point when an outward
     * movement reaches a window edge. Release can be deferred, for example,
     * while a mouse button is held during a drag operation.
     */
    ReleaseDecision update(int deltaX,
                           int deltaY,
                           int windowWidth,
                           int windowHeight,
                           bool releaseAllowed);

    /**
     * Exposed for deterministic unit tests and capture-state diagnostics.
     */
    bool isActive() const { return m_Active; }

private:
    bool m_Active = false;
    int m_X = 0;
    int m_Y = 0;
};
