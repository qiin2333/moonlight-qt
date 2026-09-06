#include "../../app/streaming/video/overlaymenupanel.h"
#include <QGuiApplication>
#include <QTest>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    OverlayMenuPanel panel;
    QString selected;
    panel.setRemoteUsbDeviceCallback([&](const QString &id) { selected = id; });
    const std::vector<OverlayMenuPanel::RemoteUsbDevice> devices {
        {QStringLiteral("1-1"), QStringLiteral("Phone"), QStringLiteral("18D1:4EE7"), true}
    };
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                               devices, {}, QStringLiteral("1 available"));
    // Offscreen cursor remains outside this panel. Explicitly entering a
    // shorter submenu must not trigger the pointer-leave dismissal timer.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), true);
    QTest::qWait(240);
    const int initialHeight = panel.height();
    // Shadow + title + padding + four preceding rows + row center.
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, QPoint(140, 8 + 32 + 4 + 4 * 38 + 19));
    if (panel.height() >= initialHeight) qFatal("USB submenu did not open");
    QTest::qWait(700);
    if (!panel.isVisible()) qFatal("submenu closed while moving toward its devices");
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                               devices, {}, QStringLiteral("1 available"));
    QTest::qWait(700);
    if (!panel.isVisible()) qFatal("device refresh dismissed submenu");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, QPoint(140, 8 + 32 + 4 + 19));
    if (selected != QStringLiteral("1-1")) qFatal("device selection was not dispatched");
    QTest::qWait(220);
    if (panel.isVisible()) qFatal("device selection did not dismiss menu");
    // Reopening restores transient pointer-triggered behavior.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), true);
    QTest::qWait(700);
    if (panel.isVisible()) qFatal("fresh pointer-triggered menu lost auto-dismiss");
    return 0;
}
