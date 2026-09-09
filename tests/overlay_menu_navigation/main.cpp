#include "../../app/streaming/video/overlaymenupanel.h"
#include <QGuiApplication>
#include <QFontDatabase>
#include <QCursor>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QTest>

// Native preview of the production panel, with simulated USB status changes.
class MenuPreview : public QRasterWindow {
public:
    explicit MenuPreview(OverlayMenuPanel &panel) : menu(panel) {
        setTitle(QStringLiteral("USB Menu Interaction Check"));
        setGeometry(150, 100, 900, 700);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(QRect(QPoint(), size()), QColor(35, 38, 45));
        painter.setPen(Qt::white);
        painter.drawText(QRect(30, 30, 800, 100), Qt::AlignLeft,
            QStringLiteral("USB menu preview - simulated device states\nClick outside to close; click the background to reopen.\nEscape: back, then close."));
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (menu.isMenuVisible()) menu.dismissOnOutsideClick(QCursor::pos());
        else menu.showAtCursor(x(), y(), width(), height(), position() + QPoint(250, 150), false);
    }
    void keyReleaseEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) menu.gamepadBack();
    }
private:
    OverlayMenuPanel &menu;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Manrope-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Manrope-SemiBold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/DMMono-Regular.ttf"));
    OverlayMenuPanel panel;
    QString selected;
    int releaseCount = 0;
    const std::vector<OverlayMenuPanel::RemoteUsbDevice> devices {
        {QStringLiteral("1-1"), QStringLiteral("Phone"), QStringLiteral("18D1:4EE7"), true},
        {QStringLiteral("1-2"), QStringLiteral("Keyboard"), QStringLiteral("046D:B34D"), true}
    };
    panel.setRemoteUsbDeviceCallback([&](const QString &id) {
        selected = id;
        panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Opening,
                                   devices, id, QStringLiteral("Connecting"));
    });
    panel.setRemoteUsbReleaseCallback([&] {
        ++releaseCount;
        panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Stopping,
                                   devices, selected, QStringLiteral("Releasing"));
    });
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                               devices, {}, QStringLiteral("1 available"));
    if (app.arguments().contains(QStringLiteral("--interactive"))) {
        MenuPreview preview(panel);
        preview.show();
        panel.setTransientParent(&preview);
        panel.setTitle(QStringLiteral("USB Menu Interaction Check"));
        panel.setRemoteUsbDeviceCallback([&](const QString &id) {
            selected = id;
            panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Opening,
                                       devices, id, QStringLiteral("Connecting"));
            QTimer::singleShot(1200, &panel, [&] {
                panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Open,
                                           devices, selected, QStringLiteral("Connected"));
            });
        });
        panel.setRemoteUsbReleaseCallback([&] {
            panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Stopping,
                                       devices, selected, QStringLiteral("Releasing"));
            QTimer::singleShot(1200, &panel, [&] {
                panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                                           devices, {}, QStringLiteral("1 available"));
            });
        });
        panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 250), false);
        return app.exec();
    }
    // Offscreen cursor remains outside this panel. Explicitly entering a
    // shorter submenu must not trigger the pointer-leave dismissal timer.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), true);
    QTest::qWait(20);
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
    if (!panel.isVisible()) qFatal("connecting status dismissed menu");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, QPoint(140, 101));
    if (selected != QStringLiteral("1-1") || releaseCount != 0)
        qFatal("connecting state allowed switching to another device");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, QPoint(140, 63));
    if (releaseCount != 1 || !panel.isVisible()) qFatal("connecting device cannot be cancelled");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, QPoint(140, 63));
    if (releaseCount != 1) qFatal("stopping device allowed duplicate release");
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Open,
                               devices, selected, QStringLiteral("Connected"));
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, QPoint(140, 63));
    if (releaseCount != 2 || !panel.isVisible()) qFatal("release did not keep status visible");
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                               devices, {}, QStringLiteral("1 available"));
    panel.gamepadBack();
    QTest::qWait(700);
    if (!panel.isVisible() || panel.height() != initialHeight) qFatal("back lost active interaction");
    panel.dismissOnOutsideClick(panel.geometry().center());
    if (!panel.isMenuVisible()) qFatal("inside click dismissed menu");
    panel.dismissOnOutsideClick(QPoint(-100, -100));
    QTest::qWait(220);
    if (panel.isVisible()) qFatal("outside click failed to dismiss menu");
    // Reopening restores transient pointer-triggered behavior.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), true);
    QTest::qWait(700);
    if (panel.isVisible()) qFatal("fresh pointer-triggered menu lost auto-dismiss");
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    panel.gamepadBack();
    QTest::qWait(220);
    if (panel.isVisible()) qFatal("back at top level failed to close menu");

    // --- Bitrate scrubber: preset tap, debounced scrub, wheel, flush ---
    int bitrateCommits = 0;
    int lastBitrate = 0;
    panel.setBitrateChangeCallback([&](int kbps) {
        lastBitrate = kbps;
        ++bitrateCommits;
    });
    panel.updateBitrateState(10000);
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    // Enter the Bitrate submenu (row 2), then tap the 5 Mbps preset (row 3).
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      QPoint(140, 8 + 32 + 4 + 2 * 38 + 19));
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      QPoint(140, 8 + 32 + 4 + 3 * 38 + 19));
    if (lastBitrate != 5000 || bitrateCommits != 1) qFatal("preset tap did not commit");
    if (!panel.isMenuVisible()) qFatal("preset tap closed the menu");
    // D-pad right scrubs upward but must wait out the commit debounce.
    panel.gamepadAdjustSlider(1);
    if (bitrateCommits != 1 || lastBitrate != 5000) qFatal("scrub committed before debounce");
    QTest::qWait(600);
    if (bitrateCommits != 2 || lastBitrate <= 5000) qFatal("scrub never committed");
    const int afterScrub = lastBitrate;
    // One wheel notch on the slider row steps again; A flushes immediately.
    QWheelEvent wheelUp(QPointF(140, 8 + 32 + 4 + 19), QPointF(),
                        QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                        Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&panel, &wheelUp);
    panel.gamepadSelect();
    if (bitrateCommits != 3 || lastBitrate <= afterScrub) qFatal("wheel step not flushed");
    if (!panel.isMenuVisible()) qFatal("slider interaction closed the menu");
    panel.dismissOnOutsideClick(QPoint(-100, -100));
    QTest::qWait(220);
    if (panel.isVisible()) qFatal("outside click after slider use failed to dismiss");
    return 0;
}
