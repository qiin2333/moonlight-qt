#include "overlaymenubutton.h"

#include <QGuiApplication>
#include <QScreen>
#include <QPainterPath>
#include <QStyleHints>

namespace {
QPoint globalMousePosition(QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}
}

OverlayMenuButton::OverlayMenuButton(QWindow* parent)
    : QRasterWindow(parent),
      m_Hovered(false),
      m_ButtonVisible(false),
      m_Pressed(false),
      m_Dragging(false)
{
    setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
             | Qt::WindowDoesNotAcceptFocus);

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    setOpacity(0.35);
}

OverlayMenuButton::~OverlayMenuButton()
{
}

void OverlayMenuButton::repositionTo(int parentX, int parentY, int parentW, int parentH)
{
    const QRect newParentGeometry(parentX, parentY, parentW, parentH);
    QPoint newPosition;

    if (m_ParentGeometry.isValid()) {
        // Preserve the button's offset when the streaming window moves or resizes.
        newPosition = newParentGeometry.topLeft() + (position() - m_ParentGeometry.topLeft());
    }
    else {
        newPosition = QPoint(newParentGeometry.right() - kButtonSize - kMargin + 1,
                             newParentGeometry.top() + kMargin);
    }

    m_ParentGeometry = newParentGeometry;
    setGeometry(QRect(clampToParent(newPosition), QSize(kButtonSize, kButtonSize)));
}

void OverlayMenuButton::showButton(int parentX, int parentY, int parentW, int parentH)
{
    repositionTo(parentX, parentY, parentW, parentH);
    m_ButtonVisible = true;
    show();
    raise();
    requestUpdate();
}

void OverlayMenuButton::hideButton()
{
    m_Pressed = false;
    m_Dragging = false;
    m_ButtonVisible = false;
    unsetCursor();
    hide();
}

QPoint OverlayMenuButton::clampToParent(const QPoint& position) const
{
    if (!m_ParentGeometry.isValid()) {
        return position;
    }

    const int minX = m_ParentGeometry.left() + kMargin;
    const int minY = m_ParentGeometry.top() + kMargin;
    const int maxX = qMax(minX, m_ParentGeometry.right() - kButtonSize - kMargin + 1);
    const int maxY = qMax(minY, m_ParentGeometry.bottom() - kButtonSize - kMargin + 1);
    return QPoint(qBound(minX, position.x(), maxX),
                  qBound(minY, position.y(), maxY));
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
        m_Pressed = true;
        m_Dragging = false;
        m_PressGlobalPosition = globalMousePosition(event);
        m_WindowPositionAtPress = position();
        event->accept();
    }
}

void OverlayMenuButton::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_Hovered) {
        m_Hovered = true;
        setOpacity(0.95);
        requestUpdate();
    }

    if (!m_Pressed) {
        setCursor(Qt::OpenHandCursor);
        return;
    }

    const QPoint delta = globalMousePosition(event) - m_PressGlobalPosition;
    if (!m_Dragging && delta.manhattanLength() >= QGuiApplication::styleHints()->startDragDistance()) {
        m_Dragging = true;
        setCursor(Qt::ClosedHandCursor);
    }

    if (m_Dragging) {
        setPosition(clampToParent(m_WindowPositionAtPress + delta));
        event->accept();
    }
}

void OverlayMenuButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_Pressed) {
        return;
    }

    const bool activate = !m_Dragging;
    m_Pressed = false;
    m_Dragging = false;
    setCursor(Qt::OpenHandCursor);
    event->accept();

    if (activate && m_ClickCallback) {
        m_ClickCallback();
    }
}

bool OverlayMenuButton::event(QEvent* ev)
{
    if (ev->type() == QEvent::Leave) {
        m_Hovered = false;
        setOpacity(0.35);
        requestUpdate();
    }
    return QRasterWindow::event(ev);
}
