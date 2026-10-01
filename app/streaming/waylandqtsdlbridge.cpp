#include "waylandqtsdlbridge.h"
#include "waylandstreamwindow.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QLibrary>
#include <QOpenGLContext>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformwindow_p.h>

namespace {

constexpr char SdlWaylandDisplayProperty[] = "SDL.video.wayland.wl_display";
constexpr char SdlForeignWindowOpenGlHint[] = "SDL_VIDEO_FOREIGN_WINDOW_OPENGL";
constexpr char SdlWaylandScaleToDisplayHint[] = "SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY";

SDL_Window* ImportedSdlWindow = nullptr;
QPointer<QWindow> ImportedQtWindow;

std::mutex SurfaceMutex;

struct Sdl3PropertyApi
{
    using PropertiesId = Uint32;
    using GetGlobalPropertiesFn = PropertiesId(SDLCALL*)();
    using SetPointerPropertyFn = bool(SDLCALL*)(PropertiesId, const char*, void*);

    Sdl3PropertyApi() : library(QStringLiteral("SDL3"), 0)
    {
        library.setLoadHints(QLibrary::PreventUnloadHint);
        if (!library.load()) {
            return;
        }

        getGlobalProperties =
            reinterpret_cast<GetGlobalPropertiesFn>(library.resolve("SDL_GetGlobalProperties"));
        setPointerProperty =
            reinterpret_cast<SetPointerPropertyFn>(library.resolve("SDL_SetPointerProperty"));
    }

    bool isAvailable() const
    {
        return getGlobalProperties != nullptr && setPointerProperty != nullptr;
    }

    QLibrary library;
    GetGlobalPropertiesFn getGlobalProperties = nullptr;
    SetPointerPropertyFn setPointerProperty = nullptr;
};

Sdl3PropertyApi& sdl3PropertyApi()
{
    static Sdl3PropertyApi api;
    return api;
}

} // namespace

namespace WaylandQtSdlBridge {

SdlVideoLifetime::~SdlVideoLifetime()
{
    if (!m_Initialized) {
        return;
    }

    SDL_GL_UnloadLibrary();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool SdlVideoLifetime::initialize()
{
    if (m_Initialized) {
        return true;
    }
    if (!configureSdlVideo()) {
        return false;
    }
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to retain SDL video for Qt Wayland: %s",
                     SDL_GetError());
        return false;
    }
    if (SDL_GL_LoadLibrary(nullptr) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to retain SDL EGL for Qt Wayland: %s",
                     SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    m_Initialized = true;
    return true;
}

bool isNativeWayland()
{
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
}

bool configureSdlVideo()
{
    if (!isNativeWayland()) {
        return true;
    }

    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL video was initialized before Wayland display sharing");
        return false;
    }

    // The imported Qt surface uses Qt logical coordinates while the SDL
    // wrapper is sized to the physical buffer. This must override both SDL's
    // default and any environment value, or SDL rescales pointer and window
    // coordinates a second time on high-DPI outputs.
    if (SDL_SetHintWithPriority(SdlWaylandScaleToDisplayHint, "0", SDL_HINT_OVERRIDE) != SDL_TRUE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to disable SDL Wayland scale-to-display");
        return false;
    }

    auto* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (guiApp == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "No QGuiApplication is available for Wayland display sharing");
        return false;
    }

    auto* wayland = guiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (wayland == nullptr || wayland->display() == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Qt did not expose its native Wayland display");
        return false;
    }

    Sdl3PropertyApi& api = sdl3PropertyApi();
    if (!api.isAvailable()) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "SDL3 property API is unavailable; native Wayland requires sdl2-compat with SDL3");
        return false;
    }

    const Sdl3PropertyApi::PropertiesId properties = api.getGlobalProperties();
    if (properties == 0 ||
        !api.setPointerProperty(properties, SdlWaylandDisplayProperty, wayland->display())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to import Qt's wl_display into SDL3");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Configured SDL to share Qt's Wayland display");
    return true;
}

QWindow* createStreamWindow(const QString& title, const QRect& geometry, QScreen* screen,
                            Qt::WindowStates initialStates, bool fullScreen)
{
    if (!isNativeWayland()) {
        return nullptr;
    }

    auto* window = new WaylandStreamWindow(SurfaceMutex);
    window->setTitle(title);
    window->setIcon(QGuiApplication::windowIcon());
    if (screen != nullptr) {
        window->setScreen(screen);
    }
    if (geometry.isValid()) {
        window->setGeometry(geometry);
    }

    window->setWindowStates(fullScreen ? Qt::WindowFullScreen : initialStates);
    if (!window->initializeFrame()) {
        delete window;
        return nullptr;
    }

    if (fullScreen) {
        window->showFullScreen();
    } else if (initialStates.testFlag(Qt::WindowMaximized)) {
        window->showMaximized();
    } else if (initialStates.testFlag(Qt::WindowMinimized)) {
        window->showMinimized();
    } else {
        window->show();
    }

    // Ensure Qt has created the wl_surface before SDL_CreateWindowFrom().
    window->create();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    return window;
}

