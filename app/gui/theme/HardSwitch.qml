import QtQuick 2.9
import QtQuick.Controls
import "."

// 方角开关：方轨 + 方滑块，滑块位移不带回弹。
Switch {
    id: control

    font.family: Theme.fontSans
    implicitWidth: 44
    implicitHeight: 22
    padding: 0
    spacing: 0

    indicator: Rectangle {
        id: track
        implicitWidth: 44
        implicitHeight: 22
        x: control.text ? control.leftPadding
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2

        radius: 0
        color: control.checked ? Theme.accent : Theme.surface2
        border.width: 1
        border.color: !control.enabled ? Theme.line
                    : control.checked ? Theme.accent
                    : (control.hovered || control.visualFocus ? Theme.accent : Theme.lineStrong)
        opacity: control.enabled ? 1.0 : 0.45

        Behavior on color {
            ColorAnimation { duration: Theme.durFast }
        }

        Rectangle {
            id: handle
            y: 3
            x: 3 + (pointerArea.dragging ? pointerArea.dragPosition
                                         : control.visualPosition) * (parent.width - width - 6)
            width: 16
            height: parent.height - 6
            radius: 0
            color: control.checked ? Theme.ink : Theme.textDim

            Behavior on x {
                enabled: !pointerArea.pressed && !control.down
                NumberAnimation { duration: Theme.durFast; easing.type: Theme.easing }
            }
        }

        // Own the pointer gesture on the painted track. This avoids the
        // FluentWinUI3 Switch regression where a stationary release is ignored,
        // while retaining both tap-to-toggle and drag-to-select behavior.
        //
        // 提交状态一律走 click()，不能用 toggle()：toggle() 只是 setChecked(!checked)，
        // 发的仅有 checkedChanged，不发 toggled()。而 ToggleRow 正是靠 toggled() 把值
        // 写回偏好设置的（checked 是绑定，初始化阶段也会变，所以不能用 checkedChanged）。
        // 用 toggle() 的结果就是：开关看着拨过去了，设置根本没保存。
        MouseArea {
            id: pointerArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.PointingHandCursor

            property real pressStartX: 0
            property real dragPosition: control.visualPosition
            property real dragOffset: 0
            property bool dragging: false

            onPressed: function(mouse) {
                pressStartX = mouse.x
                dragPosition = control.visualPosition
                var travel = width - handle.width - 6
                var handleCenter = 3 + handle.width / 2 + dragPosition * travel
                dragOffset = mouse.x - handleCenter
                dragging = false
                control.forceActiveFocus(Qt.MouseFocusReason)
            }
            onPositionChanged: function(mouse) {
                if (!pressed) {
                    return
                }
                if (Math.abs(mouse.x - pressStartX) >= 4) {
                    dragging = true
                }
                if (dragging) {
                    var travel = width - handle.width - 6
                    dragPosition = Math.max(0, Math.min(1,
                        (mouse.x - dragOffset - 3 - handle.width / 2) / travel))
                }
            }
            onReleased: function() {
                if (dragging) {
                    // 拖到哪一侧就是哪一侧：只有真的换了边才提交，
                    // 拖回原位松手等于什么都没做。
                    var targetChecked = dragPosition >= 0.5
                    if (targetChecked !== control.checked) {
                        control.click()
                    }
                } else {
                    control.click()
                }
                dragging = false
            }
            onCanceled: dragging = false
        }
    }
}
