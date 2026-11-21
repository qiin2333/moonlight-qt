#include "streamingpreferences.h"

#include <QSettings>

namespace {
constexpr auto mouseModeKey = "mouseMode";
constexpr auto legacyAbsoluteKey = "mouseacceleration";
constexpr auto legacyLocalCursorKey = "showLocalCursor";
}

StreamingPreferences::MouseMode StreamingPreferences::loadMouseMode(const QSettings& settings)
{
    if (settings.contains(mouseModeKey)) {
        bool ok = false;
        const int mode = settings.value(mouseModeKey).toInt(&ok);
        if (ok && mode >= MM_RELATIVE && mode <= MM_ABSOLUTE_LOCAL) {
            return static_cast<MouseMode>(mode);
        }
        return MM_RELATIVE;
    }

    // A new installation defaults to desktop pointer mode. Existing settings
    // are migrated separately so upgrading does not replace a saved choice.
    if (!settings.contains(legacyAbsoluteKey) && !settings.contains(legacyLocalCursorKey)) {
        return settings.contains("defaultver") ? MM_RELATIVE : MM_ABSOLUTE_LOCAL;
    }

    const bool absolute = settings.value(legacyAbsoluteKey, false).toBool();
    const bool localCursor = settings.value(legacyLocalCursorKey, false).toBool();
    return localCursor ? MM_ABSOLUTE_LOCAL : absolute ? MM_ABSOLUTE_VIDEO : MM_RELATIVE;
}

void StreamingPreferences::saveMouseMode(QSettings& settings, MouseMode mode)
{
    settings.setValue(mouseModeKey, static_cast<int>(mode));
    // Keep the legacy values in sync for users who return to an older build.
    settings.setValue(legacyAbsoluteKey, isAbsoluteMouseMode(mode));
    settings.setValue(legacyLocalCursorKey, mode == MM_ABSOLUTE_LOCAL);
}

StreamingPreferences::MouseMode
StreamingPreferences::toggleMouseInputMode(MouseMode mode, MouseMode& lastAbsoluteMode)
{
    if (mode == MM_RELATIVE) {
        return lastAbsoluteMode == MM_ABSOLUTE_VIDEO ? MM_ABSOLUTE_VIDEO : MM_ABSOLUTE_LOCAL;
    }
    lastAbsoluteMode = mode;
    return MM_RELATIVE;
}

StreamingPreferences::MouseMode StreamingPreferences::toggleMouseCursorMode(MouseMode mode)
{
    if (mode == MM_ABSOLUTE_VIDEO) {
        return MM_ABSOLUTE_LOCAL;
    }
    return mode == MM_ABSOLUTE_LOCAL ? MM_ABSOLUTE_VIDEO : MM_RELATIVE;
}

bool StreamingPreferences::showHostCursorLocally(MouseMode mode, bool hostSupportsCursor,
                                                 bool hostCursorVisible)
{
    return mode == MM_ABSOLUTE_LOCAL && (!hostSupportsCursor || hostCursorVisible);
}
