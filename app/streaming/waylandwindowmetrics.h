#pragma once

#include <algorithm>
#include <cmath>

namespace WaylandWindowMetrics {

struct Size
{
    int width = 0;
    int height = 0;
};

struct Point
{
    int x;
    int y;
};

struct CoordinateMetrics
{
    Size window;
    Size touchNormalization;

    bool isValid() const
    {
        return window.width > 0 && window.height > 0 &&
               touchNormalization.width > 0 && touchNormalization.height > 0;
    }
};

inline Size pixelSize(Size logicalSize, double devicePixelRatio)
{
    return {
        std::max(1, static_cast<int>(std::lround(logicalSize.width * devicePixelRatio))),
        std::max(1, static_cast<int>(std::lround(logicalSize.height * devicePixelRatio)))
    };
}

// SDL receives wl_pointer coordinates in Wayland surface coordinates for an
// imported Qt surface. Qt child windows use that same logical coordinate space,
// even though SDL_SetWindowSize() must describe the backing buffer in pixels.
inline int parentCoordinateForPointer(int pointerCoordinate, int parentOrigin)
{
    return parentOrigin + pointerCoordinate;
}

// For ordinary SDL-owned windows, the event and layout extents are identical.
// For an imported wl_surface, SDL normalizes touch using its physical extent;
// converting with that extent reconstructs Qt's logical surface coordinate.
inline int windowCoordinateForNormalizedTouch(float normalizedCoordinate,
                                              int windowCoordinateExtent,
                                              int waylandNormalizationExtent)
{
    const int normalizationExtent = waylandNormalizationExtent > 0
            ? waylandNormalizationExtent : windowCoordinateExtent;
    return static_cast<int>(normalizedCoordinate * normalizationExtent);
}

inline Point windowPointForNormalizedTouch(float normalizedX,
                                           float normalizedY,
                                           const CoordinateMetrics& metrics)
{
    return {
        windowCoordinateForNormalizedTouch(normalizedX,
                                           metrics.window.width,
                                           metrics.touchNormalization.width),
        windowCoordinateForNormalizedTouch(normalizedY,
                                           metrics.window.height,
                                           metrics.touchNormalization.height)
    };
}

inline float logicalNormalizedTouchDistance(float firstX,
                                            float firstY,
                                            float secondX,
                                            float secondY,
                                            const CoordinateMetrics& metrics)
{
    const float xScale = metrics.touchNormalization.width > 0 && metrics.window.width > 0
            ? static_cast<float>(metrics.touchNormalization.width) / metrics.window.width
            : 1.0f;
    const float yScale = metrics.touchNormalization.height > 0 && metrics.window.height > 0
            ? static_cast<float>(metrics.touchNormalization.height) / metrics.window.height
            : 1.0f;
    return std::hypot((firstX - secondX) * xScale,
                      (firstY - secondY) * yScale);
}

} // namespace WaylandWindowMetrics
