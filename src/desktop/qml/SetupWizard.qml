import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    objectName: "setupWizard"
    width: Math.max(420, Math.min(780, parent ? parent.width - 32 : 780))
    height: Math.max(520, Math.min(680, parent ? parent.height - 32 : 680))
    modal: true
    closePolicy: Popup.NoAutoClose
    title: "Station setup — " + appSettings.profileName
    property int step: 0
    property bool manualRadioSetup: appSettings.radioEnabled
    onStepChanged: pageFlick.contentY = 0
    Component.onCompleted: appSettings.refreshDetectedRadios()

    function goForward() {
        if (root.step === 1 && !appSettings.radioEnabled)
            root.step = 4
        else
            root.step++
    }

    function goBack() {
        if (root.step === 4 && !appSettings.radioEnabled)
            root.step = 1
        else
            root.step--
    }

    function frequencyProviderSummary() {
        if (!appSettings.radioEnabled)
            return "Frequency control: skipped in SWL mode"
        if (appSettings.frequencyBackendIndex === 0)
            return "Frequency control: OmniRig slot " + appSettings.omniRigSlot
        if (appSettings.frequencyBackendIndex === 1)
            return "Frequency control: rigctld " + appSettings.hamlibHost + ":" + appSettings.hamlibPort
        return "Frequency control: CAT4OM "
                + (appSettings.cat4omRadioId.length > 0
                   ? "radio " + appSettings.cat4omRadioId
                   : appSettings.cat4omUrl)
    }

    footer: Rectangle {
        implicitHeight: footerLayout.implicitHeight + 24
        color: "#151b23"
        border.color: "#2b3541"

        RowLayout {
            id: footerLayout
            anchors.fill: parent
            anchors.margins: 12
            Label { text: appSettings.statusMessage; color: "#91a0b1"; Layout.fillWidth: true; elide: Text.ElideRight }
            Button {
                text: "Cancel"; visible: appSettings.setupComplete
                onClicked: { root.step = 0; root.close() }
                ToolTip.visible: hovered
                ToolTip.text: "Close setup without applying these edits"
            }
            Button {
                text: "Back"; enabled: root.step > 0; onClicked: root.goBack()
                ToolTip.visible: hovered
                ToolTip.text: "Return to the previous setup page"
            }
            Button {
                objectName: "setupNextButton"
                text: root.step < 5 ? "Next" : "Finish"
                highlighted: true
                ToolTip.visible: hovered
                ToolTip.text: root.step < 5
                    ? "Continue to the next setup page"
                    : "Validate and save this station profile"
                onClicked: {
                    if (root.step < 5)
                        root.goForward()
                    else if (appSettings.completeSetup()) {
                        root.step = 0
                        root.close()
                    }
                }
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: ["Receiver", "Audio", "CAT", "Keying", "Display", "Review"]
                delegate: Label {
                    required property string modelData
                    required property int index
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: modelData
                    color: index <= root.step ? "#43c6ac" : "#667789"
                    font.weight: index === root.step ? Font.Bold : Font.Normal
                }
            }
        }
        ProgressBar { Layout.fillWidth: true; from: 0; to: 5; value: root.step }

        Flickable {
            id: pageFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: width
            contentHeight: Math.max(height, pageStack.implicitHeight)
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            StackLayout {
                id: pageStack
                currentIndex: root.step
                width: Math.max(0, pageFlick.width - 14)
                height: Math.max(pageFlick.height, implicitHeight)

                ColumnLayout {
                spacing: 16
                Label { text: "Choose the receiver setup"; font.pixelSize: 22; font.weight: Font.DemiBold }
                RadioButton {
                    text: "No radio — receive-only audio decoding (SWL)"
                    checked: !appSettings.radioEnabled
                    onClicked: {
                        appSettings.radioEnabled = false
                        root.manualRadioSetup = false
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#91a0b1"
                    text: "SWL mode skips CAT and direct key/PTT setup. It never opens a radio port and cannot transmit."
                }
                Label { text: "Detected online radios"; font.weight: Font.DemiBold }
                ComboBox {
                    Layout.fillWidth: true
                    model: appSettings.detectedRadioNames
                    enabled: count > 0
                    currentIndex: appSettings.detectedRadioIndex
                    displayText: count > 0 ? currentText : "No positively identified online radio found"
                    onActivated: {
                        appSettings.selectDetectedRadio(currentIndex)
                        root.manualRadioSetup = false
                    }
                }
                RowLayout {
                    Button {
                        text: "Refresh detection"; onClicked: appSettings.refreshDetectedRadios()
                        ToolTip.visible: hovered
                        ToolTip.text: "Ask configured integrations for positively identified online radios"
                    }
                    Button {
                        text: root.manualRadioSetup ? "Hide manual setup" : "Set up a radio manually"
                        checkable: true
                        checked: root.manualRadioSetup
                        ToolTip.visible: hovered
                        ToolTip.text: checked
                            ? "Hide the manual radio template"
                            : "Configure a radio without automatic identification"
                        onClicked: {
                            root.manualRadioSetup = checked
                            if (checked)
                                appSettings.radioEnabled = true
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#91a0b1"
                    visible: root.manualRadioSetup
                    text: "Manual radio template (all CAT values remain editable)"
                }
                ComboBox {
                    Layout.fillWidth: true
                    visible: root.manualRadioSetup
                    model: appSettings.referenceRigNames
                    currentIndex: appSettings.referenceRigIndex
                    onActivated: appSettings.selectReferenceRig(currentIndex)
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#91a0b1"
                    text: "Serial ports are not presented as radios: a port name cannot safely identify the connected model. Detection uses an installed integration and lists only radios reporting an online state."
                }
                Item { Layout.fillHeight: true }
            }

                GridLayout {
                columns: 2
                columnSpacing: 18
                rowSpacing: 12
                Label {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Select the sound-card input carrying receiver audio. This is required for both radio and receive-only SWL operation."
                    color: "#91a0b1"
                }
                Label { text: "Audio input" }
                ComboBox {
                    objectName: "setupAudioInputCombo"
                    Layout.fillWidth: true
                    model: appSettings.audioInputNames
                    currentIndex: appSettings.audioInputIndex
                    onActivated: appSettings.selectAudioInput(currentIndex)
                }
                Label { text: "" }
                Label {
                    objectName: "setupAudioInputAmbiguityNote"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: appSettings.audioInputNamesAmbiguous
                    color: "#e0b341"
                    text: "Two or more inputs report the same name, so they are numbered #1, #2 … in the order the operating system lists them. If reception starts on the wrong radio, pick the other number."
                }
                Label { text: "Selected input" }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: appSettings.audioInputDisplayName
                    color: "#43c6ac"
                }
                Label { text: "Radio audio association" }
                CheckBox {
                    text: "This input carries RX audio from this radio"
                    checked: appSettings.audioInputRadioLinked
                    enabled: appSettings.radioEnabled
                    onToggled: appSettings.audioInputRadioLinked = checked
                }
                Label { text: "" }
                Button {
                    text: "Refresh audio inputs"; onClicked: appSettings.refreshAudioInputs()
                    ToolTip.visible: hovered
                    ToolTip.text: "Rescan operating-system audio capture devices"
                }
                Label {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#f3bd55"
                    text: "Live RX uses 48 kHz mono float capture when the device supports it, otherwise its preferred PCM format is downmixed safely. Advanced channel, rate, buffer, and level controls remain planned."
                }
            }

                GridLayout {
                columns: 2
                columnSpacing: 18
                rowSpacing: 12
                Label { text: "Frequency provider" }
                ComboBox { Layout.fillWidth: true; model: ["OmniRig (Windows)", "Hamlib", "CAT4OM network service"]; currentIndex: appSettings.frequencyBackendIndex; onActivated: appSettings.frequencyBackendIndex = currentIndex }
                Label { text: "OmniRig slot"; visible: appSettings.frequencyBackendIndex === 0 }
                RowLayout {
                    Layout.fillWidth: true
                    visible: appSettings.frequencyBackendIndex === 0
                    SpinBox { from: 1; to: 2; value: appSettings.omniRigSlot; onValueModified: appSettings.omniRigSlot = value }
                    Button {
                        text: "Configure OmniRig"
                        enabled: appSettings.omniRigAvailable
                        onClicked: appSettings.showOmniRigConfiguration()
                        ToolTip.visible: hovered
                        ToolTip.text: enabled
                            ? "Open OmniRig to configure its radio model, COM port, baud rate, parity, and stop bits"
                            : "OmniRig is not available on this system"
                    }
                }
                Label { text: "Hamlib rigctld endpoint"; visible: appSettings.frequencyBackendIndex === 1 }
                RowLayout {
                    Layout.fillWidth: true
                    visible: appSettings.frequencyBackendIndex === 1
                    TextField { Layout.fillWidth: true; text: appSettings.hamlibHost; placeholderText: "127.0.0.1"; onEditingFinished: appSettings.hamlibHost = text }
                    SpinBox { editable: true; from: 1; to: 65535; value: appSettings.hamlibPort; onValueModified: appSettings.hamlibPort = value }
                }
                Label { text: "Hamlib VFO mapping"; visible: appSettings.frequencyBackendIndex === 1 }
                RowLayout {
                    Layout.fillWidth: true
                    visible: appSettings.frequencyBackendIndex === 1
                    TextField { Layout.fillWidth: true; text: appSettings.hamlibRxVfo; placeholderText: "VFOA (RX)"; onEditingFinished: appSettings.hamlibRxVfo = text }
                    TextField { Layout.fillWidth: true; text: appSettings.hamlibTxVfo; placeholderText: "VFOB (TX)"; onEditingFinished: appSettings.hamlibTxVfo = text }
                    CheckBox { text: "Allow writes"; checked: appSettings.hamlibWritable; onToggled: appSettings.hamlibWritable = checked }
                }
                Label { text: "CAT4OM Control URL"; visible: appSettings.frequencyBackendIndex === 2 }
                TextField { Layout.fillWidth: true; visible: appSettings.frequencyBackendIndex === 2; text: appSettings.cat4omUrl; placeholderText: "ws://127.0.0.1:5001/"; onEditingFinished: appSettings.cat4omUrl = text }
                Label { text: "CAT4OM radio ID"; visible: appSettings.frequencyBackendIndex === 2 }
                TextField { Layout.fillWidth: true; visible: appSettings.frequencyBackendIndex === 2; text: appSettings.cat4omRadioId; placeholderText: "Optional"; onEditingFinished: appSettings.cat4omRadioId = text }
                Label { text: "Split" }
                CheckBox { text: "Independent TX frequency"; checked: appSettings.splitEnabled; onToggled: appSettings.splitEnabled = checked }
                Label { text: "RX transverter offset (Hz)" }
                TextField { Layout.fillWidth: true; text: appSettings.rxTransverterOffsetHz.toString(); onEditingFinished: appSettings.rxTransverterOffsetHz = Number(text) }
                Label { text: "TX transverter offset (Hz)" }
                TextField { Layout.fillWidth: true; text: appSettings.txTransverterOffsetHz.toString(); onEditingFinished: appSettings.txTransverterOffsetHz = Number(text) }
                Label { text: "CW audio-to-RF mapping" }
                ComboBox {
                    Layout.fillWidth: true
                    model: ["CW-U / USB (+)", "CW-L / LSB (-)"]
                    currentIndex: appSettings.cwToneSidebandIndex
                    onActivated: appSettings.cwToneSidebandIndex = currentIndex
                }
                Label { text: "" }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#91a0b1"
                    text: appSettings.frequencyBackendIndex === 0
                          ? "OmniRig owns its COM port and serial framing; configure those values in OmniRig."
                          : appSettings.frequencyBackendIndex === 1
                            ? "rigctld owns its radio connection and serial framing; CW Buddy needs only this loopback endpoint and VFO mapping."
                            : "CAT4OM owns its radio connection and serial framing; CW Buddy needs only its Control service identity."
                }
            }

                GridLayout {
                columns: 2
                columnSpacing: 18
                rowSpacing: 12
                Label { Layout.columnSpan: 2; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#f3bd55"; text: "Select the dedicated direct-COM interface. Port discovery is passive and does not toggle either line." }
                Label { text: "Hardware keying" }
                CheckBox {
                    text: "Enable direct RTS/DTR keying"
                    checked: appSettings.directKeyingEnabled
                    onToggled: appSettings.directKeyingEnabled = checked
                    ToolTip.visible: hovered
                    ToolTip.text: "Opt in only after reading the disconnected-line and loopback test procedure"
                }
                Label { text: "Key/PTT COM port" }
                ComboBox { Layout.fillWidth: true; editable: true; model: appSettings.serialPorts; currentIndex: find(appSettings.keyingPort); displayText: currentIndex >= 0 ? currentText : appSettings.keyingPort; onActivated: appSettings.keyingPort = currentText; onAccepted: appSettings.keyingPort = editText }
                Label { text: "PTT" }
                RowLayout {
                    ComboBox { model: ["RTS", "DTR"]; currentIndex: appSettings.pttLineIndex; onActivated: appSettings.pttLineIndex = currentIndex }
                    CheckBox { text: "Active high"; checked: appSettings.pttActiveHigh; onToggled: appSettings.pttActiveHigh = checked }
                }
                Label { text: "KEY" }
                RowLayout {
                    ComboBox { model: ["RTS", "DTR"]; currentIndex: appSettings.keyLineIndex; onActivated: appSettings.keyLineIndex = currentIndex }
                    CheckBox { text: "Active high"; checked: appSettings.keyActiveHigh; onToggled: appSettings.keyActiveHigh = checked }
                }
                Label { text: "Hardware validation" }
                ColumnLayout {
                    Layout.fillWidth: true
                    enabled: appSettings.directKeyingEnabled
                             && appSettings.keyingPort.length > 0
                    CheckBox {
                        id: setupRadioDisconnectedForLoopback
                        objectName: "setupRadioDisconnectedForLoopbackCheck"
                        text: "Radio disconnected; RTS→CTS and DTR→DSR loopbacks fitted"
                    }
                    Button {
                        objectName: "setupRunDirectKeyingLoopbackButton"
                        text: "Run measured loopback"
                        enabled: setupRadioDisconnectedForLoopback.checked
                        onClicked: {
                            appSettings.runDirectKeyingLoopback(
                                setupRadioDisconnectedForLoopback.checked)
                            setupRadioDisconnectedForLoopback.checked = false
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Measure both serial loopback paths before CW Buddy can open this keying configuration"
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: appSettings.directKeyingValidated
                               ? "#43c6ac" : "#f3bd55"
                        text: appSettings.directKeyingAcceptanceStatus
                    }
                }
                Label { text: "TX speed" }
                ComboBox {
                    model: ["Match selected RX", "Fixed"]
                    currentIndex: appSettings.txSpeedMode
                    onActivated: appSettings.txSpeedMode = currentIndex
                    ToolTip.visible: hovered
                    ToolTip.text: "RX matching snapshots a supported selected-stream speed; fixed WPM is the safe fallback"
                }
                Label { text: "Fixed/fallback WPM" }
                LabeledSlider {
                    Layout.fillWidth: true
                    caption: "WPM"
                    from: 5
                    to: 80
                    stepSize: 1
                    value: appSettings.fixedTxWpm
                    onMoved: value => appSettings.fixedTxWpm = Math.round(value)
                }
                Label { Layout.columnSpan: 2; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#91a0b1"; text: "Active-high interfaces only in this first hardware slice. Validate the inactive lines with the radio disconnected, then use physical loopback before a minimum-power dummy-load test. Every reconnect remains disarmed." }
            }

                GridLayout {
                columns: 2
                columnSpacing: 18
                rowSpacing: 12
                Label { text: "Spectrum target FPS" }
                SpinBox { from: 10; to: 120; value: appSettings.targetFps; onValueModified: appSettings.targetFps = value }
                Label { text: "Waterfall lines / second" }
                SpinBox { from: 1; to: 120; value: appSettings.waterfallRate; onValueModified: appSettings.waterfallRate = value }
                Label { text: "Waterfall history (seconds)" }
                SpinBox { from: 5; to: 30; value: appSettings.waterfallTimeSpanSeconds; onValueModified: appSettings.waterfallTimeSpanSeconds = value }
                Label { text: "Visual CW reference" }
                RowLayout {
                    CheckBox { text: "Show"; checked: appSettings.showCwGuide; onToggled: appSettings.showCwGuide = checked }
                    SpinBox { editable: true; from: 0; to: 96000; value: appSettings.cwGuideCenterHz; enabled: appSettings.showCwGuide; onValueModified: appSettings.cwGuideCenterHz = value }
                    Label { text: "Hz" }
                }
                Label { text: "Range" }
                CheckBox { text: "Automatic display scaling"; checked: appSettings.automaticRange; onToggled: appSettings.automaticRange = checked }
                Label { Layout.columnSpan: 2; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#91a0b1"; text: "These are starting values. The full Settings pane provides independent visualization controls and future startup calibration recommendations." }
            }

                ColumnLayout {
                spacing: 14
                Label { text: "Ready to create this station profile"; font.pixelSize: 22; font.weight: Font.DemiBold }
                Label { text: "Profile: " + appSettings.profileName }
                Label { text: "Audio input: " + appSettings.audioInputDisplayName }
                Label { text: "Radio: " + appSettings.radioDisplayName }
                Label { text: root.frequencyProviderSummary() }
                Label { text: appSettings.radioEnabled ? "Direct key/PTT: " + (appSettings.keyingPort || "not selected") : "Direct key/PTT: disabled in SWL mode" }
                Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#f3bd55"; text: "Finishing saves configuration only. It does not open ports, arm the transmitter, or send a test signal." }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
}
