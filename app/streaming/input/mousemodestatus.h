#pragma once

#include <QCoreApplication>
#include <QString>

// Shared by the streaming menu and notifications so both describe the same
// client selection. These labels do not imply that the host cursor is visible.
namespace MouseModeStatus {
inline QString mouseMode(bool absolute)
{
    return absolute ? QCoreApplication::translate("MouseModeStatus", "Mouse mode: Absolute")
                    : QCoreApplication::translate("MouseModeStatus", "Mouse mode: Relative");
}

inline QString cursorMode(bool local)
{
    return local ? QCoreApplication::translate("MouseModeStatus", "Cursor: Local")
                 : QCoreApplication::translate("MouseModeStatus", "Cursor: Host video");
}

inline bool takeOverlayShortcutHint(bool relative, bool overlayEnabled, bool& alreadyShown)
{
    if (!relative || !overlayEnabled || alreadyShown) {
        return false;
    }
    alreadyShown = true;
    return true;
}

inline QString overlayShortcutHint()
{
    return QCoreApplication::translate("MouseModeStatus",
                                       "Press Ctrl+Alt+Shift+O to open the overlay menu.");
}
}
