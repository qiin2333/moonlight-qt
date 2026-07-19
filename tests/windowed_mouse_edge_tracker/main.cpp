#include "windowedmouseedgetracker.h"

#include <iostream>

namespace {
/**
 * Prints a focused failure message without depending on assertions, which may
 * be compiled out in release builds.
 */
bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "windowed_mouse_edge_tracker: " << message << '\n';
        return false;
    }
    return true;
}
} // namespace

int main()
{
    WindowedMouseEdgeTracker tracker;

    // Ordinary relative motion must remain captured until an edge is reached.
    tracker.reset(50, 50, 100, 100);
    auto decision = tracker.update(10, -5, 100, 100, true);
    if (!expect(!decision.shouldRelease,
                "released before reaching an edge")) {
        return 1;
    }

    // Moving outward through the right edge releases at the last client pixel.
    decision = tracker.update(50, 0, 100, 100, true);
    if (!expect(decision.shouldRelease &&
                decision.x == 99 &&
                decision.y == 45,
                "right-edge release point is incorrect")) {
        return 1;
    }

    // A held mouse button defers release so windowed drag operations are not
    // interrupted, while the next outward movement releases normally.
    tracker.reset(1, 25, 80, 60);
    decision = tracker.update(-4, 0, 80, 60, false);
    if (!expect(!decision.shouldRelease && tracker.isActive(),
                "release was not deferred while dragging")) {
        return 1;
    }
    decision = tracker.update(-1, 0, 80, 60, true);
    if (!expect(decision.shouldRelease &&
                decision.x == 0 &&
                decision.y == 25,
                "deferred left-edge release failed")) {
        return 1;
    }

    // Invalid client sizes deactivate tracking and never request a release.
    tracker.reset(0, 0, 1, 1);
    decision = tracker.update(-1, -1, 1, 1, true);
    if (!expect(!tracker.isActive() && !decision.shouldRelease,
                "invalid bounds should disable tracking")) {
        return 1;
    }

    std::cout << "windowed_mouse_edge_tracker=passed\n";
    return 0;
}
