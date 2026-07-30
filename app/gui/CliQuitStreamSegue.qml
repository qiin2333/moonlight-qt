import QtQuick 2.0
import QtQuick.Controls 2.2

import ComputerManager 1.0
import Session 1.0

import "theme"

Item {
    function onSearchingComputer() {
        stageLabel.text = qsTr("Establishing connection to PC...")
    }

    function onQuittingApp() {
        stageLabel.text = qsTr("Quitting app...")
    }

    function onFailure(message) {
        errorDialog.text = message
        errorDialog.open()
    }

    StackView.onActivated: {
        if (!launcher.isExecuted()) {
            toolBar.shown = false
            launcher.searchingComputer.connect(onSearchingComputer)
            launcher.quittingApp.connect(onQuittingApp)
            launcher.failed.connect(onFailure)
            launcher.execute(ComputerManager)
        }
    }

    // 和串流加载页 / 退出页同一套：Manrope 800 左对齐阶段文字 + 斜条纹读条。
    // 命令行入口以前还留着转圈的 BusyIndicator，那是整个应用里最后几处圆形动效。
    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.spaceXl * 2, 620)
        spacing: Theme.spaceLg

        Text {
            id: stageLabel

            width: parent.width
            text: stageText
            color: Theme.text
            font.family: Theme.fontSans
            font.pointSize: 24
            font.weight: Font.ExtraBold
            font.letterSpacing: Theme.trackingTight(24)
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
        }

        HardProgress {
            id: stageSpinner

            width: parent.width
        }
    }

    ErrorMessageDialog {
        id: errorDialog

        onClosed: {
            Qt.quit()
        }
    }
}
