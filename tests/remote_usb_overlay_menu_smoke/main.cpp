#include "streaming/video/overlaymenupanel.h"

#include <QGuiApplication>
#include <QTextStream>

#include <vector>

namespace {
void selectUsbEntry(OverlayMenuPanel& panel)
{
    panel.showAtRightEdge(0, 0, 1280, 800, std::nullopt, false);
    for (int i = 0; i < 5; ++i) {
        panel.gamepadMoveDown();
    }
    panel.gamepadSelect();
}

bool usbEntryIsHidden()
{
    OverlayMenuPanel panel;
    bool remoteUsbSelected = false;
    OverlayMenuPanel::MenuAction selectedAction =
        OverlayMenuPanel::MenuAction::MenuActionMax;
    panel.setRemoteUsbDeviceCallback(
        [&](const QString&) { remoteUsbSelected = true; });
    panel.setActionCallback(
        [&](OverlayMenuPanel::MenuAction action) { selectedAction = action; });

    selectUsbEntry(panel);
    return !remoteUsbSelected &&
           selectedAction == OverlayMenuPanel::MenuAction::ToggleFullScreen;
}

bool supportedDeviceDispatchesId()
{
    OverlayMenuPanel panel;
    QString selectedId;
    panel.setRemoteUsbDeviceCallback(
        [&](const QString& deviceId) { selectedId = deviceId; });
    panel.updateRemoteUsbState(
        true,
        OverlayMenuPanel::RemoteUsbState::Available,
        {
            {QStringLiteral("blocked"), QStringLiteral("USB Hub"),
             QStringLiteral("Not supported"), false},
            {QStringLiteral("device-42"), QStringLiteral("Test Device"),
             QStringLiteral("Input"), true},
        },
        {},
        QStringLiteral("2 available"));

    selectUsbEntry(panel);
    panel.gamepadMoveDown();
    panel.gamepadMoveDown();
    panel.gamepadSelect();
    return selectedId == QStringLiteral("device-42");
}

bool activeDeviceDispatchesRelease()
{
    OverlayMenuPanel panel;
    bool released = false;
    panel.setRemoteUsbReleaseCallback([&] { released = true; });
    panel.updateRemoteUsbState(
        true,
        OverlayMenuPanel::RemoteUsbState::Open,
        {
            {QStringLiteral("other-device"), QStringLiteral("Other Device"),
             QStringLiteral("Storage"), true},
            {QStringLiteral("active-device"), QStringLiteral("Test Device"),
             QStringLiteral("Input"), true},
        },
        QStringLiteral("active-device"),
        QStringLiteral("Connected"));

    selectUsbEntry(panel);
    panel.gamepadMoveDown();
    panel.gamepadMoveDown();
    panel.gamepadSelect();
    return released;
}
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const bool hidden = usbEntryIsHidden();
    const bool select = supportedDeviceDispatchesId();
    const bool release = activeDeviceDispatchesRelease();
    const bool passed = hidden && select && release;
    QTextStream(stdout) << "remote_usb_overlay_menu="
                        << (passed ? "passed" : "failed")
                        << " hidden=" << hidden
                        << " select=" << select
                        << " release=" << release << Qt::endl;
    return passed ? 0 : 1;
}
