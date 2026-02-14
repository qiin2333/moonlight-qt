#pragma once

#include <QRasterWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QFont>
#include <QSurfaceFormat>
#include <QElapsedTimer>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <functional>
#include <vector>

/**
 * OverlayMenuPanel - Multi-level Qt overlay menu for streaming sessions.
 *
 * Rendered by OS compositor (DWM), completely independent of the
 * D3D11/SDL/EGL video rendering pipeline.
 *
 * Menu structure:
 *   Level 0 (Top):      Quick Actions >, Bitrate >, Fullscreen, Microphone [toggle], Disconnect
 *   Level 1 (Actions):  Quit, Performance Stats, Mouse Mode, Cursor, Minimize, ...
 *   Level 2 (Bitrate):  1/2/5/10/20/30/50/100 Mbps
 *
 * Sub-level navigation uses a title bar with back button (◂ Title).
 * Win11 dark theme with Segoe MDL2 Assets icons, drop shadow, and slide animations.
 */
class OverlayMenuPanel : public QRasterWindow {
    Q_OBJECT
public:
    enum class MenuAction {
        // Quick actions (keyboard shortcuts)
        Quit,
        QuitAndExit,
        ToggleFullScreen,
        ToggleStatsOverlay,
        ToggleMouseMode,
        ToggleCursorHide,
        ToggleMinimize,
        UngrabInput,
        PasteText,
        TogglePointerRegionLock,
        // Microphone
        ToggleMicrophone,
        // Bitrate presets (kbps)
        SetBitrate1000,
        SetBitrate2000,
        SetBitrate5000,
        SetBitrate10000,
        SetBitrate20000,
        SetBitrate30000,
        SetBitrate50000,
        SetBitrate100000,
        MenuActionMax
    };

    enum class MenuItemType {
        Action,     // dispatch action + close menu
        SubMenu,    // navigate to sub-level
        Toggle,     // dispatch action, toggle visual state, keep menu open
        Back,       // navigate back to top level
    };

    using ActionCallback = std::function<void(MenuAction)>;
    using CloseCallback  = std::function<void()>;

    explicit OverlayMenuPanel(QWindow* parent = nullptr);
    ~OverlayMenuPanel() override;

    void setActionCallback(ActionCallback cb) { m_ActionCallback = cb; }
    void setCloseCallback(CloseCallback cb)   { m_CloseCallback = cb; }

    // Position the panel at the right edge of the given parent rect (SDL pixel coords)
    void showAtRightEdge(int parentX, int parentY, int parentW, int parentH);

    // Position the panel at the left edge of the given parent rect (SDL pixel coords)
    void showAtLeftEdge(int parentX, int parentY, int parentW, int parentH);

    // Position the panel at a specific cursor position (SDL pixel coords)
    void showAtCursor(int parentX, int parentY, int parentW, int parentH,
                      int cursorX, int cursorY);

    void closeMenu();
    bool isMenuVisible() const { return m_Visible; }
    bool isClosing() const { return m_Closing; }
    bool needsEventProcessing() const { return m_Visible || m_Closing; }

    // Update dynamic state before showing the menu
    void updateMicrophoneState(bool enabled);
    void updateBitrateState(int bitrateKbps);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct MenuItem {
        QString      label;
        QString      detail;       // shortcut key, status text, or "✓"
        MenuItemType type;
        MenuAction   action;
        int          targetLevel;  // for SubMenu: which level to navigate to
        bool         enabled;
        bool         toggleState;  // for Toggle: current on/off state
        bool         separatorAfter; // draw group separator after this item
    };

    struct MenuLevel {
        QString              title;
        std::vector<MenuItem> items;
    };

    enum class AnchorMode { RightEdge, LeftEdge, AtCursor };

    void buildMenuLevels();
    void navigateToLevel(int level);
    void repositionWindow();
    void showInternal();     // shared show logic after geometry is set
    int  itemAtPos(const QPoint& pos) const;

    std::vector<MenuLevel> m_MenuLevels;
    int  m_CurrentLevel;
    int  m_HoveredIndex;
    bool m_Visible;

    ActionCallback m_ActionCallback;
    CloseCallback  m_CloseCallback;

    // Parent window rect (SDL pixel coords) for repositioning on level change
    int m_ParentX, m_ParentY, m_ParentW, m_ParentH;

    // Layout constants (logical units, Qt 6 auto-scales)
    int m_ItemHeight;
    int m_Padding;
    int m_MenuWidth;
    int m_BorderRadius;
    int m_ShadowMargin;
    int m_TitleHeight;
    int m_IconAreaWidth;

    // Fonts
    QFont m_LabelFont;
    QFont m_DetailFont;
    QFont m_TitleFont;
    QFont m_IconFont;

    // Anti-flicker: grace period after show
    QElapsedTimer m_ShowTimer;

    // Animations
    QPropertyAnimation* m_OpacityAnim;
    QPropertyAnimation* m_SlideAnim;    // animates x property for slide in/out
    QVariantAnimation*  m_ContentSlideAnim; // animates content offset for level nav
    qreal  m_ContentOffset;   // horizontal paint offset during level transition
    bool   m_Closing;         // true while close animation is running
    int    m_TargetX;         // cached final x position for show animation

    // Menu anchor mode and cursor position (for AtCursor mode)
    AnchorMode m_AnchorMode;
    int m_CursorX, m_CursorY; // SDL pixel coords for AtCursor mode
};
