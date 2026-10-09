import QtQuick 2.9
import QtQuick.Controls
import QtQuick.Layouts 1.3
import "."
import "../theme"
import ".."

import StreamingPreferences 1.0
import SystemProperties 1.0

// 「基本设置」——第一个迁移到新架构的分类。
// 逻辑与旧 SettingsView 的 basicSettingsGroupBox 完全一致，只是换成卡片 + 设置行。
Column {
    id: basicPage

    signal languageChanged()

    // Piecewise-linear bitrate scale. Values are Kbps.
    readonly property int bitrateMinKbps: 500
    readonly property int bitrateMaxKbps: 800000
    readonly property var bitrateSegments: [
        { maxKbps:   5000, stepKbps:    500 }, // 0.5–5 Mbps: 0.5 Mbps
        { maxKbps:  20000, stepKbps:   1000 }, // 5–20 Mbps: 1 Mbps
        { maxKbps:  50000, stepKbps:   2000 }, // 20–50 Mbps: 2 Mbps
        { maxKbps: 100000, stepKbps:   5000 }, // 50–100 Mbps: 5 Mbps
        { maxKbps: 200000, stepKbps:  10000 }, // 100–200 Mbps: 10 Mbps
        { maxKbps: 400000, stepKbps:  50000 }, // 200–400 Mbps: 50 Mbps
        { maxKbps: 800000, stepKbps: 100000 }  // 400–800 Mbps: 100 Mbps
    ]
    readonly property int bitrateSliderMax: bitrateToSliderValue(bitrateMaxKbps)

    function bitrateToSliderValue(bitrateKbps) {
        var bitrate = Math.max(bitrateMinKbps, Math.min(bitrateMaxKbps, bitrateKbps))
        var position = 0
        var segmentMinKbps = bitrateMinKbps
        for (var i = 0; i < bitrateSegments.length; ++i) {
            var segment = bitrateSegments[i]
            if (bitrate <= segment.maxKbps)
                return position + (bitrate - segmentMinKbps) / segment.stepKbps
            position += (segment.maxKbps - segmentMinKbps) / segment.stepKbps
            segmentMinKbps = segment.maxKbps
        }
        return position
    }

    function sliderValueToBitrate(sliderValue) {
        var position = Math.max(0, Math.min(bitrateSliderMax, Math.round(sliderValue)))
        var segmentStart = 0
        var segmentMinKbps = bitrateMinKbps
        for (var i = 0; i < bitrateSegments.length; ++i) {
            var segment = bitrateSegments[i]
            var segmentSteps = (segment.maxKbps - segmentMinKbps) / segment.stepKbps
            if (position <= segmentStart + segmentSteps)
                return segmentMinKbps + (position - segmentStart) * segment.stepKbps
            segmentStart += segmentSteps
            segmentMinKbps = segment.maxKbps
        }
        return bitrateMaxKbps
    }

    width: parent ? parent.width : 0
    spacing: Theme.spaceLg

    // ================= 画面 =================
    SettingsCard {
        title: qsTr("Video")
        subtitle: qsTr("Setting values too high for your PC or network connection may cause lag, stuttering, or errors.")

        SettingsRow {
            title: qsTr("Resolution and FPS")

            Row {
                spacing: Theme.spaceSm

                AutoResizingComboBox {
                    id: resolutionComboBox

                    property int lastIndexValue

                    maximumWidth: 300
                    textRole: "text"

                    function addDetectedResolution(friendlyNamePrefix, rect) {
                        var indexToAdd = 0
                        for (var j = 0; j < resolutionComboBox.count; j++) {
                            var existing_width = parseInt(resolutionListModel.get(j).video_width);
                            var existing_height = parseInt(resolutionListModel.get(j).video_height);

                            if (rect.width === existing_width && rect.height === existing_height) {
                                // Duplicate entry, skip
                                indexToAdd = -1
                                break
                            }
                            else if (rect.width * rect.height > existing_width * existing_height) {
                                // Candidate entrypoint after this entry
                                indexToAdd = j + 1
                            }
                        }

                        // Insert this display's resolution if it's not a duplicate
                        if (indexToAdd >= 0) {
                            resolutionListModel.insert(indexToAdd,
                                                       {
                                                           "text": friendlyNamePrefix+" ("+rect.width+"x"+rect.height+")",
                                                           "video_width": ""+rect.width,
                                                           "video_height": ""+rect.height,
                                                           "is_custom": false
                                                       })
                        }
                    }

                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        // Refresh display data before using it to build the list
                        SystemProperties.refreshDisplays()

                        // Add native and safe area resolutions for all attached displays
                        var done = false
                        for (var displayIndex = 0; !done; displayIndex++) {
                            var screenRect = SystemProperties.getNativeResolution(displayIndex);
                            var safeAreaRect = SystemProperties.getSafeAreaResolution(displayIndex);

                            if (screenRect.width === 0) {
                                // Exceeded max count of displays
                                done = true
                                break
                            }

                            addDetectedResolution(qsTr("Native"), screenRect)
                            addDetectedResolution(qsTr("Native (Excluding Notch)"), safeAreaRect)
                        }

                        // Prune resolutions that are over the decoder's maximum
                        var max_pixels = SystemProperties.maximumResolution.width * SystemProperties.maximumResolution.height;
                        if (max_pixels > 0) {
                            for (var j = 0; j < resolutionComboBox.count; j++) {
                                var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                if (existing_width * existing_height > max_pixels) {
                                    resolutionListModel.remove(j)
                                    j--
                                }
                            }
                        }

                        // load the saved width/height, and iterate through the ComboBox until a match is found
                        // and set it to that index.
                        var saved_width = StreamingPreferences.width
                        var saved_height = StreamingPreferences.height
                        var index_set = false
                        for (var i = 0; i < resolutionListModel.count; i++) {
                            var el_width = parseInt(resolutionListModel.get(i).video_width);
                            var el_height = parseInt(resolutionListModel.get(i).video_height);

                            if (saved_width === el_width && saved_height === el_height) {
                                currentIndex = i
                                index_set = true
                                break
                            }
                        }

                        if (!index_set) {
                            // We did not find a match. This must be a custom resolution.
                            resolutionListModel.append({
                                                           "text": qsTr("Custom")+" ("+StreamingPreferences.width+"x"+StreamingPreferences.height+")",
                                                           "video_width": ""+StreamingPreferences.width,
                                                           "video_height": ""+StreamingPreferences.height,
                                                           "is_custom": true
                                                       })
                            currentIndex = resolutionListModel.count - 1
                        }
                        else {
                            resolutionListModel.append({
                                                           "text": qsTr("Custom"),
                                                           "video_width": "",
                                                           "video_height": "",
                                                           "is_custom": true
                                                       })
                        }

                        // Since we don't call activate() here, we need to trigger
                        // width calculation manually
                        recalculateWidth()

                        lastIndexValue = currentIndex
                    }

                    model: ListModel {
                        id: resolutionListModel
                        // Other elements may be added at runtime
                        // based on attached display resolution
                        ListElement {
                            text: qsTr("720p")
                            video_width: "1280"
                            video_height: "720"
                            is_custom: false
                        }
                        ListElement {
                            text: qsTr("1080p")
                            video_width: "1920"
                            video_height: "1080"
                            is_custom: false
                        }
                        ListElement {
                            text: qsTr("1440p")
                            video_width: "2560"
                            video_height: "1440"
                            is_custom: false
                        }
                        ListElement {
                            text: qsTr("4K")
                            video_width: "3840"
                            video_height: "2160"
                            is_custom: false
                        }
                    }

                    function updateBitrateForSelection() {
                        var selectedWidth = parseInt(resolutionListModel.get(currentIndex).video_width)
                        var selectedHeight = parseInt(resolutionListModel.get(currentIndex).video_height)

                        // Only modify the bitrate if the values actually changed
                        if (StreamingPreferences.width !== selectedWidth || StreamingPreferences.height !== selectedHeight) {
                            StreamingPreferences.width = selectedWidth
                            StreamingPreferences.height = selectedHeight

                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                          StreamingPreferences.height,
                                                                                                          StreamingPreferences.fps,
                                                                                                          StreamingPreferences.enableYUV444);
                            }
                        }

                        lastIndexValue = currentIndex
                    }

                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        if (resolutionListModel.get(currentIndex).is_custom) {
                            customResolutionDialog.open()
                        }
                        else {
                            updateBitrateForSelection()
                        }
                    }

                    NavigableDialog {
                        id: customResolutionDialog
                        standardButtons: Dialog.Ok | Dialog.Cancel
                        onOpened: {
                            // Force keyboard focus on the textbox so keyboard navigation works
                            widthField.forceActiveFocus()

                            // standardButton() was added in Qt 5.10, so we must check for it first
                            if (customResolutionDialog.standardButton) {
                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                            }
                        }

                        onClosed: {
                            widthField.clear()
                            heightField.clear()
                        }

                        onRejected: {
                            resolutionComboBox.currentIndex = resolutionComboBox.lastIndexValue
                        }

                        function isInputValid() {
                            // If we have text in either textbox that isn't valid,
                            // reject the input.
                            if ((!widthField.acceptableInput && widthField.text) ||
                                    (!heightField.acceptableInput && heightField.text)) {
                                return false
                            }

                            // The textboxes need to have text or placeholder text
                            if ((!widthField.text && !widthField.placeholderText) ||
                                    (!heightField.text && !heightField.placeholderText)) {
                                return false
                            }

                            return true
                        }

                        onAccepted: {
                            // Reject if there's invalid input
                            if (!isInputValid()) {
                                reject()
                                return
                            }

                            var width = widthField.text ? widthField.text : widthField.placeholderText
                            var height = heightField.text ? heightField.text : heightField.placeholderText

                            // Find and update the custom entry
                            for (var i = 0; i < resolutionListModel.count; i++) {
                                if (resolutionListModel.get(i).is_custom) {
                                    resolutionListModel.setProperty(i, "video_width", width)
                                    resolutionListModel.setProperty(i, "video_height", height)
                                    resolutionListModel.setProperty(i, "text", qsTr("Custom")+" ("+width+"x"+height+")")

                                    // Now update the bitrate using the custom resolution
                                    resolutionComboBox.currentIndex = i
                                    resolutionComboBox.updateBitrateForSelection()

                                    // Update the combobox width too
                                    resolutionComboBox.recalculateWidth()
                                    break
                                }
                            }
                        }

                        ColumnLayout {
                            Label {
                                text: qsTr("Custom resolutions are not officially supported by GeForce Experience, so it will not set your host display resolution. You will need to set it manually while in game.") + "\n\n" +
                                      qsTr("Resolutions that are not supported by your client or host PC may cause streaming errors.") + "\n"
                                wrapMode: Label.WordWrap
                                Layout.maximumWidth: 300
                            }

                            Label {
                                text: qsTr("Enter a custom resolution:")
                                font.bold: true
                            }

                            RowLayout {
                                HardTextField {
                                    id: widthField
                                    maximumLength: 5
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_width
                                    validator: IntValidator{bottom:256; top:8192}
                                    focus: true

                                    onTextChanged: {
                                        // standardButton() was added in Qt 5.10, so we must check for it first
                                        if (customResolutionDialog.standardButton) {
                                            customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                        }
                                    }

                                    Keys.onReturnPressed: {
                                        customResolutionDialog.accept()
                                    }

                                    Keys.onEnterPressed: {
                                        customResolutionDialog.accept()
                                    }
                                }

                                Label {
                                    text: "x"
                                    font.bold: true
                                }

                                HardTextField {
                                    id: heightField
                                    maximumLength: 5
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_height
                                    validator: IntValidator{bottom:256; top:8192}

                                    onTextChanged: {
                                        // standardButton() was added in Qt 5.10, so we must check for it first
                                        if (customResolutionDialog.standardButton) {
                                            customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                        }
                                    }

                                    Keys.onReturnPressed: {
                                        customResolutionDialog.accept()
                                    }

                                    Keys.onEnterPressed: {
                                        customResolutionDialog.accept()
                                    }
                                }
                            }
                        }
                    }
                }

                AutoResizingComboBox {
                    id: fpsComboBox

                    property int lastIndexValue

                    maximumWidth: 180
                    textRole: "text"

                    function updateBitrateForSelection() {
                        // Only modify the bitrate if the values actually changed
                        var selectedFps = parseInt(model.get(fpsComboBox.currentIndex).video_fps)
                        if (StreamingPreferences.fps !== selectedFps) {
                            StreamingPreferences.fps = selectedFps

                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                          StreamingPreferences.height,
                                                                                                          StreamingPreferences.fps,
                                                                                                          StreamingPreferences.enableYUV444);
                            }
                        }

                        lastIndexValue = currentIndex
                    }

                    NavigableDialog {
                        id: customFpsDialog
                        standardButtons: Dialog.Ok | Dialog.Cancel

                        function isInputValid() {
                            // If we have text that isn't valid, reject the input.
                            if (!fpsField.acceptableInput && fpsField.text) {
                                return false
                            }

                            // The textbox needs to have text or placeholder text
                            if (!fpsField.text && !fpsField.placeholderText) {
                                return false
                            }

                            return true
                        }

                        onOpened: {
                            // Force keyboard focus on the textbox so keyboard navigation works
                            fpsField.forceActiveFocus()

                            // standardButton() was added in Qt 5.10, so we must check for it first
                            if (customFpsDialog.standardButton) {
                                customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                            }
                        }

                        onClosed: {
                            fpsField.clear()
                        }

                        onRejected: {
                            fpsComboBox.currentIndex = fpsComboBox.lastIndexValue
                        }

                        onAccepted: {
                            // Reject if there's invalid input
                            if (!isInputValid()) {
                                reject()
                                return
                            }

                            var fps = fpsField.text ? fpsField.text : fpsField.placeholderText

                            // Find and update the custom entry
                            for (var i = 0; i < fpsListModel.count; i++) {
                                if (fpsListModel.get(i).is_custom) {
                                    fpsListModel.setProperty(i, "video_fps", fps)
                                    fpsListModel.setProperty(i, "text", qsTr("Custom (%1 FPS)").arg(fps))

                                    // Now update the bitrate using the custom resolution
                                    fpsComboBox.currentIndex = i
                                    fpsComboBox.updateBitrateForSelection()

                                    // Update the combobox width too
                                    fpsComboBox.recalculateWidth()
                                    break
                                }
                            }
                        }

                        ColumnLayout {
                            Label {
                                text: qsTr("Enter a custom frame rate:")
                                font.bold: true
                            }

                            RowLayout {
                                HardTextField {
                                    id: fpsField
                                    maximumLength: 4
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    placeholderText: fpsListModel.get(fpsComboBox.currentIndex).video_fps
                                    validator: IntValidator{bottom:10; top:9999}
                                    focus: true

                                    onTextChanged: {
                                        // standardButton() was added in Qt 5.10, so we must check for it first
                                        if (customFpsDialog.standardButton) {
                                            customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                        }
                                    }

                                    Keys.onReturnPressed: {
                                        customFpsDialog.accept()
                                    }

                                    Keys.onEnterPressed: {
                                        customFpsDialog.accept()
                                    }
                                }
                            }
                        }
                    }

                    function addRefreshRateOrdered(fpsListModel, refreshRate, description, custom) {
                        var indexToAdd = 0
                        for (var j = 0; j < fpsListModel.count; j++) {
                            var existing_fps = parseInt(fpsListModel.get(j).video_fps);

                            if (refreshRate === existing_fps || (custom && fpsListModel.get(j).is_custom)) {
                                // Duplicate entry, skip
                                indexToAdd = -1
                                break
                            }
                            else if (refreshRate > existing_fps) {
                                // Candidate entrypoint after this entry
                                indexToAdd = j + 1
                            }
                        }

                        // Insert this frame rate if it's not a duplicate
                        if (indexToAdd >= 0) {
                            // Custom values always go at the end of the list
                            if (custom) {
                                indexToAdd = fpsListModel.count
                            }

                            fpsListModel.insert(indexToAdd,
                                                {
                                                    "text": description,
                                                    "video_fps": ""+refreshRate,
                                                    "is_custom": custom
                                                })
                        }

                        return indexToAdd
                    }

                    function reinitialize() {
                        // Add native refresh rate for all attached displays
                        var done = false
                        for (var displayIndex = 0; !done; displayIndex++) {
                            var refreshRate = SystemProperties.getRefreshRate(displayIndex);
                            if (refreshRate === 0) {
                                // Exceeded max count of displays
                                done = true
                                break
                            }

                            addRefreshRateOrdered(fpsListModel, refreshRate, qsTr("%1 FPS").arg(refreshRate), false)
                        }

                        var saved_fps = StreamingPreferences.fps
                        var found = false
                        for (var i = 0; i < model.count; i++) {
                            var el_fps = parseInt(model.get(i).video_fps);

                            // Look for a matching frame rate
                            if (saved_fps === el_fps) {
                                currentIndex = i
                                found = true
                                break
                            }
                        }

                        // If we didn't find one, add a custom frame rate for the current value
                        if (!found) {
                            currentIndex = addRefreshRateOrdered(model, saved_fps, qsTr("Custom (%1 FPS)").arg(saved_fps), true)
                        }
                        else {
                            addRefreshRateOrdered(model, "", qsTr("Custom"), true)
                        }

                        recalculateWidth()

                        lastIndexValue = currentIndex
                    }

                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        reinitialize()
                        basicPage.languageChanged.connect(reinitialize)
                    }

                    model: ListModel {
                        id: fpsListModel
                        // Other elements may be added at runtime
                        ListElement {
                            text: qsTr("30 FPS")
                            video_fps: "30"
                            is_custom: false
                        }
                        ListElement {
                            text: qsTr("60 FPS")
                            video_fps: "60"
                            is_custom: false
                        }
                    }

                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        if (model.get(currentIndex).is_custom) {
                            customFpsDialog.open()
                        }
                        else {
                            updateBitrateForSelection()
                        }
                    }
                }
            }
        }

        ChoiceRow {
            title: qsTranslate("LegacySettingsPage", "Video codec")
            selectedValue: StreamingPreferences.videoCodecConfig
            onValueActivated: function(value) { StreamingPreferences.videoCodecConfig = value }

            model: ListModel {
                ListElement { text: qsTranslate("LegacySettingsPage", "Automatic (Recommended)"); val: StreamingPreferences.VCC_AUTO }
                ListElement { text: qsTranslate("LegacySettingsPage", "H.264"); val: StreamingPreferences.VCC_FORCE_H264 }
                ListElement { text: qsTranslate("LegacySettingsPage", "HEVC (H.265)"); val: StreamingPreferences.VCC_FORCE_HEVC }
                ListElement { text: qsTranslate("LegacySettingsPage", "AV1"); val: StreamingPreferences.VCC_FORCE_AV1 }
            }
        }

    }

    SettingsCard {
        title: qsTr("Network and stability")
        subtitle: qsTr("These settings apply to the next stream. Bitrate adjustment and packet loss protection can be used independently.")

        ToggleRow {
            title: qsTr("Automatically adjust bitrate (ABR)")
            description: qsTr("Allows a compatible Sunshine host to lower the video bitrate when the network is congested, up to the limit below.")
            checked: StreamingPreferences.enableSunshineAbr
            onToggled: function(value) { StreamingPreferences.enableSunshineAbr = value }
        }

        SettingsRow {
            id: bitrateRow
            title: StreamingPreferences.enableSunshineAbr ? qsTr("Video bitrate limit") : qsTr("Video bitrate")
            description: (StreamingPreferences.enableSunshineAbr
                          ? qsTr("The highest video bitrate ABR may select. FEC adds extra bandwidth.")
                          : qsTr("Lower the bitrate on slower connections. Raise the bitrate to increase image quality.")) + "\n" +
                         (StreamingPreferences.autoAdjustBitrate
                          ? qsTr("Recommended bitrate follows resolution and frame rate. Entering a value keeps it fixed.")
                          : qsTr("Your selected value is kept when resolution or frame rate changes."))

            Row {
                spacing: Theme.spaceSm
                SpinBox {
                    id: bitrateInput
                    width: 150
                    from: basicPage.bitrateMinKbps
                    to: basicPage.bitrateMaxKbps
                    stepSize: 500
                    editable: true
                    value: StreamingPreferences.bitrateKbps
                    Accessible.name: bitrateRow.title
                    Accessible.description: bitrateRow.description
                    validator: DoubleValidator {
                        bottom: bitrateInput.from / 1000
                        top: bitrateInput.to / 1000
                        decimals: 3
                        notation: DoubleValidator.StandardNotation
                        locale: bitrateInput.locale.name
                    }
                    textFromValue: function(value, locale) {
                        var decimals = value % 1000 === 0 ? 0 : value % 100 === 0 ? 1 : value % 10 === 0 ? 2 : 3
                        return Number(value / 1000).toLocaleString(locale, 'f', decimals)
                    }
                    valueFromText: function(text, locale) {
                        return Math.round(Number.fromLocaleString(locale, text) * 1000)
                    }
                    onValueModified: {
                        StreamingPreferences.autoAdjustBitrate = false
                        StreamingPreferences.bitrateKbps = value
                    }
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Mbps"
                }
            }
        }

        // The slider is only a coarse input. Restoring preferences must not
        // round a bitrate entered in the numeric field to a slider step.
        Column {
            width: parent.width
            spacing: Theme.spaceSm
            HardSlider {
                id: bitrateSlider
                width: parent.width - Theme.spaceMd * 2
                anchors.horizontalCenter: parent.horizontalCenter
                value: bitrateToSliderValue(StreamingPreferences.bitrateKbps)
                stepSize: 1
                from: 0
                to: bitrateSliderMax
                snapMode: Slider.SnapOnRelease
                Accessible.name: bitrateRow.title
                Accessible.description: bitrateRow.description
                onMoved: {
                    StreamingPreferences.autoAdjustBitrate = false
                    StreamingPreferences.bitrateKbps = sliderValueToBitrate(value)
                }
            }
            HardButton {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceMd
                text: qsTr("Use recommended bitrate (%1 Mbps)").arg(defaultBitrate / 1000)
                readonly property int defaultBitrate: StreamingPreferences.getDefaultBitrate(
                    StreamingPreferences.width, StreamingPreferences.height,
                    StreamingPreferences.fps, StreamingPreferences.enableYUV444)
                enabled: !StreamingPreferences.autoAdjustBitrate || StreamingPreferences.bitrateKbps !== defaultBitrate
                onClicked: {
                    StreamingPreferences.autoAdjustBitrate = true
                    StreamingPreferences.bitrateKbps = defaultBitrate
                }
            }
        }

        SettingsRow {
            id: fecRow
            title: qsTr("Packet loss protection (FEC)")
            description: StreamingPreferences.fecPercentage === -2
                         ? qsTr("Use the host's FEC settings. Requires a compatible host; otherwise its default behavior is used.")
                         : StreamingPreferences.fecPercentage === -1
                           ? qsTr("Ask a compatible host to choose redundancy from network feedback. Otherwise its default behavior is used.")
                           : qsTr("Your fixed percentage takes priority over the host's FEC settings. 0% disables FEC; higher values add more bandwidth relative to video data. Requires a compatible host.")

            Row {
                spacing: Theme.spaceSm
                AutoResizingComboBox {
                    anchors.verticalCenter: parent.verticalCenter
                    model: [qsTr("Host default"), qsTr("Automatic"), qsTr("Fixed")]
                    currentIndex: StreamingPreferences.fecPercentage >= 0 ? 2 : StreamingPreferences.fecPercentage === -1 ? 1 : 0
                    Accessible.name: fecRow.title
                    Accessible.description: fecRow.description
                    onActivated: {
                        if (StreamingPreferences.fecPercentage >= 0)
                            StreamingPreferences.fixedFecPercentage = StreamingPreferences.fecPercentage
                        StreamingPreferences.fecPercentage = currentIndex === 0 ? -2 : currentIndex === 1 ? -1 : StreamingPreferences.fixedFecPercentage
                        StreamingPreferences.save()
                    }
                }
                SpinBox {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: StreamingPreferences.fecPercentage >= 0
                    from: 0
                    to: 100
                    editable: true
                    value: StreamingPreferences.fixedFecPercentage
                    Accessible.name: qsTr("Fixed FEC percentage")
                    Accessible.description: fecRow.description
                    onValueModified: {
                        StreamingPreferences.fixedFecPercentage = value
                        StreamingPreferences.fecPercentage = value
                        StreamingPreferences.save()
                    }
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: StreamingPreferences.fecPercentage >= 0
                    text: "%"
                }
            }
        }
    }

    // ================= 画质增强 =================
    SettingsCard {
        title: qsTr("Enhancements")

        ToggleRow {
            readonly property bool enhancementCapable: SystemProperties.isVideoEnhancementCapable()
            readonly property bool softwareDecoderForced:
                StreamingPreferences.videoDecoderSelection === StreamingPreferences.VDS_FORCE_SOFTWARE

            title: !enhancementCapable
                   ? qsTr("Video AI-Enhancement (Not supported by the GPU)")
                   : (SystemProperties.isVideoEnhancementExperimental()
                      ? qsTr("Video AI-Enhancement (Experimental)")
                      : qsTr("Video AI-Enhancement"))
            description: qsTr("Enhance video quality by utilizing the GPU's AI-Enhancement capabilities.") + "\n" +
                         qsTr("This feature effectively upscales, reduces compression artifacts and enhances the clarity of streamed content.") + "\n" +
                         qsTr("Note:") + "\n" +
                         qsTr("If available, ensure that appropriate settings (i.e. RTX Video enhancement) are enabled in your GPU driver configuration.") + "\n" +
                         qsTr("HDR rendering has diverse issues depending on the GPU used, we are working on it but we advise to currently use Non-HDR.") + "\n" +
                         qsTr("Be advised that using this feature on laptops running on battery power may lead to significant battery drain.")

            controlEnabled: enhancementCapable && !softwareDecoderForced
            checked: controlEnabled && StreamingPreferences.videoEnhancement
            onToggled: function(value) { StreamingPreferences.videoEnhancement = value }
        }

        SettingsRow {
            title: qsTr("Stream Resolution Scale")
            description: qsTr("Renders the stream below the selected resolution and upscales it on the client.")

            Row {
                spacing: Theme.spaceSm

                HardSwitch {
                    id: streamResolutionScaleSwitch
                    anchors.verticalCenter: parent.verticalCenter
                    checked: StreamingPreferences.streamResolutionScale
                    onToggled: {
                        StreamingPreferences.streamResolutionScale = checked
                    }
                }

                HardTextField {
                    id: streamResolutionScaleRatioField
                    anchors.verticalCenter: parent.verticalCenter
                    maximumLength: 3
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator{bottom:20; top:100}
                    width: 64
                    visible: streamResolutionScaleSwitch.checked
                    text: StreamingPreferences.streamResolutionScaleRatio.toString()
                    onTextChanged: {
                        var value = parseInt(text);
                        if (!isNaN(value) && value >= 20 && value <= 100) {
                            StreamingPreferences.streamResolutionScaleRatio = value;
                        }
                    }
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "%"
                    font.bold: true
                    visible: streamResolutionScaleSwitch.checked
                }
            }
        }
    }

    // ================= 远程覆盖 =================
    SettingsCard {
        title: qsTr("Remote overrides")
        subtitle: qsTr("Used instead of the values above when streaming over the internet.")

        SettingsRow {
            title: qsTr("Remote Resolution")

            Row {
                spacing: Theme.spaceSm

                HardSwitch {
                    id: remoteResolutionSwitch
                    anchors.verticalCenter: parent.verticalCenter
                    checked: StreamingPreferences.remoteResolution
                    onToggled: {
                        StreamingPreferences.remoteResolution = checked
                    }
                }

                HardTextField {
                    id: remoteResolutionWidthField
                    anchors.verticalCenter: parent.verticalCenter
                    maximumLength: 4
                    inputMethodHints: Qt.ImhDigitsOnly
                    placeholderText: "1280"
                    validator: IntValidator{bottom:256; top:8192}
                    width: 96
                    visible: remoteResolutionSwitch.checked
                    text: StreamingPreferences.remoteResolutionWidth > 0 ? StreamingPreferences.remoteResolutionWidth.toString() : ""
                    onEditingFinished: {
                        var value = parseInt(text);
                        if (!isNaN(value)) {
                            StreamingPreferences.remoteResolutionWidth = value;
                        }
                    }
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "x"
                    font.bold: true
                    visible: remoteResolutionSwitch.checked
                }

                HardTextField {
                    id: remoteResolutionHeightField
                    anchors.verticalCenter: parent.verticalCenter
                    maximumLength: 4
                    inputMethodHints: Qt.ImhDigitsOnly
                    placeholderText: "720"
                    validator: IntValidator{bottom:256; top:8192}
                    width: 96
                    visible: remoteResolutionSwitch.checked
                    text: StreamingPreferences.remoteResolutionHeight > 0 ? StreamingPreferences.remoteResolutionHeight.toString() : ""
                    onEditingFinished: {
                        var value = parseInt(text);
                        if (!isNaN(value)) {
                            StreamingPreferences.remoteResolutionHeight = value;
                        }
                    }
                }
            }
        }

        SettingsRow {
            title: qsTr("Remote Frame Rate")

            Row {
                spacing: Theme.spaceSm

                HardSwitch {
                    id: remoteFpsSwitch
                    anchors.verticalCenter: parent.verticalCenter
                    checked: StreamingPreferences.remoteFps
                    onToggled: {
                        StreamingPreferences.remoteFps = checked
                    }
                }

                HardTextField {
                    id: remoteFpsRateField
                    anchors.verticalCenter: parent.verticalCenter
                    maximumLength: 3
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator{bottom:1; top:512}
                    placeholderText: "60"
                    width: 80
                    visible: remoteFpsSwitch.checked
                    text: StreamingPreferences.remoteFpsRate > 0 ? StreamingPreferences.remoteFpsRate.toString() : ""
                    onEditingFinished: {
                        var value = parseInt(text);
                        if (!isNaN(value)) {
                            StreamingPreferences.remoteFpsRate = value;
                        }
                    }
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("FPS")
                    font.bold: true
                    visible: remoteFpsSwitch.checked
                }
            }
        }
    }
}
