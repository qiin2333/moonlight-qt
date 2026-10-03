#pragma once

#include "SDL_compat.h"

#include <QRect>
#include <QString>
#include <Qt>
#include <mutex>

class QScreen;
class QWindow;

namespace WaylandQtSdlBridge {

// Declare after QGuiApplication and before Qt windows. This keeps SDL from
// terminating Qt's shared EGLDisplay until all Qt rendering is finished, while
// still releasing SDL's borrowed Wayland objects before Qt closes wl_display.
class SdlVideoLifetime final
{
public:
    SdlVideoLifetime() = default;
    ~SdlVideoLifetime();

    SdlVideoLifetime(const SdlVideoLifetime&) = delete;
    SdlVideoLifetime& operator=(const SdlVideoLifetime&) = delete;

    bool initialize();

private:
    bool m_Initialized = false;
};

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
bool isStreamWindowMinimized(QWindow* window);

// Re-expose the surviving Qt GUI behind the stream before SDL destroys its
// EGL surface for the borrowed wl_surface. A minimized GUI is restored without
// waiting for exposure, which the compositor intentionally withholds.
bool exposeGuiBeforeSdlTeardown(QWindow* guiWindow, QWindow* streamWindow,
                                bool keepMinimized = false);

// SDL cannot attach output enter/leave listeners to a Qt-owned wl_surface.
// Keep screen/refresh queries on Qt for the imported stream window.
int displayRefreshRate(SDL_Window* window);

// Serialize parent commits for local overlays with buffer presentation.
std::unique_lock<std::mutex> lockSurfaceForRendering(SDL_Window* window);
void forgetStreamWindow(SDL_Window* window);

} // namespace WaylandQtSdlBridge
