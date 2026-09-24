#pragma once

#include <QTimer>
#include <QEvent>
#include <QString>

#include "SDL_compat.h"

#include "streaming/input/gamepadglyphs.h"

#include "settings/streamingpreferences.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT

public:
    SdlGamepadKeyNavigation(StreamingPreferences* prefs);

    ~SdlGamepadKeyNavigation();

    Q_INVOKABLE void enable();

    Q_INVOKABLE void disable();

    Q_INVOKABLE void notifyWindowFocus(bool hasFocus);

    Q_INVOKABLE void setUiNavMode(bool settingsMode);

    // 临时挂起 UI 导航模式（下拉展开这类场景要拿回真正的方向键）。
    // 用计数而不是存旧值：挂起和恢复的配对由调用方保证，但和页面切换时的
    // setUiNavMode 谁先谁后不确定，存旧值会把页面刚设好的模式覆盖回去。
    Q_INVOKABLE void suspendUiNavMode();

    Q_INVOKABLE void resumeUiNavMode();

    Q_INVOKABLE int getConnectedGamepads();

    // NO_GAMEPAD_QUIT=1 时游戏柄退出组合键被禁用,提示文案据此回落
    Q_INVOKABLE bool gamepadQuitComboEnabled() const;

    // 手柄 UI 风格（按键提示文案用）：按当前连接的第一只手柄自动识别，
    // 未连接或识别不出时按 Xbox 布局处理。枚举与 glyph 函数定义见
    // streaming/input/gamepadglyphs.h（串流侧悬浮菜单/toast 共用）。
    Q_INVOKABLE int gamepadUiStyle();

    // 面键在当前风格下的显示名。logicalButton 为 SDL 位置语义：0=下(A)
    // 1=右(B) 2=左(X) 3=上(Y)；PS 显示 ✕/○/□/△，任天堂按其标签布局显示
    Q_INVOKABLE QString faceButtonGlyph(int logicalButton);

    Q_INVOKABLE QString startButtonName();

    Q_INVOKABLE QString selectButtonName();

    Q_INVOKABLE QString leftShoulderName();

    Q_INVOKABLE QString rightShoulderName();

private:
    // 实际生效的模式：页面要求开启，且当前没有被挂起
    bool uiNavModeActive() const;

    void sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    void updateTimerState();

    // 方向统一映射：普通界面发方向键，设置页 (uiNav) 上下退化为 Tab / Shift+Tab
    void sendDirectionKey(QEvent::Type type, int dir);

    // 摇杆/D-pad 连发节奏：起步慢，持续按住逐渐加速
    static Uint32 axisNavRepeatDelayMs(Uint32 heldMs);

    // 失焦、禁用或首轮 flush 时清空按住状态
    void resetNavRepeatState();

    GamepadUiStyle detectUiStyle() const;

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<SDL_GameController*> m_Gamepads;
    bool m_Enabled;
    bool m_UiNavMode;
    int m_UiNavSuspendCount;
    bool m_FirstPoll;
    bool m_HasFocus;

    // 摇杆导航的当前方向与计时；阈值滞回防抖，重复随按住时长加速
    enum AxisNavDir
    {
        AxisNavNone,
        AxisNavUp,
        AxisNavDown,
        AxisNavLeft,
        AxisNavRight,
    };
    int m_AxisNavDir = AxisNavNone;
    Uint32 m_AxisNavDirSince = 0;
    Uint32 m_AxisNavLastFire = 0;

    // D-pad 连发状态（下标即 AxisNavDir 值，None 不用）
    struct DpadNavState
    {
        bool held = false;
        Uint32 downSince = 0;
        Uint32 lastFire = 0;
    };
    DpadNavState m_DpadNav[5];
};
