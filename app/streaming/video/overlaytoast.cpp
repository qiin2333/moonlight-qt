#include "overlaytoast.h"

#include <QScreen>
#include <QFontMetrics>
#include <QPainterPath>

OverlayToast::OverlayToast(QWindow* parent)
    : QRasterWindow(parent),
      m_FadeAnimation(nullptr),
      m_ToastHeight(40),
      m_HorizPadding(24),
      m_VertPadding(10),
      m_BorderRadius(8)
{
    setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
             | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    m_Font = QFont("Segoe UI", 10);
    m_Font.setWeight(QFont::Medium);

    m_DismissTimer.setSingleShot(true);
    connect(&m_DismissTimer, &QTimer::timeout, this, &OverlayToast::startFadeOut);

    m_FadeAnimation = new QPropertyAnimation(this, "opacity", this);
    m_FadeAnimation->setDuration(400);
    m_FadeAnimation->setStartValue(1.0);
    m_FadeAnimation->setEndValue(0.0);
    connect(m_FadeAnimation, &QPropertyAnimation::finished,
            this, &OverlayToast::onFadeFinished);
}

OverlayToast::~OverlayToast()
{
}

void OverlayToast::showToast(int parentX, int parentY, int parentW, int parentH,
                             const QString& message, int durationMs)
{
    m_Message = message;

    // Stop any ongoing fade / dismiss
    m_DismissTimer.stop();
    m_FadeAnimation->stop();
    setOpacity(1.0);

    // Calculate dimensions
    QFontMetrics fm(m_Font);
    int textWidth = fm.horizontalAdvance(m_Message) + m_HorizPadding * 2;
    int toastWidth = qMin(textWidth, 500);
    if (toastWidth < 120) toastWidth = 120;

    // Convert SDL pixel coords to Qt DIP
    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    int qpX = qRound(parentX / dpr);
    int qpY = qRound(parentY / dpr);
    int qpW = qRound(parentW / dpr);
    int qpH = qRound(parentH / dpr);

    // Position at bottom-center, 60px above the bottom
    int x = qpX + (qpW - toastWidth) / 2;
    int y = qpY + qpH - m_ToastHeight - 60;

    setGeometry(x, y, toastWidth, m_ToastHeight);
    show();
    raise();
    requestUpdate();

    m_DismissTimer.start(durationMs);
}

void OverlayToast::startFadeOut()
{
    m_FadeAnimation->start();
}

void OverlayToast::onFadeFinished()
{
    hide();
    setOpacity(1.0);
}

void OverlayToast::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();

    // Clear to transparent
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(0, 0, w, h, Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Dark semi-transparent rounded background
    QPainterPath bg;
    bg.addRoundedRect(QRectF(0, 0, w, h), m_BorderRadius, m_BorderRadius);
    p.fillPath(bg, QColor(20, 26, 42, 200));

    // White text centered
    p.setFont(m_Font);
    p.setPen(Qt::white);
    p.drawText(QRect(m_HorizPadding, 0, w - m_HorizPadding * 2, h),
               Qt::AlignCenter, m_Message);
}
