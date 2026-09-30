#pragma once

#include "SDL_compat.h"

#include <QRect>
#include <QString>
#include <Qt>

class QScreen;
class QWindow;

namespace WaylandQtSdlBridge {

// Native Wayland is the only platform where the stream window must be owned by
// Qt. Every other window system keeps using SDL's normal top-level window path.
bool isNativeWayland();

// Import Qt's wl_display into SDL3 before SDL_INIT_VIDEO. This is required for
// an SDL window to wrap a wl_surface created by Qt.
bool configureSdlVideo();

// Create the Qt-owned Wayland toplevel and wrap its wl_surface in an SDL_Window.
QWindow* createStreamWindow(const QString& title, const QRect& geometry, QScreen* screen,
                            Qt::WindowStates initialStates, bool fullScreen);
SDL_Window* wrapStreamWindow(QWindow* window);

// SDL cannot attach output enter/leave listeners to a Qt-owned wl_surface.
// Keep screen/refresh queries on Qt for the imported stream window.
int displayRefreshRate(SDL_Window* window);
void forgetStreamWindow(SDL_Window* window);

} // namespace WaylandQtSdlBridge
