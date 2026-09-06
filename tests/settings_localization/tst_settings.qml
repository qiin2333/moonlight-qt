import QtQuick
import QtTest
import "../../app/gui/settings"

TestCase {
    name: "SettingsLocalization"
    width: 760
    height: 640
    when: windowShown

    FontLoader { source: "../../app/res/fonts/Manrope-Regular.ttf" }
    FontLoader { source: "../../app/res/fonts/Manrope-ExtraBold.ttf" }

    Component {
        id: railComponent
        CategoryRail {
            height: 600
            categories: [
                { key: "gamepad", title: "Gamepad Settings", icon: "" },
                { key: "peripherals", title: "Peripherals Settings", icon: "" },
                { key: "software", title: "Software Settings", icon: "" },
                { key: "ecosystem", title: "AlkaidLab Ecosystem", icon: "" }
            ]
        }
    }

    function test_compiledChinese() {
        compare(qsTranslate("SettingsView", "Settings"), "设置")
        compare(qsTranslate("SettingsView", "Software Settings"), "软件设置")
        compare(qsTranslate("LegacySettingsPage", "Language"), "语言")
        compare(qsTranslate("AboutSettingsPage", "About"), "关于")
        verify(qsTranslate("OverlayMenuPanel", "Connected — select to release") !== "Connected — select to release")
        verify(qsTranslate("StylusReplayTest", "Stylus replay stopped.") !== "Stylus replay stopped.")
    }

    function test_titlesFit_data() {
        return [
            { tag: "narrow-rail", railWidth: 168, compact: false },
            { tag: "standard-rail", railWidth: 200, compact: false },
            { tag: "horizontal-tabs", railWidth: 740, compact: true }
        ]
    }

    function test_titlesFit(data) {
        var rail = createTemporaryObject(railComponent, this, {
            width: data.railWidth, compact: data.compact,
            height: data.compact ? 46 : 600
        })
        verify(rail)
        var list = rail.children[0]
        for (var i = 0; i < rail.categories.length; ++i) {
            rail.currentCategory = rail.categories[i].key
            rail.ensureCurrentVisible()
            tryVerify(function() { return list.itemAtIndex(i) !== null })
            wait(50)
            var row = list.itemAtIndex(i)
            var label = row.contentItem.children[1]
            verify(!label.truncated, label.text + " is truncated")
            verify(label.contentWidth <= label.width + 1, label.text + " overflows horizontally")
            verify(label.height <= row.availableHeight + 1, label.text + " overflows vertically")
            verify(label.x + label.width <= row.contentItem.width + 1)
        }
    }
}
