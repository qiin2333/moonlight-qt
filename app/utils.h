#pragma once

#include <QString>

#define THROW_BAD_ALLOC_IF_NULL(x) \
    if ((x) == nullptr) throw std::bad_alloc()

namespace WMUtils {
    bool isRunningX11();
    // True when the process is running inside Valve's Gamescope compositor
    // (SteamOS Game Mode is the common case). This is intentionally based on
    // the compositor's environment markers instead of the host desktop name.
    bool isRunningGamescope();
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
