import QtQuick
import QtQuick.Controls
import QtQuick.Layouts 1.3

import SystemProperties 1.0

import "theme"

Item {
    id: control

    property var page
    property bool expanded: page ? page.searchExpanded : false

    width: expanded ? Math.min(340, parent ? parent.width : 340) : 150
    height: 40
    visible: page !== null
    z: 10

    Behavior on width {
        NumberAnimation { duration: Theme.durFast }
    }

    Connections {
        target: control.page
        function onSearchFocusRequested() {
            Qt.callLater(function() {
                if (control.expanded) {
                    searchField.forceActiveFocus(Qt.ShortcutFocusReason)
                }
            })
        }
    }

    Button {
        id: searchButton
        visible: !control.expanded
        anchors.fill: parent
        activeFocusOnTab: true
        Accessible.name: qsTranslate("SettingsView", "Search")
        ToolTip.visible: hovered
        ToolTip.text: qsTranslate("SettingsView", "Search settings...") +
                      (SystemProperties.isDarwin ? " (Cmd+K)" : " (Ctrl+K)")

        background: Rectangle {
            radius: 6
            color: searchButton.down ? Theme.accentSoft
                                     : (searchButton.hovered ? Theme.surface2 : Theme.ink)
            border.width: searchButton.visualFocus ? 2 : 1
            border.color: searchButton.visualFocus ? Theme.accent : Theme.lineStrong
        }

        contentItem: Row {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceSm
            anchors.rightMargin: Theme.spaceSm
            spacing: Theme.spaceSm

            Image {
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                source: "qrc:/res/fluent/tb-search.svg"
                opacity: 0.9
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTranslate("SettingsView", "Search")
                color: Theme.text
                font.family: Theme.fontSans
                font.pointSize: Theme.fontBody
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: shortcutLabel.implicitWidth + Theme.spaceSm * 2
                height: 26
                radius: 4
                color: Theme.surface2
                border.width: 1
                border.color: Theme.line

                Text {
                    id: shortcutLabel
                    anchors.centerIn: parent
                    text: SystemProperties.isDarwin ? "⌘ + K" : "Ctrl + K"
                    color: Theme.textDim
                    font.family: Theme.fontMono
                    font.pointSize: Theme.fontCaption
                }
            }
        }

        onClicked: control.page.openSearch()
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
    }

    Rectangle {
        id: expandedSearchBox
        visible: control.expanded
        anchors.fill: parent
        radius: 5
        color: Theme.ink
        border.width: searchField.activeFocus ? 2 : 1
        border.color: searchField.activeFocus ? Theme.accent : Theme.lineStrong

        Image {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            source: "qrc:/res/fluent/tb-search.svg"
            opacity: 0.9
        }

        HardTextField {
            id: searchField
            objectName: "settingsSearchField"
            anchors {
                left: parent.left
                leftMargin: 36
                right: clearSearchButton.left
                rightMargin: Theme.spaceXs
                verticalCenter: parent.verticalCenter
            }
            height: parent.height
            placeholderText: qsTranslate("SettingsView", "Search settings...")
            Accessible.name: qsTranslate("SettingsView", "Search settings...")
            text: control.page ? control.page.searchQuery : ""
            background: null
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0

            onTextChanged: {
                if (control.page && text !== control.page.searchQuery) {
                    control.page.searchQuery = text
                }
            }

            Keys.onEscapePressed: function(event) {
                control.page.closeSearch()
                event.accepted = true
            }
            Keys.onDownPressed: function(event) {
                if (searchResultsList.count > 0) {
                    searchResultsList.currentIndex = 0
                    Qt.callLater(function() {
                        if (searchResultsList.currentItem) {
                            searchResultsList.currentItem.forceActiveFocus(Qt.TabFocusReason)
                        }
                    })
                    event.accepted = true
                }
            }
            Keys.onReturnPressed: function(event) {
                if (searchResultsList.count > 0) {
                    control.page.openSearchResult(searchResultsList.model[0])
                    event.accepted = true
                }
            }
            Keys.onEnterPressed: function(event) {
                if (searchResultsList.count > 0) {
                    control.page.openSearchResult(searchResultsList.model[0])
                    event.accepted = true
                }
            }
        }

        ToolButton {
            id: clearSearchButton
            anchors {
                right: parent.right
                rightMargin: Theme.spaceXs
                verticalCenter: parent.verticalCenter
            }
            width: 32
            height: 32
            icon.source: "qrc:/res/fluent/menu-close.svg"
            icon.width: 20
            icon.height: 20
            Accessible.name: qsTranslate("SettingsView", "Clear")
            background: Rectangle {
                radius: 3
                color: clearSearchButton.hovered || clearSearchButton.visualFocus
                       ? Theme.surface2 : "transparent"
            }
            onClicked: {
                if (control.page.searchQuery !== "") {
                    control.page.searchQuery = ""
                }
                else {
                    control.page.closeSearch()
                }
            }
            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
        }
    }

    Rectangle {
        id: searchResultsPanel
        visible: control.expanded && control.page &&
                 control.page.searchQuery.trim() !== ""
        anchors {
            top: expandedSearchBox.bottom
            horizontalCenter: expandedSearchBox.horizontalCenter
        }
        width: expandedSearchBox.width
        height: searchResultsList.count > 0
                ? Math.min(360, searchResultsList.contentHeight) : 44
        color: Theme.surfaceLayer
        border.width: 1
        border.color: Theme.lineStrong
        z: 20

        ListView {
            id: searchResultsList
            anchors.fill: parent
            visible: count > 0
            clip: true
            model: control.page ? control.page.searchResults : []
            boundsBehavior: Flickable.StopAtBounds

            delegate: ItemDelegate {
                width: searchResultsList.width
                height: 56
                activeFocusOnTab: true
                onClicked: control.page.openSearchResult(modelData)

                contentItem: Column {
                    spacing: Theme.spaceXs
                    Text {
                                        text: modelData.row && modelData.row.title !== undefined
                                              ? modelData.row.title : modelData.title
                        color: Theme.text
                        font.family: Theme.fontSans
                        font.pointSize: Theme.fontRowTitle
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                                        text: modelData.row && modelData.row.description !== undefined &&
                                              modelData.row.description !== ""
                                              ? modelData.row.description : modelData.categoryTitle
                        color: Theme.textSettingsSubtitle
                        font.family: Theme.fontSans
                        font.pointSize: Theme.fontSettingsSubtitle
                        elide: Text.ElideRight
                    }
                }

                Keys.onEscapePressed: function(event) {
                    control.page.closeSearch()
                    event.accepted = true
                }
                Keys.onReturnPressed: function(event) {
                    control.page.openSearchResult(modelData)
                    event.accepted = true
                }
                Keys.onEnterPressed: function(event) {
                    control.page.openSearchResult(modelData)
                    event.accepted = true
                }
                Keys.onUpPressed: function(event) {
                    if (index === 0) {
                        searchField.forceActiveFocus(Qt.TabFocusReason)
                    }
                    else {
                        searchResultsList.currentIndex = index - 1
                        searchResultsList.currentItem.forceActiveFocus(Qt.TabFocusReason)
                    }
                    event.accepted = true
                }
                Keys.onDownPressed: function(event) {
                    if (index + 1 < searchResultsList.count) {
                        searchResultsList.currentIndex = index + 1
                        searchResultsList.currentItem.forceActiveFocus(Qt.TabFocusReason)
                    }
                    event.accepted = true
                }
            }
        }

        Text {
            anchors.centerIn: parent
            visible: searchResultsList.count === 0
            text: qsTranslate("SettingsView", "No matching settings")
            color: Theme.textSettingsSubtitle
            font.family: Theme.fontSans
            font.pointSize: Theme.fontSettingsSubtitle
        }
    }
}
