import QtQuick 2.9
import QtQuick.Controls 2.2

import TesseraDisplaySettings 1.0

// Tessera: the "Displays" section of the settings page (docs/phase-3-design.md, component 4).
// Hidden when the client has one monitor.
GroupBox {
    id: tesseraDisplaysGroupBox
    width: (parent.width - (parent.leftPadding + parent.rightPadding))
    padding: 12
    title: "<font color=\"skyblue\">" + qsTr("Displays") + "</font>"
    font.pointSize: 12
    visible: TesseraDisplaySettings.monitors.length > 1

    Component.onCompleted: {
        TesseraDisplaySettings.refreshMonitors()
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
            text: qsTr("With a Tessera host, each monitor gets its own host display at the monitor's resolution and refresh rate. Other hosts stream this display only.")
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

        Repeater {
            model: TesseraDisplaySettings.monitors

            CheckBox {
                width: parent.width
                visible: TesseraDisplaySettings.mode === 2
                text: modelData.name + " (" + modelData.nativeSize + ", " + modelData.refresh +
                      (modelData.hdr ? ", HDR" : "") + ")" + (modelData.primary ? " - " + qsTr("main display") : "")
                font.pointSize: 12
                checked: modelData.chosen
                onToggled: {
                    TesseraDisplaySettings.setChosen(modelData.key, checked)
                }
            }
        }

        Label {
            width: parent.width
            visible: TesseraDisplaySettings.mode !== 0
            text: qsTr("Up to 4 displays. Stream 0 goes to the main display if it is streamed.")
            font.pointSize: 9
            wrapMode: Text.Wrap
        }

        Label {
            width: parent.width
            visible: TesseraDisplaySettings.mode !== 0
            text: qsTr("Resolution of each display")
            font.pointSize: 12
            wrapMode: Text.Wrap
        }

        AutoResizingComboBox {
            id: tesseraResolutionComboBox
            visible: TesseraDisplaySettings.mode !== 0
            hoverEnabled: true
            // Indexes match DisplaySettings::Resolution
            model: [qsTr("Native"), qsTr("Desktop size")]
            currentIndex: TesseraDisplaySettings.resolution
            Component.onCompleted: recalculateWidth()
            onActivated: {
                TesseraDisplaySettings.resolution = currentIndex
            }

            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Desktop size streams scaled (Retina or 4K) monitors at their desktop size and upscales them, which lowers the load on the host's encoder and the network")
        }
    }
}
