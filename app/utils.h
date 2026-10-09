#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#define THROW_BAD_ALLOC_IF_NULL(x) \
    if ((x) == nullptr) throw std::bad_alloc()

namespace WMUtils {
    bool isRunningX11();
    // True when the process is running inside Valve's Gamescope compositor
    // (SteamOS Game Mode is the common case). This is intentionally based on
    // the compositor's environment markers instead of the host desktop name.
    inline bool isRunningGamescope()
    {
#if defined(Q_OS_LINUX)
        const auto hasGamescopeToken = [](const QByteArray& value) {
            const QByteArray normalized = value.toLower();
            return normalized.split(':').contains("gamescope");
        };
        return !qEnvironmentVariableIsEmpty("GAMESCOPE_WAYLAND_DISPLAY") ||
               hasGamescopeToken(qgetenv("XDG_CURRENT_DESKTOP")) ||
               hasGamescopeToken(qgetenv("XDG_SESSION_DESKTOP"));
#else
        return false;
#endif
    }
    bool isRunningNvidiaProprietaryDriverX11();
    bool supportsDesktopGLWithEGL();
    bool isRunningWayland();
    bool isRunningWindowManager();
    bool isRunningDesktopEnvironment();
    QString getDrmCardOverride();
    bool isGpuSlow();
}

namespace Utils {
    template <typename T>
    bool getEnvironmentVariableOverride(const char* name, T* value) {
        bool ok;
        *value = (T)qEnvironmentVariableIntValue(name, &ok);
        return ok;
    }
}
