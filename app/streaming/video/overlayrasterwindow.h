#pragma once

#include "overlaywindowpolicy.h"

#include <QRasterWindow>
#include <QImage>
#include <QPainter>

// Qt Wayland cannot change a surface's opacity. Keep the existing window
// opacity API for animations, but paint that opacity into Wayland buffers.
class OverlayRasterWindow : public QRasterWindow
{
    Q_OBJECT
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity NOTIFY opacityChanged)

public:
    explicit OverlayRasterWindow(QWindow* parent, OverlayWindowMode mode)
        : QRasterWindow(parent), m_Mode(mode)
    {
    }

    qreal opacity() const
    {
        return m_Mode == OverlayWindowMode::WaylandSubsurface ? m_PaintOpacity
                                                              : QRasterWindow::opacity();
    }

    void setOpacity(qreal opacity)
    {
        if (m_Mode != OverlayWindowMode::WaylandSubsurface) {
            QRasterWindow::setOpacity(opacity);
            return;
        }

        opacity = qBound(qreal(0), opacity, qreal(1));
        if (m_PaintOpacity != opacity) {
            m_PaintOpacity = opacity;
            update();
            emit opacityChanged(opacity);
        }
    }

protected:
    virtual void paintOverlay(QPainter& painter) = 0;

    void paintEvent(QPaintEvent*) override
    {
        if (m_Mode != OverlayWindowMode::WaylandSubsurface) {
            QPainter painter(this);
            paintOverlay(painter);
            return;
        }

        // Apply opacity to the completed image, just as a compositor does.
        // Setting opacity on each primitive would expose the shadow through
        // the panel and make overlapping strokes more opaque than the rest.
        const qreal scale = devicePixelRatio();
        QImage content(size() * scale, QImage::Format_ARGB32_Premultiplied);
        content.setDevicePixelRatio(scale);
        content.fill(Qt::transparent);
        {
            QPainter painter(&content);
            paintOverlay(painter);
        }
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(QRect(QPoint(), size()), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setOpacity(m_PaintOpacity);
        painter.drawImage(QPoint(), content);
    }

private:
    const OverlayWindowMode m_Mode;
    qreal m_PaintOpacity = 1.0;
};
