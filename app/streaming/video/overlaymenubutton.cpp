#include "overlaymenubutton.h"

#include "overlaywindowutils.h"

#include <QScreen>
#include <QPainterPath>

OverlayMenuButton::OverlayMenuButton(QWindow* parent)
    : QRasterWindow(parent),
      m_Hovered(false),
      m_ButtonVisible(false),
      m_AutoHideOnLeave(false)
{
    setFlags(OverlayWindowUtils::nonActivatingToolFlags());

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    setOpacity(0.35);
}

OverlayMenuButton::~OverlayMenuButton()
{
}

void OverlayMenuButton::repositionTo(int parentX, int parentY, int parentW, int parentH,
                                     Placement placement, int cursorY)
{
#ifdef Q_OS_MACOS
    int qpX = parentX;
    int qpY = parentY;
    int qpW = parentW;
    int qpH = parentH;
    int qpCursorY = cursorY;
#else
    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    int qpX = qRound(parentX / dpr);
    int qpY = qRound(parentY / dpr);
    int qpW = qRound(parentW / dpr);
    int qpH = qRound(parentH / dpr);
    int qpCursorY = cursorY >= 0 ? qRound(cursorY / dpr) : -1;
#endif

    int x;
    int y;

    switch (placement) {
    case Placement::LeftEdge:
        x = qpX;
        y = qpCursorY >= 0 ? qpY + qpCursorY - kButtonSize / 2 : qpY + (qpH - kButtonSize) / 2;
        break;
    case Placement::RightEdge:
        x = qpX + qpW - kButtonSize;
        y = qpCursorY >= 0 ? qpY + qpCursorY - kButtonSize / 2 : qpY + (qpH - kButtonSize) / 2;
        break;
    case Placement::TopRight:
    default:
        x = qpX + qpW - kButtonSize - kMargin;
        y = qpY + kMargin;
        break;
    }

    int minY = qpY + kMargin;
    int maxY = qpY + qpH - kButtonSize - kMargin;
    if (maxY < minY) {
        maxY = minY;
    }
    y = qBound(minY, y, maxY);

    setGeometry(x, y, kButtonSize, kButtonSize);
}

void OverlayMenuButton::showButton(int parentX, int parentY, int parentW, int parentH,
                                   Placement placement, int cursorY)
{
    repositionTo(parentX, parentY, parentW, parentH, placement, cursorY);
    m_ButtonVisible = true;
    OverlayWindowUtils::showWithoutActivating(this);
    requestUpdate();
}

void OverlayMenuButton::hideButton(bool notify)
{
    bool wasVisible = m_ButtonVisible;
    m_Hovered = false;
    m_ButtonVisible = false;
    setOpacity(0.35);
    hide();

    if (notify && wasVisible && m_HideCallback) {
        m_HideCallback();
    }
}

void OverlayMenuButton::drawCrescentMoon(QPainter& p, qreal cx, qreal cy, qreal radius)
{
    // Crescent moon: full circle minus an offset circle
    QPainterPath moonPath;
    moonPath.addEllipse(QPointF(cx, cy), radius, radius);

    QPainterPath cutout;
    cutout.addEllipse(QPointF(cx + radius * 0.5, cy - radius * 0.25), radius * 0.78, radius * 0.82);

    QPainterPath crescent = moonPath.subtracted(cutout);

    // Soft golden glow
    QColor moonColor = m_Hovered ? QColor(255, 235, 140) : QColor(230, 215, 150);
    p.fillPath(crescent, moonColor);
}

void OverlayMenuButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();

    // Clear to transparent
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(0, 0, w, h, Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    qreal cx = w / 2.0;
    qreal cy = h / 2.0;
    qreal bgRadius = qMin(w, h) / 2.0 - 1.0;

    // Circular dark background
    QPainterPath bgCircle;
    bgCircle.addEllipse(QPointF(cx, cy), bgRadius, bgRadius);

    QColor bgColor = m_Hovered ? QColor(35, 40, 75, 230) : QColor(20, 24, 50, 200);
    p.fillPath(bgCircle, bgColor);

    // Subtle border
    QColor borderColor = m_Hovered ? QColor(120, 150, 230, 150) : QColor(70, 85, 150, 80);
    p.setPen(QPen(borderColor, 1.0));
    p.drawPath(bgCircle);

    // Draw crescent moon centered in the background
    qreal moonR = bgRadius * 0.55;
    drawCrescentMoon(p, cx, cy, moonR);
}

void OverlayMenuButton::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_ClickCallback) {
            m_ClickCallback();
        }
    }
}

void OverlayMenuButton::mouseMoveEvent(QMouseEvent*)
{
    if (!m_Hovered) {
        m_Hovered = true;
        setOpacity(0.95);
        requestUpdate();
    }
}

bool OverlayMenuButton::event(QEvent* ev)
{
    if (ev->type() == QEvent::Leave) {
        m_Hovered = false;
        if (m_AutoHideOnLeave) {
            hideButton();
            return true;
        }
        setOpacity(0.35);
        requestUpdate();
    }
    return QRasterWindow::event(ev);
}
