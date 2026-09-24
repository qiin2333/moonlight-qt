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

    // Windows 走外挂 usbipd-win；macOS 走捆绑的 moonlight-usbd（usbipdcpp）；
    // Linux 走标准 usbip-host 栈（usbip 工具 + root usbipd 守护进程）。
    readonly property bool isMac: SystemProperties.isDarwin
    readonly property bool isLinux: SystemProperties.isLinux

    SettingsCard {
        id: usbForwardingCard
        title: qsTr("USB Device Forwarding")
        subtitle: qsTr("Share local gamepads and other peripherals with the streaming host. Forwarding is confirmed per device during a stream and can be stopped at any time.")
        visible: SystemProperties.usbForwardingAvailable

        readonly property string usbipdUrl: "https://github.com/dorssel/usbipd-win/releases/latest"

        Component.onCompleted: {
            if (visible) {
                UsbForwardingEnvironment.refresh()
            }
        }
        onVisibleChanged: if (visible) UsbForwardingEnvironment.refresh()

        ToggleRow {
            title: qsTr("Enable USB device forwarding")
            description: qsTr("When off, streaming will not discover or start any USB forwarding service.")
            checked: StreamingPreferences.usbForwardingEnabled
            onToggled: function(value) { StreamingPreferences.usbForwardingEnabled = value }
        }

        SettingsRow {
            id: usbEnvRow

            title: !peripheralsPage.isMac && !peripheralsPage.isLinux
                   ? "usbipd-win" : qsTr("USB sharing service")
            description: {
                if (UsbForwardingEnvironment.checking) {
                    return qsTr("Checking environment…")
                }
                switch (UsbForwardingEnvironment.state) {
                case UsbForwardingEnvironment.Checking:
                    return qsTr("Checking environment…")
                case UsbForwardingEnvironment.DriverStopped:
                    return peripheralsPage.isLinux
                        ? qsTr("The USB/IP kernel module is not loaded. Moonlight loads it automatically when you share a device.")
                        : qsTr("USB driver not running. Start VBoxUSBMon as administrator, or restart Windows.")
                case UsbForwardingEnvironment.CheckFailed:
                    if (peripheralsPage.isMac) {
                        return qsTr("Could not verify the bundled USB sharing service. Reinstall Moonlight.")
                    }
                    return peripheralsPage.isLinux
                        ? qsTr("Could not verify the USB sharing environment. Check the usbip installation.")
                        : qsTr("Could not verify the USB service and driver. Check the usbipd-win installation.")
                case UsbForwardingEnvironment.Ready:
                    if (peripheralsPage.isMac || peripheralsPage.isLinux) {
                        return qsTr("%1 · Ready").arg(UsbForwardingEnvironment.usbipdVersion)
                    }
                    return qsTr("v%1 · Service running")
                          .arg(UsbForwardingEnvironment.usbipdVersion)
                case UsbForwardingEnvironment.ServiceStopped:
                    if (peripheralsPage.isLinux) {
                        return qsTr("Installed (v%1). The usbipd daemon is not running; Moonlight starts it when you share a device.")
                            .arg(UsbForwardingEnvironment.usbipdVersion)
                    }
                    return qsTr("Installed (v%1). The usbipd service is not running.")
                        .arg(UsbForwardingEnvironment.usbipdVersion)
                case UsbForwardingEnvironment.NotInstalled:
                default:
                    if (peripheralsPage.isMac) {
                        return qsTr("The bundled USB sharing service is missing. Reinstall Moonlight.")
                    }
                    return peripheralsPage.isLinux
                        ? qsTr("Not installed. The usbip tool is required to share USB devices. Install the USB/IP package of your distribution (usbip-utils or linux-tools).")
                        : qsTr("Not installed. usbipd-win is required to share USB devices.")
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
                    visible: !peripheralsPage.isMac && !peripheralsPage.isLinux
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

        // macOS 平台说明：被系统驱动占用的设备（HID/存储/摄像头）无法共享。
        SettingsRow {
            visible: peripheralsPage.isMac
            title: qsTr("Device availability")
            description: qsTr("Devices managed by macOS itself — keyboards, mice, storage, and cameras — are shown as \"In use by macOS\" and cannot be shared without administrator access.")
        }

        // Linux 平台说明：共享会接管设备，本机暂时不可用；共享需要管理员确认。
        SettingsRow {
            visible: peripheralsPage.isLinux
            title: qsTr("Device availability")
            description: qsTr("While a device is shared, it is taken over by the USB/IP driver and stops working on this computer until you release it. The first share asks for administrator confirmation; the authorization is remembered for a few minutes.")
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
