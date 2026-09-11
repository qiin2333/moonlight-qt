import QtQuick
import "."
import "../theme"

Flow {
    id: cloud

    property var tags: []

    spacing: Theme.spaceSm

    Repeater {
        model: cloud.tags

        TechTag {
            required property string modelData

            text: modelData
        }
    }
}
