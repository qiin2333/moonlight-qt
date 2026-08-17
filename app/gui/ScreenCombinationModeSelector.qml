import QtQuick 2.9
import QtQuick.Controls
import QtQuick.Layouts 1.3

import StreamingPreferences 1.0

import "theme"

Item {
    id: selector

    property bool compact: false
    property bool saveOnSelection: false
    property Item navUpItem: null
    property Item navDownItem: null
    readonly property int columns: compact ? 3 : 2
    readonly property Item firstItem: optionRepeater.count > 0 ? optionRepeater.itemAt(0) : null
    readonly property int optionCount: modeModel.count

    implicitHeight: optionGrid.implicitHeight

    function selectMode(modeValue) {
        StreamingPreferences.screenCombinationMode = modeValue
        if (saveOnSelection) {
            StreamingPreferences.save()
        }
    }

    ListModel {
        id: modeModel

        ListElement {
            title: qsTr("Follow host")
            description: qsTr("Use the Sunshine host setting without overriding it.")
            modeValue: -1
        }
        ListElement {
            title: qsTr("No operation mode")
            description: qsTr("Send no-operation mode and keep the current display layout.")
            modeValue: 0
        }
        ListElement {
            title: qsTr("Activate mode")
            description: qsTr("Activate the selected display for streaming.")
            modeValue: 1
        }
        ListElement {
            title: qsTr("Primary streaming mode")
            description: qsTr("Activate the selected display and set it as primary.")
            modeValue: 2
        }
        ListElement {
            title: qsTr("Secondary streaming mode")
            description: qsTr("Keep the physical display primary and use the Foundation display as secondary.")
            modeValue: 4
        }
        ListElement {
            title: qsTr("Exclusive display streaming mode")
            description: qsTr("Use only the selected display and disable other displays.")
            modeValue: 3
        }
    }

    GridLayout {
        id: optionGrid

        width: parent.width
        columns: selector.columns
        columnSpacing: Theme.spaceSm
        rowSpacing: Theme.spaceSm

        Repeater {
            id: optionRepeater
            model: modeModel

            AbstractButton {
                id: optionButton

                readonly property bool selected: StreamingPreferences.screenCombinationMode === model.modeValue

                Layout.fillWidth: true
                Layout.preferredWidth: (selector.width - optionGrid.columnSpacing * (selector.columns - 1)) / selector.columns
                Layout.preferredHeight: selector.compact ? 78 : 112

                activeFocusOnTab: true
                hoverEnabled: true

                onClicked: selector.selectMode(model.modeValue)

                KeyNavigation.left: index % selector.columns > 0
                                    ? optionRepeater.itemAt(index - 1) : null
                KeyNavigation.right: index % selector.columns < selector.columns - 1
                                     && index + 1 < optionRepeater.count
                                     ? optionRepeater.itemAt(index + 1) : null
                KeyNavigation.up: index >= selector.columns
                                  ? optionRepeater.itemAt(index - selector.columns)
                                  : selector.navUpItem
                KeyNavigation.down: index + selector.columns < optionRepeater.count
                                    ? optionRepeater.itemAt(index + selector.columns)
                                    : selector.navDownItem

                background: Rectangle {
                    color: optionButton.selected ? Theme.accentSoft
                                                 : (optionButton.hovered || optionButton.visualFocus
                                                    ? Theme.surface2 : Theme.surfaceLayer)
                    border.width: optionButton.visualFocus ? 2 : 1
                    border.color: optionButton.selected || optionButton.visualFocus
                                  ? Theme.accent : Theme.lineStrong

                    Rectangle {
                        anchors {
                            left: parent.left
                            top: parent.top
                            bottom: parent.bottom
                        }
                        width: optionButton.selected ? Theme.accentBar : 0
                        color: Theme.accent
                    }
                }

                contentItem: Item {
                    ScreenCombinationPreview {
                        id: optionPreview
                        width: selector.compact ? 54 : 112
                        height: selector.compact ? 46 : 76
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.spaceSm + (optionButton.selected ? Theme.accentBar : 0)
                        anchors.verticalCenter: parent.verticalCenter
                        modeValue: model.modeValue
                        selected: optionButton.selected
                    }

                    Column {
                        anchors {
                            left: optionPreview.right
                            leftMargin: selector.compact ? Theme.spaceXs : Theme.spaceMd
                            right: parent.right
                            rightMargin: Theme.spaceSm
                        }
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.spaceXs

                        Text {
                            width: parent.width
                            text: model.title
                            color: Theme.text
                            font.family: Theme.fontSans
                            font.pointSize: selector.compact ? Theme.fontCaption : Theme.fontRowTitle
                            font.weight: Font.Bold
                            wrapMode: Text.Wrap
                            maximumLineCount: selector.compact ? 2 : 2
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            visible: !selector.compact
                            text: model.description
                            color: Theme.textDim
                            font.family: Theme.fontMono
                            font.pointSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                        }

                        MicroLabel {
                            visible: optionButton.selected && !selector.compact
                            text: qsTr("Selected")
                            color: Theme.accent
                            font.weight: Font.Bold
                        }
                    }
                }

                ToolTip.delay: 500
                ToolTip.timeout: 10000
                ToolTip.visible: selector.compact && hovered
                ToolTip.text: model.title + "\n" + model.description
            }
        }
    }
}
