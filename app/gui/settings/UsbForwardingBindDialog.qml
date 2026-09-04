pragma ComponentBehavior: Bound
import QtQuick 2.9
import QtQuick.Controls
import QtQuick.Layouts 1.3
import ".."
import "../theme"
import UsbForwardingBackend 1.0

// USB 设备共享管理对话框。共享/停止共享会触发一次 UAC（usbipd bind/unbind 写
// HKLM 注册表，需要管理员；每个设备一次）。bind 是持久化的：重启后依然共享；
// 串流中是否真正转发，仍由悬浮菜单逐台确认。
NavigableDialog {
    id: dialog

    title: qsTr("Share USB devices")
    closePolicy: Popup.CloseOnEscape
    width: Math.max(360, Math.min(600, Overlay.overlay.width - Theme.spaceLg * 2))
    standardButtons: Dialog.Close

    property string statusText: ""
    property bool statusIsError: false

    onOpened: {
        statusText = ""
        UsbForwardingBackend.refresh()
    }

    Connections {
        target: UsbForwardingBackend
        function onOperationFinished(success, message) {
            dialog.statusText = message
            dialog.statusIsError = !success
        }
    }

    function deviceStatusText(d) {
        if (!d.isConnected) {
            return d.isBound ? qsTr("Not connected") : ""
        }
        if (!d.isSupported) {
            return qsTr("Unsupported")
        }
        if (d.isAttached) {
            return qsTr("In use")
        }
        return d.isBound ? qsTr("Shared") : qsTr("Not shared")
    }

    function deviceStatusColor(d) {
        if (!d.isConnected || !d.isSupported) {
            return Theme.textFaint
        }
        if (d.isAttached) {
            return Theme.acid
        }
        return d.isBound ? Theme.accent : Theme.textFaint
    }

    ColumnLayout {
        width: parent ? parent.width : 0
        spacing: Theme.spaceMd

        // 接管语义警告
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: warningLayout.implicitHeight + Theme.spaceMd * 2
            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.10)
            border.width: 1
            border.color: Theme.danger

            ColumnLayout {
                id: warningLayout
                width: parent ? parent.width - Theme.spaceLg * 2 : 0
                x: Theme.spaceLg
                y: Theme.spaceMd
                spacing: 2

                Text {
                    Layout.fillWidth: true
                    text: qsTr("A shared device is taken over by the forwarding service and becomes unavailable on this computer.")
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pointSize: Theme.fontBody
                    font.weight: Font.Medium
                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Stop sharing to restore it. Sharing needs administrator confirmation, once per device.")
                    color: Theme.textDim
                    font.family: Theme.fontSans
                    font.pointSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }
        }

        // 状态反馈行（bind/unbind 结果）
        Text {
            Layout.fillWidth: true
            visible: dialog.statusText !== ""
            text: dialog.statusText
            color: dialog.statusIsError ? Theme.danger : Theme.accent
            font.family: Theme.fontSans
            font.pointSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }

        // 错误 / 空态
        Text {
            Layout.fillWidth: true
            visible: UsbForwardingBackend.error !== ""
            text: UsbForwardingBackend.error
            color: Theme.danger
            font.family: Theme.fontSans
            font.pointSize: Theme.fontBody
            wrapMode: Text.Wrap
        }

        Text {
            Layout.fillWidth: true
            visible: UsbForwardingBackend.error === ""
                     && UsbForwardingBackend.devices.length === 0
                     && !UsbForwardingBackend.busy
            text: qsTr("No USB devices found.")
            color: Theme.textDim
            font.family: Theme.fontSans
            font.pointSize: Theme.fontBody
        }

        // 设备列表
        ScrollView {
            id: deviceScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(300, deviceColumn.implicitHeight + 2)
            visible: UsbForwardingBackend.devices.length > 0
            clip: true

            Column {
                id: deviceColumn
                width: deviceScroll.availableWidth

                Repeater {
                    model: UsbForwardingBackend.devices

                    Rectangle {
                        id: row
                        required property int index
                        required property var modelData

                        width: deviceColumn.width
                        height: rowLayout.implicitHeight + Theme.spaceMd * 2
                        color: "transparent"

                        Rectangle {
                            anchors {
                                left: parent.left
                                right: parent.right
                                bottom: parent.bottom
                            }
                            height: 1
                            color: Theme.line
                            visible: row.index < UsbForwardingBackend.devices.length - 1
                        }

                        RowLayout {
                            id: rowLayout
                            anchors.fill: parent
                            anchors.margins: Theme.spaceMd
                            spacing: Theme.spaceMd

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1

                                Text {
                                    Layout.fillWidth: true
                                    text: row.modelData.description !== ""
                                          ? row.modelData.description
                                          : qsTr("USB device")
                                    color: Theme.text
                                    font.family: Theme.fontSans
                                    font.pointSize: Theme.fontBody
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: row.modelData.vidPid !== ""
                                    text: row.modelData.vidPid.toUpperCase()
                                    color: Theme.textFaint
                                    font.family: Theme.fontSans
                                    font.pointSize: Theme.fontCaption
                                }
                            }

                            Text {
                                Layout.alignment: Qt.AlignVCenter
                                text: dialog.deviceStatusText(row.modelData)
                                color: dialog.deviceStatusColor(row.modelData)
                                font.family: Theme.fontSans
                                font.pointSize: Theme.fontCaption
                                font.weight: Font.SemiBold
                            }

                            HardButton {
                                Layout.alignment: Qt.AlignVCenter
                                text: row.modelData.isBound
                                      ? qsTr("Stop sharing")
                                      : qsTr("Share")
                                visible: row.modelData.isBound
                                         || (row.modelData.isConnected && row.modelData.isSupported)
                                enabled: !UsbForwardingBackend.busy
                                primary: !row.modelData.isBound
                                onClicked: {
                                    dialog.statusText = ""
                                    if (row.modelData.isBound) {
                                        UsbForwardingBackend.unbind(
                                            row.modelData.busId,
                                            row.modelData.persistedGuid)
                                    } else {
                                        UsbForwardingBackend.bind(row.modelData.busId)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
