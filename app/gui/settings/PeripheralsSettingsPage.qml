pragma ComponentBehavior: Bound
import QtQuick 2.9
import QtQuick.Controls
import "."
import "../theme"
import StreamingPreferences 1.0
import SystemProperties 1.0
import UsbForwardingEnvironment 1.0
import UsbForwardingBackend 1.0

// 「外设」——本机物理外设与串流主机之间的设备级能力。
// 与「输入设备」「手柄」分类的区别：那两页配置的是输入如何映射到串流，
// 这一页管理的是把真实 USB 设备交给主机直接使用（USB 设备转发）。
Column {
    id: peripheralsPage

    width: parent ? parent.width : 0
    spacing: Theme.spaceLg

    SettingsCard {
        id: usbForwardingCard
        title: qsTr("USB Device Forwarding")
        subtitle: qsTr("Share local gamepads and other peripherals with the streaming host. Forwarding is confirmed per device during a stream and can be stopped at any time.")
        visible: SystemProperties.remoteUsbAvailable

        readonly property string usbipdUrl: "https://github.com/dorssel/usbipd-win/releases/latest"

        Component.onCompleted: {
            if (visible) {
                UsbForwardingEnvironment.refresh()
            }
        }

        ToggleRow {
            title: qsTr("Enable USB device forwarding")
            description: qsTr("When off, streaming will not discover or start any USB forwarding service.")
            checked: StreamingPreferences.usbForwardingEnabled
            onToggled: function(value) { StreamingPreferences.usbForwardingEnabled = value }
        }

        SettingsRow {
            id: usbEnvRow

            title: "usbipd-win"
            description: {
                if (UsbForwardingEnvironment.checking) {
                    return qsTr("Checking environment…")
                }
                switch (UsbForwardingEnvironment.state) {
                case UsbForwardingEnvironment.Ready:
                    return qsTr("v%1 · Service running")
                        .arg(UsbForwardingEnvironment.usbipdVersion)
                case UsbForwardingEnvironment.ServiceStopped:
                    return qsTr("Installed (v%1). The usbipd service is not running.")
                        .arg(UsbForwardingEnvironment.usbipdVersion)
                case UsbForwardingEnvironment.NotInstalled:
                default:
                    return qsTr("Not installed. usbipd-win is required to share USB devices.")
                }
            }
            descriptionFontPointSize: Theme.fontSettingsSubtitle + 1

            Flow {
                width: Math.min(260, Math.max(0, usbEnvRow.width - Theme.spaceMd * 2))
                spacing: Theme.spaceSm

                HardButton {
                    text: qsTr("Refresh")
                    onClicked: UsbForwardingEnvironment.refresh()
                }

                HardLink {
                    text: qsTr("Install / Repair")
                    onClicked: peripheralsPage.openExternal(usbForwardingCard.usbipdUrl)
                }
            }
        }

        SettingsRow {
            title: qsTr("Shared devices")
            description: qsTr("Choose which devices can be forwarded to the streaming host. Sharing takes over the device on this computer.")

            Flow {
                width: Math.min(260, Math.max(0, usbEnvRow.width - Theme.spaceMd * 2))
                spacing: Theme.spaceSm

                HardButton {
                    text: qsTr("Manage devices…")
                    primary: true
                    onClicked: bindDialog.open()
                }
            }
        }

        SettingsRow {
            title: qsTr("Voice input")
            description: qsTr("Microphone audio does not travel through USB forwarding. Use the in-stream microphone feature for voice.")
        }
    }

    UsbForwardingBindDialog {
        id: bindDialog
    }

    function openExternal(url) {
        if (!Qt.openUrlExternally(url)) {
            ToolTip.show(qsTr("No external browser is available."), 3500)
        }
    }
}
