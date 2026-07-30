import QtQuick 2.9
import QtQuick.Controls 2.2
import "."
import "../theme"

// 开关行。checked 不做双向绑定，由使用方在 onToggled 里写回偏好设置，
// 避免初始化阶段的写回把已保存的值覆盖掉。
SettingsRow {
    id: toggleRow

    property alias checked: control.checked
    property alias controlEnabled: control.enabled
    property alias tooltip: tip.text

    signal toggled(bool value)

    HardSwitch {
        id: control
        hoverEnabled: true
        onCheckedChanged: toggleRow.toggled(checked)

        ToolTip {
            id: tip
            visible: text !== "" && control.hovered
            delay: 800
            timeout: 8000
        }
    }
}