SDL_Window* wrapStreamWindow(QWindow* window)
{
    if (window == nullptr || !isNativeWayland()) {
        return nullptr;
    }

    auto* native = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    auto* surface = native != nullptr ? native->surface() : nullptr;
    if (surface == nullptr) {
        SDL_SetError("Qt failed to create the Wayland stream surface");
        return nullptr;
    }

    // SDL2's compatibility entry point forwards this wl_surface through the
    // sdl2-compat.external_window property. Create it as OpenGL-capable so the
    // normal renderer fallback path remains usable; Vulkan can reconfigure an
    // external window without replacing its native surface.
    const char* oldHintValue = SDL_GetHint(SdlForeignWindowOpenGlHint);
    const bool hadOldHint = oldHintValue != nullptr;
    const QByteArray oldHint = oldHintValue ? QByteArray(oldHintValue) : QByteArray();
    SDL_SetHint(SdlForeignWindowOpenGlHint, "1");
    SDL_Window* sdlWindow = SDL_CreateWindowFrom(surface);
    SDL_SetHint(SdlForeignWindowOpenGlHint, hadOldHint ? oldHint.constData() : nullptr);
    if (sdlWindow != nullptr) {
        ImportedSdlWindow = sdlWindow;
        ImportedQtWindow = window;
    }
    return sdlWindow;
}

bool isStreamWindowMinimized(QWindow* window)
{
    auto* streamWindow = dynamic_cast<WaylandStreamWindow*>(window);
    return streamWindow != nullptr && streamWindow->isMinimized();
}

bool exposeGuiBeforeSdlTeardown(QWindow* guiWindow, QWindow* streamWindow, bool keepMinimized)
{
    if (guiWindow == nullptr || streamWindow == nullptr || !isNativeWayland()) {
        return false;
    }

    // SDL renderer destruction changes the native current context without
    // updating Qt's thread-local cache. Explicitly rebind through Qt before
    // QRhi can trust that cache. Rebinding also makes any pending Qt GL
    // resource cleanup safe; merely calling doneCurrent on a stale cache can
    // otherwise run that cleanup without the correct native context.
    if (auto* context = QOpenGLContext::currentContext()) {
        if (context->surface() != nullptr && !context->makeCurrent(context->surface())) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to restore Qt's OpenGL context after Wayland SDL teardown");
            return false;
        }
    }

    if (keepMinimized) {
        guiWindow->showMinimized();
        return true;
    }

    // SDL's OpenGL renderer owns an EGL surface for the Qt-owned stream
    // wl_surface. Keep the surviving Qt Quick window exposed while that EGL
    // surface is destroyed, otherwise some EGL implementations leave Qt's
    // hidden QRhi surface unable to resume after the stream window is gone.
    // The stream window remains above the GUI throughout this handoff.
    guiWindow->setVisible(true);

    constexpr qint64 RestoreTimeoutMs = 500;
    QElapsedTimer timer;
    timer.start();
    while (!guiWindow->isExposed() && timer.elapsed() < RestoreTimeoutMs) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents();
        QThread::msleep(1);
    }

    if (!guiWindow->isExposed()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Qt GUI did not expose before Wayland SDL teardown");
        return false;
    }
    return true;
}

int displayRefreshRate(SDL_Window* window)
{
    if (window != ImportedSdlWindow || ImportedQtWindow == nullptr ||
        ImportedQtWindow->screen() == nullptr) {
        return 0;
    }

    return qRound(ImportedQtWindow->screen()->refreshRate());
}

std::unique_lock<std::mutex> lockSurfaceForRendering(SDL_Window* window)
{
    return window != nullptr && window == ImportedSdlWindow
               ? std::unique_lock<std::mutex>(SurfaceMutex)
               : std::unique_lock<std::mutex>();
}

void forgetStreamWindow(SDL_Window* window)
{
    if (window == ImportedSdlWindow) {
        ImportedSdlWindow = nullptr;
        ImportedQtWindow.clear();
    }
}

} // namespace WaylandQtSdlBridge
