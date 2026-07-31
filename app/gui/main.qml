import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import AutoUpdateChecker 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0

import "theme"

ApplicationWindow {
    property bool pollingActive: false

    // Set by SettingsView to force the back operation to pop all
    // pages except the initial view. This is required when doing
    // a retranslate() because AppView breaks for some reason.
    property bool clearOnBack: false

    id: window
    width: 1280
    height: 640

    // FluentWinUI3's ApplicationWindow is just "color: palette.window", and on macOS
    // that palette follows the system appearance regardless of the color scheme we
    // ask for. Pin it so pages we haven't given a background of their own (the
    // connection spinner, the quit page) are never white-on-white.
    color: Theme.ink

    // This function runs prior to creation of the initial StackView item
    function doEarlyInit() {
        // Override the background color to Material 2 colors for Qt 6.5+
        // in order to improve contrast between GFE's placeholder box art
        // and the background of the app grid.
        if (SystemProperties.usesMaterial3Theme) {
            Material.background = "#303030"
        }

        SdlGamepadKeyNavigation.enable()
    }

    Component.onCompleted: {
        // Show the window according to the user's preferences
        if (SystemProperties.hasDesktopEnvironment) {
            if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_MAXIMIZED) {
                window.showMaximized()
            }
            else if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_FULLSCREEN) {
                window.showFullScreen()
            }
            else {
                window.show()
            }
        } else {
            window.showFullScreen()
        }

        // Display any modal dialogs for configuration warnings
        if (runConfigChecks) {
            if (SystemProperties.isWow64) {
                wow64Dialog.open()
            }

            // Hardware acceleration and unmapped gamepads are checked asynchronously
            SystemProperties.hasHardwareAccelerationChanged.connect(hasHardwareAccelerationChanged)
            SystemProperties.unmappedGamepadsChanged.connect(hasUnmappedGamepadsChanged)
            SystemProperties.startAsyncLoad()
        }
    }

    function hasHardwareAccelerationChanged() {
        if (!SystemProperties.hasHardwareAcceleration && StreamingPreferences.videoDecoderSelection !== StreamingPreferences.VDS_FORCE_SOFTWARE) {
            if (SystemProperties.isRunningXWayland) {
                xWaylandDialog.open()
            }
            else {
                noHwDecoderDialog.open()
            }
        }
    }

    function hasUnmappedGamepadsChanged() {
        if (SystemProperties.unmappedGamepads) {
            unmappedGamepadDialog.unmappedGamepads = SystemProperties.unmappedGamepads
            unmappedGamepadDialog.open()
        }
    }

    // It would be better to use TextMetrics here, but it always lays out
    // the text slightly more compactly than real Text does in ToolTip,
    // causing unexpected line breaks to be inserted
    Text {
        id: tooltipTextLayoutHelper
        visible: false
        font: ToolTip.toolTip.font
        text: ToolTip.toolTip.text
    }

    function goBack() {
        if (clearOnBack) {
            // Pop all items except the first one
            stackView.pop(null)
            clearOnBack = false
        }
        else {
            stackView.pop()
        }
    }

    // 全局壁纸。PcView 负责抓取、缓存和刷新，抓到之后写回这里，
    // 这样连接进度页、退出页、设置页共用同一张背景，而不是各自一片纯色。
    property string backgroundImageUrl: ""

    // PcView / AppView / SettingsView 各自已经按自己的配方铺了一层壁纸（半透明 + 压暗），
    // 它们背后再垫一张全尺寸原图的话两层会错位叠在一起。所以这层只服务于自己不画背景的页面
    // ——连接进度页、退出页。
    readonly property bool showGlobalBackground:
        !(stackView.currentItem && stackView.currentItem.usesOwnBackground === true)

    Image {
        anchors.fill: parent
        source: window.backgroundImageUrl
        visible: source != "" && window.showGlobalBackground
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        z: -3
    }

    // 压暗壁纸，保证上层的文字和加载动画有足够对比度。用 ink 而不是纯黑，
    // 和各页自己那层遮罩同一个底色，切页时不会有色温跳变。
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(Theme.ink.r, Theme.ink.g, Theme.ink.b, 0.72)
        visible: window.showGlobalBackground
        z: -2
    }

    StackView {
        id: stackView
        anchors.fill: parent
        focus: true

        // 切页动效：neo-brutalism 要更短更机械，所以不缩放（缩放读起来是「软」的），
        // 改成 12px 横向位移 + 淡入，150ms OutQuad。
        pushEnter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durNormal; easing.type: Theme.easing }
                NumberAnimation { property: "x"; from: 12; to: 0; duration: Theme.durNormal; easing.type: Theme.easing }
            }
        }
        pushExit: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durFast; easing.type: Easing.InQuad }
                NumberAnimation { property: "x"; from: 0; to: -12; duration: Theme.durFast; easing.type: Easing.InQuad }
            }
        }
        popEnter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durNormal; easing.type: Theme.easing }
                NumberAnimation { property: "x"; from: -12; to: 0; duration: Theme.durNormal; easing.type: Theme.easing }
            }
        }
        popExit: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durFast; easing.type: Easing.InQuad }
                NumberAnimation { property: "x"; from: 0; to: 12; duration: Theme.durFast; easing.type: Easing.InQuad }
            }
        }

        // This configures the maximum width of the singleton attached QML ToolTip. If left unconstrained,
        // it will never insert a line break and just extend on forever.
        ToolTip.toolTip.contentWidth: Math.min(tooltipTextLayoutHelper.width, 400)
        ToolTip.toolTip.margins: 8

        ToolTip.toolTip.onVisibleChanged: {
            if (ToolTip.toolTip.visible) ToolTip.toolTip.y = toolBar.height - 5
        }

        Component.onCompleted: {
            // Perform our early initialization before constructing
            // the initial view and pushing it to the StackView
            doEarlyInit()
            push(initialView)
        }

        onCurrentItemChanged: {
            // Ensure focus travels to the next view when going back
            if (currentItem) {
                currentItem.forceActiveFocus()
            }
        }

        Keys.onEscapePressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onBackPressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onMenuPressed: {
            settingsButton.clicked()
        }

        // This is a keypress we've reserved for letting the
        // SdlGamepadKeyNavigation object tell us to show settings
        // when Menu is consumed by a focused control.
        Keys.onHangupPressed: {
            settingsButton.clicked()
        }
    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with Moonlight on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on Moonlight.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // When we become invisible while streaming is going on,
        // stop polling immediately.
        if (!visible) {
            inactivityTimer.stop()

            if (pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
        else if (active) {
            // When we become visible and active again, start polling
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    onActiveChanged: {
        if (active) {
            // Stop the inactivity timer
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
        else {
            // Start the inactivity timer to stop polling
            // if focus does not return within a few minutes.
            inactivityTimer.restart()
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    function navigateTo(url, objectType)
    {
        var existingItem = stackView.find(function(item, index) {
            return item instanceof objectType
        })

        if (existingItem !== null) {
            // Pop to the existing item
            stackView.pop(existingItem)
        }
        else {
            // Create a new item
            stackView.push(url)
        }
    }

    // 添加工具栏作为浮动元素
    ToolBar {
        id: toolBar

        // 各个 segue 页面用 shown 而不是直接写 visible：直接 visible=false 的话
        // 工具栏会「啪」地消失，而 visible 变假之后就不再渲染，opacity 动画也没机会跑。
        property bool shown: true
        opacity: shown ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation { duration: 220; easing.type: Easing.InOutQuad }
        }

        height: 56
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        z: 1

        // 以前这是一条「浮」在壁纸上的透明工具栏（topMargin: 5 + transparent 背景）。
        // 新风格里它是一条真正的 bar：贴住窗口顶边、底部一条 1px 分隔线，页面内容从
        // 线下面开始。底色留一半透明度让壁纸透上来 —— 全不透明的话这条 bar 会像一块
        // 贴在窗口上的黑板，和下面的壁纸完全割裂。不加模糊（这套风格里没有毛玻璃）。
        background: Rectangle {
            color: Qt.rgba(Theme.ink.r, Theme.ink.g, Theme.ink.b, 0.55)

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: Theme.line
            }
        }

        RowLayout {
            spacing: Theme.spaceSm
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            anchors.fill: parent

            NavigableToolButton {
                // Only make the button visible if the user has navigated somewhere.
                visible: stackView.depth > 1

                iconSource: "qrc:/res/fluent/tb-back.svg"

                onClicked: goBack()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            // 字标 + 页名面包屑，左对齐。窄窗口下字标先让位，页名一直留着。
            Text {
                id: wordmark
                visible: toolBar.width > 700
                text: "MOONLIGHT"
                color: Theme.text
                font.family: Theme.fontSans
                font.pointSize: Theme.fontCardTitle
                font.weight: Font.ExtraBold
                font.letterSpacing: Theme.tracking(Theme.fontCardTitle, 0.1)
                verticalAlignment: Text.AlignVCenter
                Layout.fillHeight: true
            }

            Text {
                visible: wordmark.visible
                text: "/"
                color: Theme.textFaint
                font.family: Theme.fontMono
                font.pointSize: Theme.fontCardTitle
                verticalAlignment: Text.AlignVCenter
                Layout.fillHeight: true
                Layout.leftMargin: Theme.spaceXs
                Layout.rightMargin: Theme.spaceXs
            }

            // 这一条必须始终存在：RowLayout 靠它 fillWidth 把右边那排按钮推到边上。
            Text {
                id: titleRowLabel
                text: stackView.currentItem ? stackView.currentItem.objectName : ""
                color: Theme.accent
                font.family: Theme.fontSans
                font.pointSize: Theme.fontRowTitle
                font.weight: Font.Bold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.tracking(Theme.fontRowTitle, 0.14)
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                Layout.fillHeight: true
                Layout.fillWidth: true
            }

            Text {
                id: versionLabel
                visible: stackView.currentItem instanceof SettingsView
                text: qsTr("Version %1").arg(SystemProperties.versionString)
                color: Theme.textDim
                font.family: Theme.fontMono
                font.pointSize: Theme.fontCaption
                horizontalAlignment: Qt.AlignRight
                verticalAlignment: Text.AlignVCenter
                Layout.fillHeight: true
                Layout.rightMargin: Theme.spaceSm
            }

            NavigableToolButton {
                id: qqButton
                visible: SystemProperties.hasBrowser &&
                         stackView.currentItem instanceof SettingsView

                iconSource: "qrc:/res/qq-2.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                // 源串必须是英文：这个仓库的源语言是 en_GB，中文源串会变成 28 个
                // 语言包里的 msgid，而且全都 unfinished —— 英语用户看到的就是那四个
                // 中文字。梗放到 zh_CN 的译文里，两边都能要。
                ToolTip.text: qsTr("Join our QQ group")

                // TODO need to make sure browser is brought to foreground.
                onClicked: Qt.openUrlExternally("https://qm.qq.com/cgi-bin/qm/qr?k=wI7aTvDQdd900n1L_wjjJw3qNP0yOgUa&jump_from=webapi&authKey=CDBn7sGy7HpCKYTcFmoEdNuG/zmkrBWUC/W5A/oZZycKzXwuO/XFCA97IpJRktj3");

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: addPcButton
                visible: stackView.currentItem instanceof PcView

                iconSource:  "qrc:/res/fluent/tb-add-pc.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add PC manually") + (newPcShortcut.nativeText ? (" ("+newPcShortcut.nativeText+")") : "")

                Shortcut {
                    id: newPcShortcut
                    sequence: StandardKey.New
                    onActivated: addPcButton.clicked()
                }

                onClicked: {
                    addPcDialog.open()
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                property string browserUrl: ""

                id: updateButton

                iconSource: "qrc:/res/fluent/tb-update.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered || visible

                // Invisible until we get a callback notifying us that
                // an update is available
                visible: false

                onClicked: {
                    if (AutoUpdateChecker.supportsInAppUpdate()) {
                        portableUpdateDialog.text = qsTr("Preparing portable update...")
                        portableUpdateDialog.open()
                        AutoUpdateChecker.installUpdate(browserUrl)
                    }
                    else if (SystemProperties.hasBrowser) {
                        Qt.openUrlExternally(browserUrl);
                    }
                }

                function updateAvailable(version, url)
                {
                    ToolTip.text = qsTr("Update available for Moonlight: Version %1").arg(version)
                    updateButton.browserUrl = url
                    updateButton.visible = true
                }

                function portableUpdateStatusChanged(message)
                {
                    portableUpdateDialog.text = message
                    if (!portableUpdateDialog.visible) {
                        portableUpdateDialog.open()
                    }
                }

                function portableUpdateFailed(message)
                {
                    portableUpdateDialog.close()
                    portableUpdateErrorDialog.text = message
                    portableUpdateErrorDialog.open()
                }

                Component.onCompleted: {
                    AutoUpdateChecker.onUpdateAvailable.connect(updateAvailable)
                    AutoUpdateChecker.onPortableUpdateStatusChanged.connect(portableUpdateStatusChanged)
                    AutoUpdateChecker.onPortableUpdateFailed.connect(portableUpdateFailed)
                    if (StreamingPreferences.autoUpdateCheck) {
                        AutoUpdateChecker.start()
                    }
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: helpButton
                visible: SystemProperties.hasBrowser

                iconSource: "qrc:/res/fluent/tb-help.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Help") + (helpShortcut.nativeText ? (" ("+helpShortcut.nativeText+")") : "")

                Shortcut {
                    id: helpShortcut
                    sequence: StandardKey.HelpContents
                    onActivated: helpButton.clicked()
                }

                // TODO need to make sure browser is brought to foreground.
                onClicked: Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide");

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                // TODO: Implement gamepad mapping then unhide this button
                visible: false

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Gamepad Mapper")

                iconSource: "qrc:/res/fluent/tb-gamepad.svg"

                onClicked: navigateTo("qrc:/gui/GamepadMapper.qml", GamepadMapper)

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: ipSettingsButton
                visible: stackView.currentItem instanceof AppView &&
                         stackView.currentItem.hasMultipleAddresses

                iconSource: "qrc:/res/fluent/tb-network.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Connection IP")

                onClicked: {
                    if (stackView.currentItem.openIpDialog) {
                        stackView.currentItem.openIpDialog()
                    }
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: displaySettingsButton
                visible: stackView.currentItem instanceof AppView

                iconSource: "qrc:/res/fluent/tb-display.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Display Settings")

                onClicked: {
                    if (stackView.currentItem.openDisplayDialog) {
                        stackView.currentItem.openDisplayDialog()
                    }
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: settingsButton

                iconSource:  "qrc:/res/fluent/tb-settings.svg"

                onClicked: navigateTo("qrc:/gui/SettingsView.qml", SettingsView)

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }

                Shortcut {
                    id: settingsShortcut
                    sequence: StandardKey.Preferences
                    onActivated: settingsButton.clicked()
                }

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Settings") + (settingsShortcut.nativeText ? (" ("+settingsShortcut.nativeText+")") : "")
            }
        }
    }

    ErrorMessageDialog {
        id: noHwDecoderDialog
        text: qsTr("No functioning hardware accelerated video decoder was detected by Moonlight. " +
                   "Your streaming performance may be severely degraded in this configuration.")
        helpText: qsTr("Click the Help button for more information on solving this problem.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems"
    }

    NavigableMessageDialog {
        id: portableUpdateDialog
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape
        showSpinner: true
        text: qsTr("Preparing portable update...")
    }

    ErrorMessageDialog {
        id: portableUpdateErrorDialog
        text: ""
    }

    ErrorMessageDialog {
        id: xWaylandDialog
        text: qsTr("Hardware acceleration doesn't work on XWayland. Continuing on XWayland may result in poor streaming performance. " +
                   "Try running with QT_QPA_PLATFORM=wayland or switch to X11.")
        helpText: qsTr("Click the Help button for more information.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems"
    }

    NavigableMessageDialog {
        id: wow64Dialog
        standardButtons: Dialog.Ok | Dialog.Cancel
        text: qsTr("This version of Moonlight isn't optimized for your PC. Please download the '%1' version of Moonlight for the best streaming performance.").arg(SystemProperties.friendlyNativeArchName)
        onAccepted: {
            Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-qt/releases");
        }
    }

    ErrorMessageDialog {
        id: unmappedGamepadDialog
        property string unmappedGamepads : ""
        text: qsTr("Moonlight detected gamepads without a mapping:") + "\n" + unmappedGamepads
        helpTextSeparator: "\n\n"
        helpText: qsTr("Click the Help button for information on how to map your gamepads.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Gamepad-Mapping"
    }

    // This dialog appears when quitting via keyboard or gamepad button
    NavigableMessageDialog {
        id: quitConfirmationDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Are you sure you want to quit?")
        // For keyboard/gamepad navigation
        onAccepted: Qt.quit()
    }

    // HACK: This belongs in StreamSegue but keeping a dialog around after the parent
    // dies can trigger bugs in Qt 5.12 that cause the app to crash. For now, we will
    // host this dialog in a QML component that is never destroyed.
    //
    // To repro: Start a stream, cut the network connection to trigger the "Connection
    // terminated" dialog, wait until the app grid times out back to the PC grid, then
    // try to dismiss the dialog.
    ErrorMessageDialog {
        id: streamSegueErrorDialog

        property bool quitAfter: false

        onClosed: {
            if (quitAfter) {
                Qt.quit()
            }

            // StreamSegue assumes its dialog will be re-created each time we
            // start streaming, so fake it by wiping out the text each time.
            text = ""
        }
    }

    NavigableDialog {
        id: addPcDialog
        property string label: qsTr("Enter the IP address of your host PC:")

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                ComputerManager.addNewHostManually(editText.text.trim())
            }
        }

        ColumnLayout {
            spacing: Theme.spaceSm

            Text {
                text: addPcDialog.label
                color: Theme.text
                font.family: Theme.fontSans
                font.pointSize: Theme.fontRowTitle
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }

            HardTextField {
                id: editText
                placeholderText: "192.168.1.100"
                Layout.fillWidth: true
                Layout.minimumWidth: 260
                focus: true

                Keys.onReturnPressed: {
                    addPcDialog.accept()
                }

                Keys.onEnterPressed: {
                    addPcDialog.accept()
                }
            }

            // 云主机推广。放在这里是因为「我没有可以串流的主机」正好是打开这个框的
            // 人最可能卡住的地方 —— 手动填 IP 填不出一台主机来。
            //
            // 没有浏览器可用时整块隐藏（和 QQ 按钮同一个判断），否则按钮点了没反应。
            Item {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceSm
                implicitHeight: promoColumn.implicitHeight
                visible: SystemProperties.hasBrowser

                Column {
                    id: promoColumn

                    anchors { left: parent.left; right: parent.right }
                    spacing: Theme.spaceSm

                    Rectangle {
                        width: parent.width
                        height: 1
                        color: Theme.line
                    }

                    MicroLabel {
                        width: parent.width
                        text: qsTr("No host PC of your own?")
                        // 这句比一般微标签长，允许折行（MicroLabel 默认单行省略）
                        elide: Text.ElideNone
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        text: qsTr("Procriva Cloud rents out cloud hosts that are ready to stream.")
                        color: Theme.text
                        font.family: Theme.fontSans
                        font.pointSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }

                    HardButton {
                        text: qsTr("Learn more")
                        // 焦点默认停在输入框上，别让这颗按钮抢走
                        focusPolicy: Qt.TabFocus

                        onClicked: Qt.openUrlExternally("https://client.cloud.procriva.com/")
                    }
                }
            }
        }
    }
}
