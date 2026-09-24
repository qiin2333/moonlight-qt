import QtQuick 2.9
import QtQuick.Controls

import SdlGamepadKeyNavigation 1.0

import "."

// 方角输入框。FluentWinUI3 的 TextField 背景是圆角 + 底部一条粗下划线，
// 换成 1px 方框，聚焦时描边转 accent。
//
// 里面填的基本都是数字和 IP，所以正文走等宽字体 —— 等宽 + tabular-nums 是这套风格
// 表达「这是数据」的方式，边输边跳的比例字宽在这里读起来很脏。
TextField {
    id: control

    color: Theme.text
    placeholderTextColor: Theme.textFaint
    selectByMouse: true

    font.family: Theme.fontMono
    font.pointSize: Theme.fontBody

    leftPadding: Theme.spaceSm
    rightPadding: Theme.spaceSm
    topPadding: Theme.spaceXs
    bottomPadding: Theme.spaceXs

    // 手柄用户:X(Key_Menu)呼出屏幕键盘(实例挂在 main.qml,所有文本框
    // 自动获得该能力)。没接手柄时不拦截按键,保留 Qt 自带的文本编辑菜单。
    Keys.onMenuPressed: function(event) {
        if (typeof window !== "undefined" && window.gamepadOsk !== null &&
                SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            window.gamepadOsk.openFor(control)
        }
        else {
            event.accepted = false
        }
    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 32

        radius: 0
        color: Theme.ink
        // 输入框的焦点就是「光标在这里」，所以看 activeFocus 而不是 visualFocus。
        // 粗细/颜色的规矩和其他控件一致：focus 2px accent，hover 1px lineStrong。
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.accent
                                          : (control.hovered ? Theme.lineStrong : Theme.line)

        Behavior on border.color {
            ColorAnimation { duration: Theme.durFast }
        }
    }
}
