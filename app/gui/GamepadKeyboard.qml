import QtQuick 2.9
import QtQuick.Controls
import QtQuick.Layouts

import SdlGamepadKeyNavigation 1.0

import "."
import "theme"

// 手柄屏幕键盘。纯手柄用户在文本框聚焦时按 X(Key_Menu)呼出:方向键/
// D-pad 在键位间移动,选键按 A 上屏,⌫ 删除,OK 或 Escape 把缓冲写回
// 目标输入框并关闭(关闭不丢字)。缓冲本身是 TextField,鼠标和物理键盘
// 也可以直接点按/输入。
NavigableDialog {
    id: osk

    property TextField targetField: null

    // 键位表。最后一列去掉容易误触的引号斜杠,保留 IP/主机名常用的符号
    property var _rows: [
        ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
        ["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
        ["a", "s", "d", "f", "g", "h", "j", "k", "l", "-"],
        ["z", "x", "c", "v", "b", "n", "m", ".", "_", ":"]
    ]

    title: qsTr("On-Screen Keyboard")
    standardButtons: Dialog.NoButton

    function openFor(field) {
        targetField = field
        bufferField.text = field.text
        if (!visible) {
            // 设置页等 ui-nav 激活的场景下,网格需要真实方向键而不是被
            // 翻译成 Tab/Shift+Tab;关闭时按计数归还
            SdlGamepadKeyNavigation.suspendUiNavMode()
        }
        open()
        // 落在 QWERTY 第二行正中,往哪个方向都有键位
        _moveFocus(1, 4)
    }

    function commitKey(key) {
        bufferField.text += key
    }

    function backspace() {
        var t = bufferField.text
        if (t.length > 0) {
            bufferField.text = t.substring(0, t.length - 1)
        }
    }

    function applyAndClose() {
        if (targetField) {
            targetField.text = bufferField.text
        }
        close()
    }

    function _keyItem(row, col) {
        if (row < 0 || row >= _rows.length) {
            return null
        }
        var gridRow = keyColumn.children[row]
        if (!gridRow) {
            return null
        }
        // Row.children 里混着 Repeater 本体(它会把委托排在自己前面),
        // 按 objectName 过滤出真实键位,不能直接用 children 下标
        var n = 0
        for (var i = 0; i < gridRow.children.length; i++) {
            var item = gridRow.children[i]
            if (item.objectName !== "oskKey") {
                continue
            }
            if (n === col) {
                return item
            }
            n++
        }
        return null
    }

    function _moveFocus(row, col) {
        var item = _keyItem(row, col)
        if (item) {
            item.forceActiveFocus()
        }
    }

    onClosed: {
        SdlGamepadKeyNavigation.resumeUiNavMode()
        // Escape/B 关闭同样保留已输入文本,焦点还给来源输入框
        if (targetField) {
            targetField.text = bufferField.text
            targetField.forceActiveFocus()
        }
    }

    ColumnLayout {
        spacing: Theme.spaceSm

        HardTextField {
            id: bufferField

            Layout.fillWidth: true
            font.pointSize: Theme.fontRowTitle
        }

        Column {
            id: keyColumn

            spacing: Theme.spaceXs

            Repeater {
                model: osk._rows

                Row {
                    required property int index

                    readonly property int row: index

                    spacing: Theme.spaceXs

                    Repeater {
                        model: osk._rows[parent.row]

                        HardButton {
                            required property int index
                            required property var modelData

                            readonly property int row: parent.row
                            readonly property int col: index

                            objectName: "oskKey"
                            text: modelData
                            implicitWidth: 30
                            implicitHeight: 30
                            font.pointSize: Theme.fontBody

                            onClicked: osk.commitKey(modelData)
                            Keys.onLeftPressed: osk._moveFocus(row, col - 1)
                            Keys.onRightPressed: osk._moveFocus(row, col + 1)
                            Keys.onUpPressed: osk._moveFocus(row - 1, col)
                            Keys.onDownPressed: {
                                if (row < osk._rows.length - 1) {
                                    osk._moveFocus(row + 1, col)
                                }
                                else {
                                    spaceButton.forceActiveFocus()
                                }
                            }
                            Keys.onReturnPressed: osk.commitKey(modelData)
                            Keys.onEnterPressed: osk.commitKey(modelData)
                        }
                    }
                }
            }

            Row {
                spacing: Theme.spaceXs

                HardButton {
                    id: spaceButton

                    text: qsTr("Space")
                    implicitWidth: 96
                    font.pointSize: Theme.fontBody

                    onClicked: osk.commitKey(" ")
                    Keys.onLeftPressed: osk._moveFocus(osk._rows.length - 1, osk._rows[osk._rows.length - 1].length - 1)
                    Keys.onRightPressed: backspaceButton.forceActiveFocus()
                    Keys.onUpPressed: osk._moveFocus(osk._rows.length - 1, 0)
                    Keys.onReturnPressed: osk.commitKey(" ")
                    Keys.onEnterPressed: osk.commitKey(" ")
                }

                HardButton {
                    id: backspaceButton

                    text: qsTr("Backspace")
                    implicitWidth: 96
                    font.pointSize: Theme.fontBody

                    onClicked: osk.backspace()
                    Keys.onLeftPressed: spaceButton.forceActiveFocus()
                    Keys.onRightPressed: okButton.forceActiveFocus()
                    Keys.onUpPressed: osk._moveFocus(osk._rows.length - 1, 4)
                    Keys.onReturnPressed: osk.backspace()
                    Keys.onEnterPressed: osk.backspace()
                }

                HardButton {
                    id: okButton

                    text: qsTr("OK")
                    primary: true
                    font.pointSize: Theme.fontBody

                    onClicked: osk.applyAndClose()
                    Keys.onLeftPressed: backspaceButton.forceActiveFocus()
                    Keys.onUpPressed: osk._moveFocus(osk._rows.length - 1, 5)
                    Keys.onReturnPressed: osk.applyAndClose()
                    Keys.onEnterPressed: osk.applyAndClose()
                }
            }
        }
    }
}
