#pragma once

#include <QRasterWindow>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPoint>
#include <QFont>
#include <QIcon>
#include <QHash>
#include <QSurfaceFormat>
#include <QElapsedTimer>
#include <QTimer>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

/**
 * OverlayMenuPanel - Multi-level Qt overlay menu for streaming sessions.
 *
 * Rendered by OS compositor (DWM), completely independent of the
 * D3D11/SDL/EGL video rendering pipeline.
 *
 * Menu structure:
 *   Level 0 (Top):      Quick Actions >, Menu Position >, Bitrate >, Fullscreen, Microphone [toggle], Disconnect
 *   Level 1 (Actions):  Quit, Performance Stats, Mouse Mode, Cursor, Minimize, ...
 *   Level 2 (Bitrate):  log-scale scrubber row + 1/2/5/10/20/30/50/100 Mbps presets
 *   Level 3 (Position): Top, Right, Left, Floating button, Disabled
 *   Developer builds may append a function-test panel entry.
 *
 * Sub-level navigation uses a title bar with back button (◂ Title).
 * Square industrial theme matching Theme.qml, with hard shadows and brand accents.
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
        ShowHostFiles,
        SelectRemoteUsbDevice,
        ReleaseRemoteUsbDevice,
        // Microphone
        ToggleMicrophone,
        // Gamepad mouse emulation
        ToggleGamepadMouse,
        // Set bitrate to the kbps value carried in MenuItem::payload.
        // Handled inside the panel (slider row + presets); never dispatched.
        SetBitrate,
        SetMenuPlacementTop,
        SetMenuPlacementRight,
        SetMenuPlacementLeft,
        SetMenuPlacementButton,
        SetMenuPlacementDisabled,
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
        OpenStylusReplayPanel,
#endif
        MenuActionMax
    };

    enum class FileMappingState {
        Unknown,
        Checking,
        Unavailable,
        Available,
        Mounting,
        Open,
        Error
    };

    enum class RemoteUsbState {
        Unavailable,
        Discovering,
        Available,
        Opening,
        Open,
        Stopping,
        Error
    };

    struct RemoteUsbDevice {
        QString id;
        QString label;
        QString detail;
        bool supported = true;
    };

    enum class MenuItemType {
        Action,     // dispatch action + close menu
        SubMenu,    // navigate to sub-level
        Toggle,     // dispatch action, toggle visual state, keep menu open
        Back,       // navigate back to top level
        Slider,     // in-row value scrubber (drag/wheel/gamepad); keeps menu open
    };

    using ActionCallback = std::function<void(MenuAction)>;
    using CloseCallback  = std::function<void()>;
    using RemoteUsbDeviceCallback = std::function<void(const QString&)>;
    using RemoteUsbReleaseCallback = std::function<void()>;
    // Fired when a bitrate adjustment settles (debounced while scrubbing,
    // immediate for preset taps). The value is in kbps.
    using BitrateChangeCallback = std::function<void(int)>;

    explicit OverlayMenuPanel(QWindow* parent = nullptr);
    ~OverlayMenuPanel() override;

    void setActionCallback(ActionCallback cb) { m_ActionCallback = cb; }
    void setCloseCallback(CloseCallback cb)   { m_CloseCallback = cb; }
    void setRemoteUsbDeviceCallback(RemoteUsbDeviceCallback cb) {
        m_RemoteUsbDeviceCallback = std::move(cb);
    }
    void setRemoteUsbReleaseCallback(RemoteUsbReleaseCallback cb) {
        m_RemoteUsbReleaseCallback = std::move(cb);
    }
    void setBitrateChangeCallback(BitrateChangeCallback cb) {
        m_BitrateChangeCallback = std::move(cb);
    }

    // Human-readable bitrate label shared with toast/log formatting.
    static QString formatBitrateKbps(int kbps);

    // Position the panel at the right edge of the given Qt logical parent rect.
    void showAtRightEdge(int parentX, int parentY, int parentW, int parentH,
                         std::optional<QPoint> pointerGlobalPosition = std::nullopt,
                         bool closeWhenPointerOutside = true);

    // Position the panel at the left edge of the given Qt logical parent rect.
    void showAtLeftEdge(int parentX, int parentY, int parentW, int parentH,
                        std::optional<QPoint> pointerGlobalPosition = std::nullopt,
                        bool closeWhenPointerOutside = true);

    // Position the panel at the top edge of the given Qt logical parent rect.
    void showAtTopEdge(int parentX, int parentY, int parentW, int parentH,
                       std::optional<QPoint> pointerGlobalPosition = std::nullopt,
                       bool closeWhenPointerOutside = true);

    // Position the panel at a specific Qt global logical position.
    void showAtCursor(int parentX, int parentY, int parentW, int parentH,
                      const QPoint& cursorPosition, bool pointerTriggered = true);

    void closeMenu();
    void dismissOnOutsideClick(const QPoint& globalPosition);
    bool isMenuVisible() const { return m_Visible; }
    bool isClosing() const { return m_Closing; }
    bool needsEventProcessing() const { return m_Visible || m_Closing; }

    // Update dynamic state before showing the menu
    void updateMicrophoneState(bool enabled);
    void updateBitrateState(int bitrateKbps);
    void updateMenuPositionState(MenuAction activePlacementAction);
    void updateGamepadMouseState(bool enabled);
    void updateFileMappingState(FileMappingState state, const QString& detail);
    void updateRemoteUsbState(bool available,
                              RemoteUsbState state,
                              std::vector<RemoteUsbDevice> devices,
                              const QString& activeDeviceId,
                              const QString& detail);
    void setHasGamepads(bool has) {
        if (m_HasGamepads != has) {
            m_HasGamepads = has;
            buildMenuLevels();  // rebuild to show/hide gamepad items
        }
    }

    // Gamepad navigation
    void gamepadMoveUp();
    void gamepadMoveDown();
    void gamepadSelect();
    void gamepadBack();
    // Step the focused slider row (DPAD left/right); direction is -1 or +1.
    // Rapid consecutive presses accelerate the step size.
    void gamepadAdjustSlider(int direction);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
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
        QString      payload;      // opaque action payload; never rendered

        MenuItem(QString label,
                 QString detail,
                 MenuItemType type,
                 MenuAction action,
                 int targetLevel,
                 bool enabled,
                 bool toggleState,
                 bool separatorAfter,
                 QString payload = {})
            : label(std::move(label)),
              detail(std::move(detail)),
              type(type),
              action(action),
              targetLevel(targetLevel),
              enabled(enabled),
              toggleState(toggleState),
              separatorAfter(separatorAfter),
              payload(std::move(payload))
        {
        }
    };

    struct MenuLevel {
        QString              title;
        std::vector<MenuItem> items;
    };

    enum class AnchorMode { RightEdge, LeftEdge, TopEdge, AtCursor };

    // Hit zones inside the slider row (local coordinates).
    enum class SliderZone { None, Minus, Track, Plus };

    struct SliderRowRects {
        QRect value;
        QRect track;
        QRect minus;
        QRect plus;
    };

    void buildMenuLevels();
    void navigateToLevel(int level);
    void repositionWindow();
    void showInternal();     // shared show logic after geometry is set
    void schedulePointerOutsideCheck();
    void beginInteraction();
    void forceRepaint();     // synchronous repaint (requestUpdate is async on Windows)
    int  itemAtPos(const QPoint& pos) const;
    void dispatchActionItem(const MenuItem& item);

    // --- Bitrate slider row ---
    SliderRowRects sliderRowRects(int contentWidth, int itemY) const;
    SliderZone sliderZoneAt(const QPoint& localPos, int rowIdx) const;
    double bitrateFraction() const;                 // m_BitrateKbps on the log scale, 0..1
    void setBitrateFromFraction(double fraction);   // inverse of bitrateFraction()
    void setBitrateKbps(int bitrateKbps);           // clamp + refresh + schedule commit
    void adjustBitrateStep(int direction, int multiplier);
    void selectBitratePreset(const MenuItem& item); // snap to preset + commit now
    void commitBitrateNow();                        // flush pending change to callback
    void refreshBitrateDetails();                   // level-0 detail + preset checkmarks

    std::vector<MenuLevel> m_MenuLevels;
    int  m_CurrentLevel;
    int  m_HoveredIndex;
    bool m_Visible;
    bool m_HasGamepads;
    FileMappingState m_FileMappingState;
    QString m_FileMappingDetail;
    bool m_RemoteUsbAvailable;
    RemoteUsbState m_RemoteUsbState;
    std::vector<RemoteUsbDevice> m_RemoteUsbDevices;
    QString m_RemoteUsbActiveDeviceId;
    QString m_RemoteUsbDetail;

    ActionCallback m_ActionCallback;
    CloseCallback  m_CloseCallback;
    RemoteUsbDeviceCallback m_RemoteUsbDeviceCallback;
    RemoteUsbReleaseCallback m_RemoteUsbReleaseCallback;
    BitrateChangeCallback m_BitrateChangeCallback;

    // Bitrate slider state. m_BitrateKbps is the on-screen value;
    // m_CommittedBitrateKbps is the last value sent to the callback.
    int m_BitrateKbps = 10000;
    int m_CommittedBitrateKbps = 10000;
    QTimer m_BitrateCommitTimer;      // debounces callback while scrubbing
    bool m_SliderDragging = false;    // pointer is scrubbing the track
    SliderZone m_SliderPressedZone = SliderZone::None;
    SliderZone m_SliderHotZone = SliderZone::None;
    qreal m_WheelAccum = 0.0;         // high-resolution wheel accumulation
    QElapsedTimer m_SliderAdjustClock; // gamepad repeat acceleration window
    int m_SliderAdjustStreak = 0;

    // Parent window rect in Qt global logical coordinates for level changes
    int m_ParentX, m_ParentY, m_ParentW, m_ParentH;

    // Layout constants (logical units, Qt 6 auto-scales)
    int m_ItemHeight;
    int m_Padding;
    int m_MenuWidth;
    int m_ShadowMargin;
    int m_TitleHeight;
    int m_IconAreaWidth;

    // Fonts
    QFont m_LabelFont;
    QFont m_DetailFont;
    QFont m_TitleFont;
    QHash<QString, QIcon> m_MenuIcons;

    // Anti-flicker: grace period after show
    QElapsedTimer m_ShowTimer;
    QTimer m_LeaveTimer;
    bool m_CloseWhenPointerOutside;

    // Animations
    QPropertyAnimation* m_OpacityAnim;
    QPropertyAnimation* m_SlideAnim;    // animates x or y based on the anchor
    QVariantAnimation*  m_ContentSlideAnim; // animates content offset for level nav
    qreal  m_ContentOffset;   // horizontal paint offset during level transition
    bool   m_Closing;         // true while close animation is running
    QPoint m_TargetPosition;  // cached final position for show animation

    // Menu anchor mode and pointer position in Qt global logical coordinates.
    AnchorMode m_AnchorMode;
    std::optional<QPoint> m_TriggerPosition;
};
