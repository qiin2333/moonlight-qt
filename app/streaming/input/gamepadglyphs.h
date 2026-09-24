#pragma once

#include <QString>

// 提示文案里的按键名跟随手柄实体布局：Xbox 布局为缺省，PS/Switch 实机
// 自动识别。GUI 导航（QML）与串流侧（悬浮菜单/toast）共用同一套定义。
// 注意保持零协议依赖（tests/overlay_menu_navigation 会独立编译包含本头
// 文件的 overlaymenupanel.cpp，其 include 路径里没有 moonlight-common-c），
// LI_CTYPE_* 的映射请放在使用方。

enum GamepadUiStyle
{
    GamepadUiStyleXbox = 0,
    GamepadUiStylePlayStation = 1,
    GamepadUiStyleNintendo = 2,
};

// 面键显示名。logicalButton 为 SDL 位置语义：0=下(A) 1=右(B) 2=左(X)
// 3=上(Y)。任天堂按其标签布局显示（物理下键就叫 B），PS 用符号。
inline QString gamepadFaceButtonGlyph(GamepadUiStyle style, int logicalButton)
{
    switch (style) {
    case GamepadUiStylePlayStation:
        switch (logicalButton) {
        case 0:
            return QStringLiteral("✕");
        case 1:
            return QStringLiteral("○");
        case 2:
            return QStringLiteral("□");
        case 3:
            return QStringLiteral("△");
        }
        break;
    case GamepadUiStyleNintendo:
        switch (logicalButton) {
        case 0:
            return QStringLiteral("B");
        case 1:
            return QStringLiteral("A");
        case 2:
            return QStringLiteral("Y");
        case 3:
            return QStringLiteral("X");
        }
        break;
    default:
        break;
    }

    switch (logicalButton) {
    case 0:
        return QStringLiteral("A");
    case 1:
        return QStringLiteral("B");
    case 2:
        return QStringLiteral("X");
    case 3:
        return QStringLiteral("Y");
    }
    return QString();
}

inline QString gamepadStartButtonName(GamepadUiStyle style)
{
    switch (style) {
    case GamepadUiStylePlayStation:
        return QStringLiteral("Options");
    case GamepadUiStyleNintendo:
        return QStringLiteral("+");
    default:
        return QStringLiteral("Start");
    }
}

inline QString gamepadSelectButtonName(GamepadUiStyle style)
{
    switch (style) {
    case GamepadUiStylePlayStation:
        return QStringLiteral("Share");
    case GamepadUiStyleNintendo:
        return QStringLiteral("−");
    default:
        return QStringLiteral("Select");
    }
}

inline QString gamepadLeftShoulderName(GamepadUiStyle style)
{
    switch (style) {
    case GamepadUiStylePlayStation:
        return QStringLiteral("L1");
    case GamepadUiStyleNintendo:
        return QStringLiteral("L");
    default:
        return QStringLiteral("LB");
    }
}

inline QString gamepadRightShoulderName(GamepadUiStyle style)
{
    switch (style) {
    case GamepadUiStylePlayStation:
        return QStringLiteral("R1");
    case GamepadUiStyleNintendo:
        return QStringLiteral("R");
    default:
        return QStringLiteral("RB");
    }
}
