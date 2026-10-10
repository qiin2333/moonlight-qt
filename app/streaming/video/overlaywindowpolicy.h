#pragma once

#include "utils.h"

#include <QGuiApplication>
#include <Qt>

enum class OverlayWindowMode
{
    Detached,
    WaylandSubsurface,
};

namespace OverlayWindowPolicy {

inline bool useGamescopeOverlayWindowFlags()
{
#if defined(Q_OS_LINUX)
    return WMUtils::isRunningGamescope() &&
           QGuiApplication::platformName() == QStringLiteral("xcb");
#else
    return false;
#endif
}

inline Qt::WindowFlags flags(OverlayWindowMode mode, bool transparentForInput = false)
{
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;
    if (mode == OverlayWindowMode::WaylandSubsurface) {
        flags |= Qt::SubWindow;
    } else {
        flags |= Qt::Tool | Qt::WindowStaysOnTopHint;
        if (useGamescopeOverlayWindowFlags()) {
            // On X11 this maps to override_redirect. Gamescope can compose
            // the surface above Moonlight without registering another app window.
            flags |= Qt::BypassWindowManagerHint;
        }
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
