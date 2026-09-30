#include "waylandqtsdlbridge.h"

#include <QByteArray>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLibrary>
#include <QPointer>
#include <QScreen>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>

namespace {

constexpr char SdlWaylandDisplayProperty[] = "SDL.video.wayland.wl_display";
constexpr char SdlForeignWindowOpenGlHint[] = "SDL_VIDEO_FOREIGN_WINDOW_OPENGL";
constexpr char SdlWaylandScaleToDisplayHint[] = "SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY";

SDL_Window* ImportedSdlWindow = nullptr;
QPointer<QWindow> ImportedQtWindow;

class WaylandStreamWindow final : public QWindow
{
protected:
    void closeEvent(QCloseEvent* event) override
    {
        SDL_Event quitEvent = {};
        quitEvent.type = SDL_QUIT;
        if (SDL_PushEvent(&quitEvent) < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to queue quit for Wayland stream window: %s",
                        SDL_GetError());
        }

        // Keep the wl_surface alive until Session has stopped the renderer and
        // destroyed SDL's borrowed wrapper. Cleanup deletes this QWindow after
        // SDL_DestroyWindow(), so accepting the close here would leave SDL with
        // a dangling native surface.
        event->ignore();
    }
};

struct Sdl3PropertyApi
{
    using PropertiesId = Uint32;
    using GetGlobalPropertiesFn = PropertiesId (SDLCALL*)();
    using SetPointerPropertyFn = bool (SDLCALL*)(PropertiesId, const char*, void*);

    Sdl3PropertyApi()
        : library(QStringLiteral("SDL3"), 0)
    {
        library.setLoadHints(QLibrary::PreventUnloadHint);
        if (!library.load()) {
            return;
        }

        getGlobalProperties = reinterpret_cast<GetGlobalPropertiesFn>(
                library.resolve("SDL_GetGlobalProperties"));
        setPointerProperty = reinterpret_cast<SetPointerPropertyFn>(
                library.resolve("SDL_SetPointerProperty"));
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
    if (SDL_SetHintWithPriority(SdlWaylandScaleToDisplayHint,
                                "0",
                                SDL_HINT_OVERRIDE) != SDL_TRUE) {
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
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Qt did not expose its native Wayland display");
        return false;
    }

    Sdl3PropertyApi& api = sdl3PropertyApi();
    if (!api.isAvailable()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL3 property API is unavailable; native Wayland requires sdl2-compat with SDL3");
        return false;
    }

    const Sdl3PropertyApi::PropertiesId properties = api.getGlobalProperties();
    if (properties == 0 ||
            !api.setPointerProperty(properties,
                                    SdlWaylandDisplayProperty,
                                    wayland->display())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to import Qt's wl_display into SDL3");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Configured SDL to share Qt's Wayland display");
    return true;
}

QWindow* createStreamWindow(const QString& title,
                            const QRect& geometry,
                            QScreen* screen,
                            Qt::WindowStates initialStates,
                            bool fullScreen)
{
    if (!isNativeWayland()) {
        return nullptr;
    }

    auto* window = new WaylandStreamWindow();
    window->setTitle(title);
    window->setIcon(QGuiApplication::windowIcon());
    if (screen != nullptr) {
        window->setScreen(screen);
    }
    if (geometry.isValid()) {
        window->setGeometry(geometry);
    }

    if (fullScreen) {
        window->showFullScreen();
    }
    else if (initialStates.testFlag(Qt::WindowMaximized)) {
        window->showMaximized();
    }
    else if (initialStates.testFlag(Qt::WindowMinimized)) {
        window->showMinimized();
    }
    else {
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

    const WId surface = window->winId();
    if (surface == 0) {
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
    SDL_Window* sdlWindow = SDL_CreateWindowFrom(reinterpret_cast<void*>(surface));
    SDL_SetHint(SdlForeignWindowOpenGlHint, hadOldHint ? oldHint.constData() : nullptr);
    if (sdlWindow != nullptr) {
        ImportedSdlWindow = sdlWindow;
        ImportedQtWindow = window;
    }
    return sdlWindow;
}

int displayRefreshRate(SDL_Window* window)
{
    if (window != ImportedSdlWindow || ImportedQtWindow == nullptr ||
            ImportedQtWindow->screen() == nullptr) {
        return 0;
    }

    return qRound(ImportedQtWindow->screen()->refreshRate());
}

void forgetStreamWindow(SDL_Window* window)
{
    if (window == ImportedSdlWindow) {
        ImportedSdlWindow = nullptr;
        ImportedQtWindow.clear();
    }
}

} // namespace WaylandQtSdlBridge
