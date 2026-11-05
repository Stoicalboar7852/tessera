import QtQuick 2.9
import QtQuick.Controls 2.2

import TesseraDisplaySettings 1.0

// Tessera: the "Displays" section of the settings page (docs/phase-3-design.md, component 4, and its addendum).
// A picture of the client's monitors as the OS arranges them; clicking one shows its resolution, frame rate and
// bitrate. Hidden when the client has one monitor.
GroupBox {
    id: tesseraDisplaysGroupBox
    width: (parent.width - (parent.leftPadding + parent.rightPadding))
    padding: 12
    title: "<font color=\"skyblue\">" + qsTr("Displays") + "</font>"
    font.pointSize: 12
    visible: TesseraDisplaySettings.monitors.length > 1

    // The monitor whose settings are shown, as an index into TesseraDisplaySettings.monitors
    property int selectedIndex: 0
    readonly property var selected: selectedIndex < TesseraDisplaySettings.monitors.length ?
                                        TesseraDisplaySettings.monitors[selectedIndex] : null
    readonly property bool multiDisplayMode: TesseraDisplaySettings.mode !== 0

    Component.onCompleted: {
        TesseraDisplaySettings.refreshMonitors()

        // Start on the main display
        for (var i = 0; i < TesseraDisplaySettings.monitors.length; i++) {
            if (TesseraDisplaySettings.monitors[i].primary) {
                selectedIndex = i
                break
            }
        }
    }

    function megabits(kbps) {
        return kbps / 1000.0
    }

    Column {
        anchors.fill: parent
        spacing: 5

        Label {
            width: parent.width
            text: qsTr("Displays to stream")
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        Label {
            width: parent.width
            text: qsTr("With a Tessera host, each monitor gets its own host display. Other hosts stream the main display only.")
            font.pointSize: 9
            wrapMode: Text.Wrap
        }

        AutoResizingComboBox {
            id: tesseraModeComboBox
            // Indexes match DisplaySettings::Mode
            model: [qsTr("This display"), qsTr("All displays"), qsTr("Chosen displays")]
            currentIndex: TesseraDisplaySettings.mode
            Component.onCompleted: recalculateWidth()
            onActivated: {
                TesseraDisplaySettings.mode = currentIndex
            }
        }

        Label {
            width: parent.width
            visible: multiDisplayMode
            text: qsTr("Click a display to change how it streams. Up to 4 displays stream; the main display is display 1 when it streams.")
            font.pointSize: 9
            wrapMode: Text.Wrap
        }

        // The monitors as the OS arranges them, scaled to fit
        Item {
            id: layoutPicture
            visible: multiDisplayMode
            width: parent.width

            readonly property var bounds: {
                var monitors = TesseraDisplaySettings.monitors
                if (monitors.length === 0) {
                    return { x: 0, y: 0, w: 1, h: 1 }
                }
                var left = monitors[0].x, top = monitors[0].y
                var right = left + monitors[0].w, bottom = top + monitors[0].h
                for (var i = 1; i < monitors.length; i++) {
                    left = Math.min(left, monitors[i].x)
                    top = Math.min(top, monitors[i].y)
                    right = Math.max(right, monitors[i].x + monitors[i].w)
                    bottom = Math.max(bottom, monitors[i].y + monitors[i].h)
                }
                return { x: left, y: top, w: Math.max(right - left, 1), h: Math.max(bottom - top, 1) }
            }
            readonly property real pictureScale: Math.min(width / bounds.w, 180 / bounds.h)
            readonly property real offsetX: (width - bounds.w * pictureScale) / 2

            height: bounds.h * pictureScale

            Repeater {
                model: TesseraDisplaySettings.monitors

                Rectangle {
                    // A small gap between touching monitors
                    x: layoutPicture.offsetX + (modelData.x - layoutPicture.bounds.x) * layoutPicture.pictureScale + 2
                    y: (modelData.y - layoutPicture.bounds.y) * layoutPicture.pictureScale + 2
                    width: Math.max(modelData.w * layoutPicture.pictureScale - 4, 1)
                    height: Math.max(modelData.h * layoutPicture.pictureScale - 4, 1)
                    radius: 3
                    color: modelData.streamed ? "#2b4f6e" : "#303030"
                    border.color: index === selectedIndex ? "skyblue" : "#808080"
                    border.width: index === selectedIndex ? 3 : 1

                    Label {
                        anchors.fill: parent
                        anchors.margins: 4
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.Wrap
                        elide: Text.ElideRight
                        font.pointSize: 9
                        color: modelData.streamed ? "white" : "#a0a0a0"
                        text: modelData.name + (modelData.primary ? "\n" + qsTr("Main display") : "") +
                              "\n" + (modelData.streamed ? modelData.width + " × " + modelData.height
                                                         : qsTr("Not streamed"))
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            selectedIndex = index
                        }
                    }
                }
            }
        }

        // The selected monitor's settings
        Label {
            width: parent.width
            visible: multiDisplayMode && selected !== null
            text: selected !== null ? selected.name + (selected.primary ? " (" + qsTr("main display") + ")" : "") : ""
            font.pointSize: 12
            font.bold: true
            wrapMode: Text.Wrap
        }

        CheckBox {
            id: tesseraChosenCheckBox
            visible: TesseraDisplaySettings.mode === 2 && selected !== null
            text: qsTr("Stream this display")
            font.pointSize: 12
            checked: selected !== null && selected.chosen
            onToggled: {
                TesseraDisplaySettings.setChosen(selected.key, checked)
            }
        }

        Label {
            width: parent.width
            visible: multiDisplayMode
            text: qsTr("Resolution")
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        AutoResizingComboBox {
            id: tesseraSizeComboBox
            visible: multiDisplayMode
            hoverEnabled: true
            model: selected !== null ? selected.sizeOptions : []
            currentIndex: selected !== null ? selected.sizeIndex : 0
            // Not while the combo box is being created (upstream's width binding needs its indicator)
            onModelChanged: if (indicator) recalculateWidth()
            onCountChanged: if (indicator) recalculateWidth()
            Component.onCompleted: recalculateWidth()
            onActivated: {
                TesseraDisplaySettings.setSize(selected.key, currentIndex)
            }

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Desktop size streams a scaled (Retina or 4K) monitor at its desktop size and upscales it, which lowers the load on the host's encoder and the network")
        }

        Label {
            width: parent.width
            visible: multiDisplayMode
            text: qsTr("Frame rate")
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        AutoResizingComboBox {
            id: tesseraFrameRateComboBox
            visible: multiDisplayMode
            model: selected !== null ? selected.frameRateOptions : []
            currentIndex: selected !== null ? selected.frameRateIndex : 0
            // Not while the combo box is being created (upstream's width binding needs its indicator)
            onModelChanged: if (indicator) recalculateWidth()
            onCountChanged: if (indicator) recalculateWidth()
            Component.onCompleted: recalculateWidth()
            onActivated: {
                TesseraDisplaySettings.setFrameRate(selected.key, currentIndex)
            }
        }

        Label {
            width: parent.width
            visible: multiDisplayMode
            text: qsTr("Video bitrate: %1 Mbps").arg(megabits(tesseraBitrateSlider.value)) +
                  (selected !== null && selected.autoBitrate ? " (" + qsTr("automatic") + ")" : "")
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        Row {
            width: parent.width
            spacing: 5
            visible: multiDisplayMode

            Slider {
                id: tesseraBitrateSlider
                // While dragging the slider shows its own value; the setting is saved on release
                value: selected !== null ? selected.bitrateKbps : 500
                stepSize: 500
                from: 500
                to: TesseraDisplaySettings.maximumBitrateKbps
                snapMode: "SnapOnRelease"
                width: Math.min(300, parent.width - (tesseraAutoBitrateButton.visible ? tesseraAutoBitrateButton.width + parent.spacing : 0))

                function save() {
                    if (selected !== null) {
                        TesseraDisplaySettings.setBitrate(selected.key, Math.round(value / 500) * 500)
                    }
                }

                onPressedChanged: {
                    if (!pressed) {
                        save()
                    }
                }
                onMoved: {
                    // Keyboard changes
                    if (!pressed) {
                        save()
                    }
                }
            }

            Button {
                id: tesseraAutoBitrateButton
                visible: selected !== null && !selected.autoBitrate
                text: qsTr("Automatic (%1 Mbps)").arg(selected !== null ? megabits(selected.defaultBitrateKbps) : 0)
                onClicked: {
                    TesseraDisplaySettings.setBitrate(selected.key, 0)
                }
            }
        }

        Label {
            width: parent.width
            visible: TesseraDisplaySettings.active
            text: qsTr("Total for %1 displays: %2 Mbps").arg(TesseraDisplaySettings.streamCount)
                                                       .arg(megabits(TesseraDisplaySettings.totalBitrateKbps))
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        Label {
            width: parent.width
            visible: multiDisplayMode && !TesseraDisplaySettings.active
            text: TesseraDisplaySettings.mode === 2 ? qsTr("Choose at least two displays to stream several.")
                                                    : qsTr("Fewer than two displays are connected: streaming one display.")
            font.pointSize: 9
            wrapMode: Text.Wrap
        }
    }
}
