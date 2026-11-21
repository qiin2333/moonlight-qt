#include "settings/streamingpreferences.h"
#include "streaming/input/remotecursorupdate.h"
#include "streaming/input/mousemodestatus.h"
#include "streaming/input/macosdragbuttonstate.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QVector>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        qFatal("%s", message);
    }
}

void testMacSystemDragButtonState()
{
    constexpr int leftButton = 1;
    constexpr int rightButton = 3;
    MacSystemDragButtonState state;
    QVector<int> releases;
    int releaseResult = 0;
    const auto sendRelease = [&](int button) {
        releases.append(button);
        return releaseResult;
    };

    state.recordPress(rightButton);
    releaseResult = -1;
    state.releaseForReset(sendRelease);
    require(releases == QVector<int>{ rightButton },
            "capture loss must attempt to release the original mapped button");
    releaseResult = 0;
    require(state.handleButtonUp(sendRelease), "a failed cleanup must retain release ownership");
    require(releases == QVector<int>({ rightButton, rightButton }),
            "the matching physical release must retry the original mapped button");
    require(!state.handleButtonUp(sendRelease), "successful physical release must clear ownership");

    releases.clear();
    state.recordPress(rightButton);
    releaseResult = -1;
    state.releaseForReset(sendRelease);
    releaseResult = 0;
    state.releaseForReset(sendRelease);
    require(state.handleButtonUp(sendRelease), "a retried cleanup must consume the matching up");
    require(releases == QVector<int>({ rightButton, rightButton }),
            "a later cleanup must retry without duplicating the physical release");

    releases.clear();
    state.recordPress(rightButton);
    releaseResult = -1;
    state.releaseForReset(sendRelease);
    require(state.hasPendingRelease(), "failed cleanup must schedule an automatic retry");
    state.retryPendingRelease(sendRelease);
    require(state.hasPendingRelease(), "failed automatic retry must preserve ownership");
    releaseResult = 0;
    state.retryPendingRelease(sendRelease);
    require(!state.hasPendingRelease(), "successful automatic retry must clear pending state");
    require(state.handleButtonUp(sendRelease), "automatic cleanup retry must consume physical up");
    require(releases == QVector<int>({ rightButton, rightButton, rightButton }),
            "physical up after automatic cleanup must not send another release");

    releases.clear();
    state.recordPress(leftButton);
    releaseResult = -1;
    require(state.handleButtonUp(sendRelease), "a failed physical up must retain ownership");
    releaseResult = 0;
    state.retryPendingRelease(sendRelease);
    require(!state.handleButtonUp(sendRelease),
            "automatic retry after physical up must not consume a later unrelated up");
    require(releases == QVector<int>({ leftButton, leftButton }),
            "automatic retry must release the button whose physical up failed");

    releases.clear();
    state.recordPress(rightButton);
    releaseResult = -1;
    require(state.handleButtonUp(sendRelease), "the old physical up must be consumed");
    require(!state.allowButtonDown(sendRelease), "a new press must wait for the pending release");
    releaseResult = 0;
    state.retryPendingRelease(sendRelease);
    require(state.handleButtonUp(sendRelease),
            "automatic retry must consume the up of the suppressed new press");
    require(releases == QVector<int>({ rightButton, rightButton, rightButton }),
            "the suppressed new press must not cause an extra release");

    releases.clear();
    state.recordPress(leftButton);
    state.releaseForReset(sendRelease);
    require(state.handleButtonUp(sendRelease), "cleanup must consume the matching physical up");
    require(releases == QVector<int>{ leftButton },
            "a successful cleanup must not queue a duplicate release");
    require(!state.handleButtonUp(sendRelease), "the physical up must be consumed only once");

    releases.clear();
    state.recordPress(rightButton);
    releaseResult = -1;
    require(state.handleButtonUp(sendRelease), "a failed physical release must be consumed");
    require(!state.allowButtonDown(sendRelease),
            "a new press must be suppressed while the old release still fails");
    releaseResult = 0;
    require(state.allowButtonDown(sendRelease),
            "a new press may proceed after the release retries");
    require(releases == QVector<int>({ rightButton, rightButton, rightButton }),
            "each retry must release the original button before accepting another press");
    state.recordPress(leftButton);
    require(state.handleButtonUp(sendRelease), "the new press must own its matching release");
    require(releases.last() == leftButton, "the new press must release its own mapped button");

    releases.clear();
    state.recordPress(leftButton);
    state.releaseForReset(sendRelease);
    require(state.allowButtonDown(sendRelease), "a new press must clear a stale pending up");
    require(!state.handleButtonUp(sendRelease),
            "an unrelated physical up must not be consumed after a new press");
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testMacSystemDragButtonState();
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
