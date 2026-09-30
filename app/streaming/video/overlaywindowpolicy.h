#pragma once

#include <Qt>

enum class OverlayWindowMode
{
    Detached,
    WaylandSubsurface,
};

namespace OverlayWindowPolicy {

inline Qt::WindowFlags flags(OverlayWindowMode mode, bool transparentForInput = false)
{
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;
    if (mode == OverlayWindowMode::WaylandSubsurface) {
        flags |= Qt::SubWindow;
    }
    else {
        flags |= Qt::Tool | Qt::WindowStaysOnTopHint;
    }
    if (transparentForInput) {
        flags |= Qt::WindowTransparentForInput;
    }
    return flags;
}

inline bool usesParentCoordinates(OverlayWindowMode mode)
{
    return mode == OverlayWindowMode::WaylandSubsurface;
}

} // namespace OverlayWindowPolicy
