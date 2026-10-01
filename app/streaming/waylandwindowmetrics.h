#pragma once

#include <algorithm>
#include <cmath>

namespace WaylandWindowMetrics {

struct Size
{
    int width = 0;
    int height = 0;
};

struct CoordinateMetrics
{
    Size window;
    Size touchNormalization;

    bool isValid() const
    {
        return window.width > 0 && window.height > 0 && touchNormalization.width > 0 &&
               touchNormalization.height > 0;
    }
};

// SDL normalizes imported-surface touch positions and deltas using the buffer
// size. Normalize once at dispatch so absolute input, virtual trackpads, and
// all gesture thresholds use fractions of the logical client area.
template <typename TouchEvent>
inline TouchEvent logicalTouchEvent(TouchEvent event, const CoordinateMetrics& metrics)
{
    if (metrics.isValid()) {
        const float xScale =
            static_cast<float>(metrics.touchNormalization.width) / metrics.window.width;
        const float yScale =
            static_cast<float>(metrics.touchNormalization.height) / metrics.window.height;
        event.x *= xScale;
        event.y *= yScale;
        event.dx *= xScale;
        event.dy *= yScale;
    }
    return event;
}

inline Size pixelSize(Size logicalSize, double devicePixelRatio)
{
    return { (std::max)(1, static_cast<int>(std::lround(logicalSize.width * devicePixelRatio))),
             (std::max)(1, static_cast<int>(std::lround(logicalSize.height * devicePixelRatio))) };
}

} // namespace WaylandWindowMetrics
