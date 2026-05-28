#pragma once

#include <QRasterWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <functional>

/**
 * OverlayMenuButton - A small floating button rendered by the OS compositor,
 * positioned at the top-right corner of the streaming window.
 *
 * When clicked, triggers a callback to open the overlay menu.
 * Semi-transparent when idle, fully opaque on hover.
 * Independent of D3D11/SDL rendering pipeline.
 */
class OverlayMenuButton : public QRasterWindow {
    Q_OBJECT
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity)

public:
    using ClickCallback = std::function<void()>;

    enum class Placement {
        TopRight,
        LeftEdge,
        RightEdge,
    };

    explicit OverlayMenuButton(QWindow* parent = nullptr);
    ~OverlayMenuButton() override;

    void setClickCallback(ClickCallback cb) { m_ClickCallback = std::move(cb); }
    void setAutoHideOnLeave(bool autoHide) { m_AutoHideOnLeave = autoHide; }

    /**
     * Reposition the button relative to the given parent rect (SDL pixel coords).
     * Places the button at the requested location on the streaming window.
     */
    void repositionTo(int parentX, int parentY, int parentW, int parentH,
                      Placement placement = Placement::TopRight,
                      int cursorY = -1);

    /**
     * Show the button at the requested location on the given parent rect.
     */
    void showButton(int parentX, int parentY, int parentW, int parentH,
                    Placement placement = Placement::TopRight,
                    int cursorY = -1);

    /**
     * Hide the button.
     */
    void hideButton();

    bool isButtonVisible() const { return m_ButtonVisible; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;

private:
    void drawCrescentMoon(QPainter& p, qreal cx, qreal cy, qreal radius);

    ClickCallback m_ClickCallback;
    bool m_Hovered;
    bool m_ButtonVisible;
    bool m_AutoHideOnLeave;

    // Button size (logical pixels)
    static constexpr int kButtonSize = 36;
    static constexpr int kMargin = 10;  // margin from window edge
};
