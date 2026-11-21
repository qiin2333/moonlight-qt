#include "settings/streamingpreferences.h"
#include "streaming/input/remotecursorupdate.h"
#include "streaming/input/mousemodestatus.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        qFatal("%s", message);
    }
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "test settings directory must be available");
    QSettings settings(directory.filePath("mouse-mode.ini"), QSettings::IniFormat);
    using P = StreamingPreferences;

    require(P::loadMouseMode(settings) == P::MM_ABSOLUTE_LOCAL,
            "new installations must default to local desktop cursor mode");
    settings.setValue("defaultver", 1);
    require(P::loadMouseMode(settings) == P::MM_RELATIVE,
            "existing installations without stored mouse options must keep the old default");
    settings.setValue("mouseacceleration", false);
    settings.setValue("showLocalCursor", false);
    require(P::loadMouseMode(settings) == P::MM_RELATIVE,
            "existing relative mode must not change on upgrade");
    settings.setValue("mouseacceleration", true);
    require(P::loadMouseMode(settings) == P::MM_ABSOLUTE_VIDEO,
            "legacy absolute mode without local cursor must keep the video cursor");
    settings.setValue("showLocalCursor", true);
    require(P::loadMouseMode(settings) == P::MM_ABSOLUTE_LOCAL,
            "legacy absolute mode with local cursor must be preserved");
    settings.setValue("mouseacceleration", false);
    require(P::loadMouseMode(settings) == P::MM_ABSOLUTE_LOCAL,
            "legacy local cursor selection must enable absolute input");
    settings.setValue("mouseMode", "invalid");
    require(P::loadMouseMode(settings) == P::MM_RELATIVE,
            "invalid stored modes must fall back to relative input");
    settings.setValue("mouseMode", 42);
    require(P::loadMouseMode(settings) == P::MM_RELATIVE,
            "unknown stored modes must fall back to relative input");

    for (auto mode : { P::MM_RELATIVE, P::MM_ABSOLUTE_VIDEO, P::MM_ABSOLUTE_LOCAL }) {
        P::saveMouseMode(settings, mode);
        require(P::loadMouseMode(settings) == mode, "stored mode must round-trip");
        require(settings.value("mouseacceleration").toBool() == (mode != P::MM_RELATIVE),
                "legacy absolute setting must match the new mode");
        require(settings.value("showLocalCursor").toBool() == (mode == P::MM_ABSOLUTE_LOCAL),
                "legacy cursor setting must match the new mode");
    }

    auto lastAbsolute = P::MM_ABSOLUTE_LOCAL;
    auto current = P::toggleMouseInputMode(P::MM_RELATIVE, lastAbsolute);
    require(current == P::MM_ABSOLUTE_LOCAL, "first input toggle should use local cursor mode");
    current = P::toggleMouseCursorMode(current);
    require(current == P::MM_ABSOLUTE_VIDEO, "cursor toggle should select video cursor");
    current = P::toggleMouseInputMode(current, lastAbsolute);
    require(current == P::MM_RELATIVE && lastAbsolute == P::MM_ABSOLUTE_VIDEO,
            "switching to relative mode must retain the previous absolute cursor choice");
    current = P::toggleMouseInputMode(current, lastAbsolute);
    require(current == P::MM_ABSOLUTE_VIDEO,
            "returning to absolute mode must restore the cursor choice");
    require(P::toggleMouseCursorMode(P::MM_RELATIVE) == P::MM_RELATIVE,
            "cursor toggle must not change relative input");
    require(P::showHostCursorLocally(P::MM_ABSOLUTE_LOCAL, true, true),
            "a supported visible host cursor should use the local cursor");
    require(P::showHostCursorLocally(P::MM_ABSOLUTE_LOCAL, false, true),
            "an unsupported host must retain the generic local pointer fallback");
    require(!P::showHostCursorLocally(P::MM_ABSOLUTE_LOCAL, true, false),
            "a hidden host cursor should also be hidden locally");
    require(!P::showHostCursorLocally(P::MM_ABSOLUTE_VIDEO, true, true),
            "video cursor mode must not display a second local cursor");

    bool hintShown = false;
    require(!MouseModeStatus::takeOverlayShortcutHint(false, true, hintShown) && !hintShown,
            "absolute mode must not consume the relative overlay hint");
    require(!MouseModeStatus::takeOverlayShortcutHint(true, false, hintShown) && !hintShown,
            "a disabled overlay must not consume or show the shortcut hint");
    require(MouseModeStatus::takeOverlayShortcutHint(true, true, hintShown) && hintShown,
            "first relative capture with an enabled overlay must show the hint");
    require(!MouseModeStatus::takeOverlayShortcutHint(true, true, hintShown),
            "the overlay shortcut hint must be shown only once per session");
    require(MouseModeStatus::overlayShortcutHint().contains(QStringLiteral("Ctrl+Alt+Shift+O")),
            "the overlay hint must name the menu shortcut");

    RemoteCursorUpdate shape;
    shape.hasShape = true;
    shape.visible = true;
    shape.shapeId = 7;
    shape.width = 1;
    shape.height = 1;
    shape.bgra = QByteArray(4, 'x');
    RemoteCursorUpdate visibility;
    visibility.visible = false;
    visibility.shapeId = 7;
    shape.coalesce(visibility);
    require(shape.hasShape && shape.shapeId == 7 && shape.bgra == QByteArray(4, 'x') &&
                !shape.visible,
            "visibility-only updates must not discard an unprocessed cursor shape");
    RemoteCursorUpdate replacement;
    replacement.hasShape = true;
    replacement.visible = true;
    replacement.shapeId = 8;
    replacement.bgra = QByteArray(4, 'y');
    shape.coalesce(replacement);
    require(shape.hasShape && shape.shapeId == 8 && shape.bgra == QByteArray(4, 'y') &&
                shape.visible,
            "a newer shape must replace the previous pending shape");
    return 0;
}
