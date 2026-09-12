import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Pane {
    id: root
    signal done()
    signal setupRequested()
    property bool sdrDiscoveryRequested: false
    // True from the moment a device scan is asked for until the settings
    // object publishes a result. It is deliberately driven by the request and
    // by the completion notification rather than by an assumed duration, so it
    // never claims progress it cannot observe.
    // Reported by the application rather than assumed here. Enumeration runs on
    // a pooled thread now, so this is true for exactly as long as the scan
    // actually takes and the waiting indicator can animate while it does.
    readonly property bool sdrDiscoveryRunning: appSettings.sdrDiscoveryRunning

    function formatFrequencyKhz(frequencyHz) {
        return Number((Number(frequencyHz) / 1000).toFixed(3)).toString()
    }

    function parseFrequencyKhz(value) {
        var khz = Number(String(value).replace(",", "."))
        if (!Number.isFinite(khz) || khz <= 0 || khz > 99000000)
            return 0
        return Math.round(khz * 1000)
    }

    function requestInitialSdrDiscovery() {
        if (sdrDiscoveryRequested)
            return
        sdrDiscoveryRequested = true
        root.beginSdrDiscovery()
    }

    function beginSdrDiscovery() {
        if (sdrDiscoveryRunning)
            return
        // The application raises and clears the waiting state itself, and
        // refuses a second scan while one is in flight. Discovery remains
        // receive-only and does not open or start any device it finds.
        Qt.callLater(function() { appSettings.refreshSdrDevices() })
    }

    padding: 0
    background: Rectangle { color: "#151b23" }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Label { text: "Settings"; font.pixelSize: 22; font.weight: Font.DemiBold }
            Label { text: "Profile: " + appSettings.profileName; color: "#8290a0" }
            Item { Layout.fillWidth: true }
            ToolButton {
                text: "Close"; onClicked: root.done()
                ToolTip.visible: hovered
                ToolTip.text: "Close Settings; unapplied edits remain unsaved"
            }
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            onCurrentIndexChanged: {
                if (currentIndex === 1)
                    root.requestInitialSdrDiscovery()
            }
            TabButton { text: "Audio" }
            TabButton { text: "SDR" }
            TabButton { text: "Decoder" }
            TabButton { text: "Radio" }
            TabButton { text: "Cluster" }
            TabButton { text: "Network" }
            TabButton { text: "Keying" }
            TabButton { text: "Display" }
            TabButton { text: "Station" }
            TabButton { text: "About" }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 26
            Layout.rightMargin: 26
            Layout.bottomMargin: 8

            ScrollView {
                contentWidth: availableWidth
                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 12
                    anchors.margins: 22
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Choose the sound-card input carrying receiver audio. The selection is independent of radio control and is used in radio and SWL profiles."
                    }
                    Label { text: "Audio input" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: appSettings.audioInputNames
                        currentIndex: appSettings.audioInputIndex
                        onActivated: appSettings.selectAudioInput(currentIndex)
                    }
                    Label { text: "Selected input" }
                    Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: appSettings.audioInputDisplayName; color: "#43c6ac" }
                    Label { text: "Monitor output" }
                    ComboBox {
                        objectName: "audioMonitorOutputCombo"
                        Layout.fillWidth: true
                        model: appSettings.audioOutputNames
                        currentIndex: appSettings.audioOutputIndex
                        onActivated: appSettings.selectAudioOutput(currentIndex)
                        ToolTip.visible: hovered
                        ToolTip.text: "Device every monitor mode plays through: the receiver window (RX), one decoder card's stream, or REGION — the whole decode region at once, every signal in it heard together at the pitch its position in the region gives it. Region listening needs 48 kHz mono, which is why the decode region is capped at 24 kHz"
                    }
                    Label { text: "Selected output" }
                    Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: appSettings.audioOutputDisplayName; color: "#43c6ac" }
                    Label { text: "Radio audio association" }
                    CheckBox {
                        text: "This input carries RX audio from the configured radio"
                        checked: appSettings.audioInputRadioLinked
                        enabled: appSettings.radioEnabled
                        onToggled: appSettings.audioInputRadioLinked = checked
                    }
                    Label { text: "" }
                    Button {
                        text: "Refresh audio devices"
                        onClicked: {
                            appSettings.refreshAudioInputs()
                            appSettings.refreshAudioOutputs()
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Rescan operating-system audio input and monitor-output devices"
                    }
                    Label { text: "DC rejection" }
                    CheckBox {
                        objectName: "audioDcRejectionCheck"
                        text: "Remove input DC offset"
                        checked: appSettings.audioDcRejection
                        onToggled: appSettings.audioDcRejection = checked
                    }
                    Label { text: "Software input gain" }
                    CheckBox {
                        objectName: "audioAutomaticGainCheck"
                        text: "Automatic gain"
                        checked: appSettings.audioAutomaticGain
                        onToggled: appSettings.audioAutomaticGain = checked
                    }
                    Label { text: "Manual gain (dB)" }
                    SpinBox {
                        editable: true
                        from: -40
                        to: 40
                        value: Math.round(appSettings.audioGainDb)
                        enabled: !appSettings.audioAutomaticGain
                        onValueModified: appSettings.audioGainDb = value
                    }
                    Label { text: "Automatic target (dBFS)" }
                    SpinBox {
                        editable: true
                        from: -40
                        to: -1
                        value: Math.round(appSettings.audioAutomaticGainTargetDbfs)
                        enabled: appSettings.audioAutomaticGain
                        onValueModified: appSettings.audioAutomaticGainTargetDbfs = value
                    }
                    Label { text: "Processing bandwidth" }
                    CheckBox {
                        objectName: "audioAutomaticBandwidthCheck"
                        text: "Automatic from audio sample rate"
                        checked: appSettings.audioAutomaticBandwidth
                        onToggled: appSettings.audioAutomaticBandwidth = checked
                    }
                    Label { text: "Lower frequency (Hz)" }
                    SpinBox {
                        editable: true
                        from: 0
                        to: 95950
                        value: Math.round(appSettings.audioLowerFrequencyHz)
                        enabled: !appSettings.audioAutomaticBandwidth
                        onValueModified: appSettings.audioLowerFrequencyHz = value
                    }
                    Label { text: "Upper frequency (Hz)" }
                    SpinBox {
                        editable: true
                        from: 50
                        to: 96000
                        value: Math.round(appSettings.audioUpperFrequencyHz)
                        enabled: !appSettings.audioAutomaticBandwidth
                        onValueModified: appSettings.audioUpperFrequencyHz = value
                    }
                    Label { text: "" }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#f3bd55"
                        text: "DC rejection removes the persistent zero-frequency peak. Software gain is optional and does not alter the operating-system mixer; when automatic gain is disabled, the manual dB value is exact. Automatic bandwidth derives a 100–3000 Hz CW-oriented view from the input sample rate."
                    }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 12
                    anchors.margins: 22
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Select direct, receive-only SDR input for a much wider RF passband than sound-card audio. SDR reception never exposes transmit, PTT, or KEY control."
                    }
                    Label { text: "Receiver source" }
                    ComboBox {
                        objectName: "receiverInputTypeCombo"
                        Layout.fillWidth: true
                        model: appSettings.receiverInputTypeNames
                        currentIndex: appSettings.receiverInputTypeIndex
                        enabled: appSettings.sdrBackendAvailable
                                 && appSettings.sdrDeviceNames.length > 0
                        onActivated: appSettings.receiverInputTypeIndex = currentIndex
                        ToolTip.visible: hovered
                        ToolTip.text: appSettings.sdrBackendAvailable
                            ? "Choose conventional sound-card audio or a directly connected wide-passband SDR"
                            : "SDR is unavailable in this build; sound-card audio remains fully operational"
                    }
                    Label { text: "SoapySDR backend" }
                    Label {
                        objectName: "sdrBackendStateLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: appSettings.sdrBackendAvailable ? "#43c6ac" : "#f3bd55"
                        text: appSettings.sdrBackendAvailable
                            ? "Available" + (appSettings.sdrBackendVersion.length > 0
                                ? "  •  " + appSettings.sdrBackendVersion : "")
                            : "Unavailable — sound-card audio remains selected"
                    }
                    Label { text: "Installed modules" }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: appSettings.sdrModuleNames.length > 0
                            ? appSettings.sdrModuleNames.join(", ")
                            : "No SoapySDR receiver modules detected"
                    }
                    Label { text: "Physical SDR" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        ComboBox {
                            objectName: "sdrDeviceCombo"
                            Layout.fillWidth: true
                            visible: !root.sdrDiscoveryRunning
                            model: appSettings.sdrDeviceNames
                            currentIndex: appSettings.sdrDeviceIndex
                            enabled: appSettings.sdrBackendAvailable
                                     && appSettings.sdrDeviceNames.length > 0
                            onActivated: appSettings.selectSdrDevice(currentIndex)
                            ToolTip.visible: hovered
                            ToolTip.text: enabled
                                ? "Select one physical receive-only SDR; alternative operating modes of the same serial number are grouped together"
                                : "Install the matching SoapySDR hardware module, connect the receiver, then refresh"
                        }
                        RowLayout {
                            objectName: "sdrDiscoveryBusyRow"
                            Layout.fillWidth: true
                            visible: root.sdrDiscoveryRunning
                            spacing: 10
                            BusyIndicator {
                                objectName: "sdrDiscoveryBusyIndicator"
                                running: root.sdrDiscoveryRunning
                                implicitWidth: 26
                                implicitHeight: 26
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    objectName: "sdrDiscoveryBusyLabel"
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: "#f3bd55"
                                    text: "Scanning for receivers…"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: "#91a0b1"
                                    text: "Each installed vendor module is asked what hardware it can see, which can take several seconds. Nothing is opened or started."
                                }
                            }
                        }
                    }
                    Label { text: "Operating mode" }
                    ComboBox {
                        objectName: "sdrOperatingModeCombo"
                        Layout.fillWidth: true
                        model: appSettings.sdrOperatingModeNames
                        currentIndex: appSettings.sdrOperatingModeIndex
                        enabled: appSettings.sdrOperatingModeNames.length > 1
                        onActivated:
                            appSettings.selectSdrOperatingMode(currentIndex)
                        ToolTip.visible: hovered
                        ToolTip.text: appSettings.sdrOperatingModeNames.length > 1
                            ? "Choose a hardware operating configuration. Single tuner is recommended for CW Buddy's current one-channel receive path."
                            : "This receiver exposes one operating configuration"
                    }
                    Label { text: "Mode details" }
                    Label {
                        objectName: "sdrOperatingModeHelp"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        property string selectedMode: appSettings.sdrOperatingModeIndex >= 0
                            ? appSettings.sdrOperatingModeNames[appSettings.sdrOperatingModeIndex]
                            : ""
                        text: selectedMode.indexOf("ST ") === 0
                            ? "Single tuner is the recommended one-channel CW Buddy mode and can select either tuner input exposed by the driver."
                            : selectedMode.indexOf("DT ") === 0
                              ? "Dual tuner provides two synchronized RX channels; CW Buddy currently consumes channel 0 only."
                              : selectedMode.indexOf("MA8 ") === 0
                                ? "Master mode using the alternative 8 MHz master sample clock for coordinated master/slave operation."
                                : selectedMode.indexOf("MA ") === 0
                                  ? "Master mode using the 6 MHz master sample clock for coordinated master/slave operation."
                                  : selectedMode.indexOf("SL ") === 0
                                    ? "Slave is an advanced mode controlled by a coordinated RSPduo master process."
                                    : "The receiver exposes a single default operating configuration."
                    }
                    Label { text: "Discovery" }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            objectName: "refreshSdrDevicesButton"
                            text: root.sdrDiscoveryRunning
                                  ? "Scanning…" : "Refresh devices"
                            enabled: !root.sdrDiscoveryRunning
                            onClicked: root.beginSdrDiscovery()
                            ToolTip.visible: hovered
                            ToolTip.text: enabled
                                ? "Rescan SoapySDR modules and attached receivers without starting reception"
                                : "A scan is already in progress"
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: appSettings.sdrBackendAvailable ? "#91a0b1" : "#f3bd55"
                            // While a scan runs the stored text still describes
                            // the previous result, so it is withheld rather
                            // than presented as the current state.
                            text: root.sdrDiscoveryRunning
                                  ? "Scanning for receivers…"
                                  : appSettings.sdrDiagnostic
                        }
                    }
                    Label { text: "SDR center frequency (kHz)" }
                    TextField {
                        objectName: "sdrCenterFrequencyField"
                        Layout.fillWidth: true
                        enabled: appSettings.sdrBackendAvailable
                                 && !appSettings.sdrFollowRadioVfo
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        text: root.formatFrequencyKhz(
                                  appSettings.sdrCenterFrequencyHz)
                        placeholderText: "7021.43"
                        validator: RegularExpressionValidator {
                            regularExpression: /[0-9]{1,8}([.,][0-9]{0,3})?/
                        }
                        onEditingFinished: {
                            var frequencyHz = root.parseFrequencyKhz(text)
                            if (frequencyHz > 0)
                                appSettings.sdrCenterFrequencyHz = frequencyHz
                            text = root.formatFrequencyKhz(
                                appSettings.sdrCenterFrequencyHz)
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Enter VFO-style kHz (for example 7021.43 means 7.02143 MHz). Radio-follow mode updates this from authoritative CAT readback."
                    }
                    Label { text: "IQ sample rate" }
                    ComboBox {
                        objectName: "sdrSampleRateCombo"
                        Layout.fillWidth: true
                        enabled: appSettings.sdrBackendAvailable
                        editable: true
                        model: appSettings.sdrSampleRateOptions.length > 0
                            ? appSettings.sdrSampleRateOptions
                            : [62500, 96000, 125000, 192000, 250000,
                               384000, 500000, 768000, 1000000,
                               2000000, 2400000, 8000000, 10000000]
                        currentIndex: model.indexOf(appSettings.sdrSampleRateHz)
                        displayText: appSettings.sdrSampleRateHz + " Hz"
                        onActivated: appSettings.sdrSampleRateHz = currentValue
                        onAccepted: appSettings.sdrSampleRateHz = Number(editText.replace(/[^0-9]/g, ""))
                        ToolTip.visible: hovered
                        ToolTip.text: "Effective IQ output rate. Drivers such as SDRplay apply their supported hardware decimation automatically for lower rates."
                    }
                    Label { text: "Hardware RF bandwidth" }
                    ComboBox {
                        objectName: "sdrBandwidthCombo"
                        Layout.fillWidth: true
                        enabled: appSettings.sdrBackendAvailable
                        editable: true
                        model: appSettings.sdrBandwidthOptions.length > 1
                            ? appSettings.sdrBandwidthOptions
                            : [0, 200000, 300000, 600000, 1536000,
                               5000000, 6000000, 7000000, 8000000]
                        currentIndex: model.indexOf(appSettings.sdrBandwidthHz)
                        displayText: appSettings.sdrBandwidthHz === 0
                            ? "Automatic" : appSettings.sdrBandwidthHz + " Hz"
                        onActivated: appSettings.sdrBandwidthHz = currentValue
                        onAccepted: appSettings.sdrBandwidthHz =
                            Number(editText.replace(/[^0-9]/g, ""))
                        ToolTip.visible: hovered
                        ToolTip.text: "Requested analogue/baseband RF filter width; the provider selects the nearest supported value"
                    }
                    Label { text: "Driver decimation" }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#43c6ac"
                        text: "Automatic through the effective IQ sample rate ("
                              + appSettings.sdrSampleRateHz + " S/s requested)"
                    }
                    Label { text: "Antenna / tuner input" }
                    ComboBox {
                        objectName: "sdrAntennaCombo"
                        Layout.fillWidth: true
                        model: appSettings.sdrAntennaNames
                        currentIndex: appSettings.sdrAntennaIndex
                        enabled: appSettings.sdrAntennaNames.length > 0
                        onActivated: appSettings.selectSdrAntenna(currentIndex)
                        ToolTip.visible: hovered
                        ToolTip.text: enabled
                            ? "Receiver input exposed by the selected SDR operating mode"
                            : "This SDR driver does not expose an antenna selector"
                    }
                    Label { text: "Decoder window center (kHz)" }
                    TextField {
                        objectName: "sdrDecoderCenterFrequencyField"
                        Layout.fillWidth: true
                        enabled: appSettings.sdrBackendAvailable
                                 && !appSettings.sdrFollowRadioVfo
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        text: root.formatFrequencyKhz(
                                  appSettings.sdrDecoderCenterFrequencyHz)
                        validator: RegularExpressionValidator {
                            regularExpression: /[0-9]{1,8}([.,][0-9]{0,3})?/
                        }
                        onEditingFinished: {
                            var frequencyHz = root.parseFrequencyKhz(text)
                            if (frequencyHz > 0)
                                appSettings.sdrDecoderCenterFrequencyHz = frequencyHz
                            text = root.formatFrequencyKhz(
                                appSettings.sdrDecoderCenterFrequencyHz)
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Only this bounded RF region is sent to stream detection and CW decoding"
                    }
                    Label { text: "Decoder bandwidth" }
                    ComboBox {
                        objectName: "sdrDecoderBandwidthCombo"
                        Layout.fillWidth: true
                        // Presets for quick selection; the width itself is
                        // continuous, so a value set by dragging the spectrum
                        // matches no entry and currentIndex is -1. displayText
                        // is bound to the live value rather than to the model,
                        // so the real width still shows.
                        model: [2000, 3000, 6000, 12000, 24000]
                        currentIndex: model.indexOf(appSettings.sdrDecoderBandwidthHz)
                        displayText: (appSettings.sdrDecoderBandwidthHz / 1000)
                                     + " kHz"
                        onActivated:
                            appSettings.sdrDecoderBandwidthHz = currentValue
                        ToolTip.visible: hovered
                        ToolTip.text: "Limits CPU-intensive CW detection while the full acquired spectrum remains visible"
                    }
                    Label { text: "SDR LO offset (Hz)" }
                    TextField {
                        objectName: "sdrRadioLoOffsetField"
                        Layout.fillWidth: true
                        enabled: appSettings.sdrFollowRadioVfo
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        text: appSettings.sdrRadioLoOffsetHz.toString()
                        validator: RegularExpressionValidator { regularExpression: /-?[0-9]{1,8}/ }
                        onEditingFinished: appSettings.sdrRadioLoOffsetHz = Number(text)
                        ToolTip.visible: hovered
                        ToolTip.text: "Optional offset between the radio RX frequency and SDR center; automatically bounded so the decoder remains inside the acquired passband"
                    }
                    Label { text: "Visible RF span" }
                    Label {
                        objectName: "sdrWidePassbandLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#43c6ac"
                        text: appSettings.sdrWidePassbandSummary
                    }
                    Label { text: "Gain control" }
                    CheckBox {
                        objectName: "sdrAutomaticGainCheck"
                        text: "Use device automatic gain when supported"
                        enabled: appSettings.sdrBackendAvailable
                                 && appSettings.sdrAutomaticGainAvailable
                        checked: appSettings.sdrAutomaticGain
                        onToggled: appSettings.sdrAutomaticGain = checked
                    }
                    Label { text: "Manual gain (dB)" }
                    SpinBox {
                        objectName: "sdrGainSpinBox"
                        editable: true
                        from: Math.ceil(appSettings.sdrMinimumGainDb)
                        to: Math.floor(appSettings.sdrMaximumGainDb)
                        value: Math.round(appSettings.sdrGainDb)
                        enabled: appSettings.sdrBackendAvailable
                                 && !appSettings.sdrAutomaticGain
                        onValueModified: appSettings.sdrGainDb = value
                    }
                    Label { text: "Safety and availability" }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#f3bd55"
                        text: appSettings.sdrBackendAvailable
                            ? "Configuration is receive-only. Selecting or refreshing a device does not start it; use the receiver workspace to begin reception."
                            : "Install SoapySDR plus the receiver-specific module (for example RTL-SDR or SDRplay), then use an SDR-enabled CW Buddy build. No external SDR application is required."
                    }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 12
                    anchors.margins: 22
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Optionally configure a compatible local decoding model. The deterministic decoder remains available and model output cannot control transmission."
                    }
                    Label { text: "Keying model" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RadioButton {
                            objectName: "keyingModelAdaptiveThresholdRadio"
                            text: "Adaptive threshold"
                            checked: appSettings.keyingModel !== "semi-markov"
                            onToggled: if (checked) appSettings.keyingModel = "adaptive-threshold"
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 26
                            Layout.bottomMargin: 6
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "Decides key-up and key-down from the envelope, moment by moment. Steadiest on hand and bug sending, and shows text soonest."
                        }
                        RadioButton {
                            objectName: "keyingModelSemiMarkovRadio"
                            text: "Semi-Markov (HSMM)"
                            checked: appSettings.keyingModel === "semi-markov"
                            onToggled: if (checked) appSettings.keyingModel = "semi-markov"
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: 26
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "Weighs each mark and gap against the lengths Morse expects. Stronger on machine-sent, weighted and Farnsworth keying; weaker when the sender's timing wanders. Costs about one character of delay."
                        }
                    }
                    Label { text: "Weak signals" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        CheckBox {
                            objectName: "decodeWeakSignalsCheck"
                            text: "Decode every tracked signal"
                            checked: appSettings.decodeWeakSignals
                            onToggled: appSettings.decodeWeakSignals = checked
                        }
                        RowLayout {
                            spacing: 8
                            Label {
                                // The threshold decides nothing once every
                                // tracked signal is decoded, so it reads as
                                // inert rather than as a limit still in force.
                                color: appSettings.decodeWeakSignals ? "#667586" : "#c7d2df"
                                text: "Decode only above"
                            }
                            SpinBox {
                                objectName: "minimumDecodeSnrDbSpin"
                                enabled: !appSettings.decodeWeakSignals
                                // SpinBox counts in whole numbers, so the
                                // threshold is held here in tenths of a
                                // decibel and presented with one decimal.
                                from: 0
                                to: 400
                                stepSize: 1
                                editable: true
                                value: Math.round(appSettings.minimumDecodeSnrDb * 10)
                                validator: DoubleValidator {
                                    bottom: 0.0
                                    top: 40.0
                                    decimals: 1
                                    notation: DoubleValidator.StandardNotation
                                }
                                textFromValue: function(value, locale) {
                                    return Number(value / 10).toLocaleString(locale, 'f', 1)
                                }
                                valueFromText: function(text, locale) {
                                    return Math.round(Number.fromLocaleString(locale, text) * 10)
                                }
                                onValueModified: appSettings.minimumDecodeSnrDb = value / 10
                                ToolTip.visible: hovered && enabled
                                ToolTip.text: "Signal-to-noise level, in decibels, a tracked signal must reach before it is decoded"
                            }
                            Label {
                                color: appSettings.decodeWeakSignals ? "#667586" : "#91a0b1"
                                text: "dB above the noise floor"
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "A signal below the threshold is still detected, followed, and drawn in the spectrum; only its decoding is withheld. Off by default because below this level the decoder receives fragments rather than copy, filling the transcript with nothing while each such track costs a full decoder's work. The default of 4.0 dB is deliberately permissive: a higher threshold was tried and suppressed signals that could be worked, so this withholds decoding only from tracks that are barely above the noise at all. Enable the option above to decode every tracked signal regardless of level."
                        }
                    }
                    Label { text: "Local model" }
                    CheckBox {
                        objectName: "localDecoderEnabledCheck"
                        text: "Enable local model refinement"
                        checked: appSettings.localDecoderEnabled
                        enabled: appSettings.localDecoderBackendAvailable
                        onToggled: appSettings.localDecoderEnabled = checked
                    }
                    Label { text: "Model file" }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            objectName: "localDecoderModelPathField"
                            Layout.fillWidth: true
                            readOnly: true
                            text: appSettings.localDecoderModelPath
                            placeholderText: "No model selected"
                            ToolTip.visible: hovered && text.length > 0
                            ToolTip.text: text
                        }
                        Button {
                            objectName: "browseLocalDecoderModelButton"
                            text: "Browse…"
                            onClicked: localDecoderModelDialog.open()
                            ToolTip.visible: hovered
                            ToolTip.text: "Choose a compatible local ONNX model file"
                        }
                        Button {
                            text: "Clear"
                            enabled: appSettings.localDecoderModelPath.length > 0
                            onClicked: appSettings.clearLocalDecoderModel()
                            ToolTip.visible: hovered
                            ToolTip.text: "Remove the selected local model from this profile"
                        }
                    }
                    Label { text: "Metadata file" }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            objectName: "localDecoderMetadataPathField"
                            Layout.fillWidth: true
                            readOnly: true
                            text: appSettings.localDecoderMetadataPath
                            placeholderText: "No metadata selected"
                            ToolTip.visible: hovered && text.length > 0
                            ToolTip.text: text
                        }
                        Button {
                            objectName: "browseLocalDecoderMetadataButton"
                            text: "Browse…"
                            onClicked: localDecoderMetadataDialog.open()
                            ToolTip.visible: hovered
                            ToolTip.text: "Choose the JSON metadata describing the selected model"
                        }
                        Button {
                            text: "Clear"
                            enabled: appSettings.localDecoderMetadataPath.length > 0
                            onClicked: appSettings.clearLocalDecoderMetadata()
                            ToolTip.visible: hovered
                            ToolTip.text: "Remove the selected model metadata from this profile"
                        }
                    }
                    Label { text: "Status" }
                    Label {
                        objectName: "localDecoderStatusLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: appSettings.localDecoderBackendAvailable
                              ? replayController.localCharacterStatus
                              : appSettings.localDecoderStatus
                        color: replayController.localCharacterState === "error"
                               ? "#ef7d85"
                               : (appSettings.localDecoderBackendAvailable
                                  ? "#43c6ac" : "#f3bd55")
                    }
                    Label { text: "Debug capture" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "Records raw live audio and per-track decoder internals to a timestamped folder, for troubleshooting a signal that will not decode. Review the files before sharing them: the audio is whatever the selected input picked up."
                        }
                        RowLayout {
                            spacing: 8
                            Button {
                                objectName: "settingsDebugCaptureButton"
                                text: replayController.debugCaptureActive
                                      ? "Stop capture" : "Start capture"
                                enabled: replayController.debugCaptureActive
                                         || replayController.liveCapturing
                                onClicked: replayController.debugCaptureActive
                                           ? replayController.stopDebugCapture()
                                           : replayController.startDebugCapture()
                                ToolTip.visible: hovered
                                ToolTip.text: replayController.debugCaptureActive
                                    ? "Stop and finalize the current diagnostic capture"
                                    : "Record bounded raw audio and decoder evidence for troubleshooting"
                            }
                            Button {
                                objectName: "settingsDebugCaptureFolderButton"
                                text: "Open capture folder"
                                // Nothing has been written yet before the first
                                // capture, so there is no folder to open.
                                enabled: replayController.debugCapturePath.length > 0
                                onClicked: replayController.openDebugCaptureFolder()
                                ToolTip.visible: hovered
                                ToolTip.text: enabled
                                    ? "Open the latest capture folder in the file manager"
                                    : "Create a diagnostic capture first"
                            }
                        }
                        RowLayout {
                            spacing: 8
                            Label { text: "Stop automatically after" }
                            SpinBox {
                                objectName: "debugCaptureMaximumSecondsSpin"
                                from: 30
                                to: 1800
                                stepSize: 30
                                editable: true
                                value: appSettings.debugCaptureMaximumSeconds
                                onValueModified: appSettings.debugCaptureMaximumSeconds = value
                            }
                            Label { text: "seconds"; color: "#91a0b1" }
                        }
                        Label {
                            objectName: "settingsDebugCaptureStatusLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            visible: replayController.debugCaptureActive
                                     || replayController.debugCapturePath.length > 0
                            text: replayController.debugCaptureActive
                                  ? "Capturing... " + replayController.debugCaptureElapsedSeconds.toFixed(0)
                                    + "s / " + appSettings.debugCaptureMaximumSeconds + "s - "
                                    + replayController.debugCapturePath
                                  : "Last capture: " + replayController.debugCaptureNote
                                    + " - " + replayController.debugCapturePath
                            color: replayController.debugCaptureActive ? "#f3bd55" : "#6c7c8e"
                        }
                    }
                    Rectangle {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        height: 1
                        color: "#2b3541"
                    }
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Optional offline callsign suggestions can use a managed Super Check Partial MASTER.SCP cache or an operator-supplied file. Matches only rank calls already present in multiple acoustic alternatives; they never confirm a stream, replace decoded text, alert on your call, or control transmission."
                    }
                    Label { text: "Managed SCP database" }
                    CheckBox {
                        objectName: "managedCallsignDatabaseEnabledCheck"
                        text: "Use managed offline cache"
                        checked: callsignDatabaseUpdater.managedEnabled
                        onToggled: callsignDatabaseUpdater.managedEnabled = checked
                    }
                    Label { text: "Managed updates" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        CheckBox {
                            objectName: "managedCallsignDatabaseAutoUpdateCheck"
                            text: "Automatically check at most daily"
                            enabled: callsignDatabaseUpdater.managedEnabled
                            checked: callsignDatabaseUpdater.autoUpdateEnabled
                            onToggled: callsignDatabaseUpdater.autoUpdateEnabled = checked
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Button {
                                objectName: "managedCallsignDatabaseUpdateButton"
                                enabled: callsignDatabaseUpdater.managedEnabled
                                         && !callsignDatabaseUpdater.checking
                                         && !callsignDatabaseUpdater.downloading
                                text: callsignDatabaseUpdater.checking
                                      || callsignDatabaseUpdater.downloading
                                      ? "Working…"
                                      : (callsignDatabaseUpdater.updateAvailable
                                         ? "Download update"
                                         : "Check for updates")
                                onClicked: callsignDatabaseUpdater.updateAvailable
                                           ? callsignDatabaseUpdater.updateDatabase()
                                           : callsignDatabaseUpdater.checkForUpdates()
                                ToolTip.visible: hovered
                                ToolTip.text: callsignDatabaseUpdater.updateAvailable
                                    ? "Download, validate, and atomically replace the managed offline list"
                                    : "Check the managed callsign-list provider for a newer release"
                            }
                            Label {
                                Layout.fillWidth: true
                                color: "#8290a0"
                                elide: Text.ElideRight
                                text: callsignDatabaseUpdater.installedVersion.length > 0
                                      ? "Installed release "
                                        + callsignDatabaseUpdater.installedVersion.substring(0, 10)
                                      : "No managed copy installed"
                            }
                        }
                        Label {
                            objectName: "managedCallsignDatabaseStatusLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: "#80cbc4"
                            text: callsignDatabaseUpdater.statusMessage
                                  + " · Last check: "
                                  + callsignDatabaseUpdater.lastCheckedText
                        }
                    }
                    Label { text: "Local callsign suggestions" }
                    CheckBox {
                        objectName: "localCallsignDatabaseEnabledCheck"
                        text: "Enable operator-supplied local file"
                        enabled: !callsignDatabaseUpdater.managedEnabled
                        checked: appSettings.localCallsignDatabaseEnabled
                        onToggled: appSettings.localCallsignDatabaseEnabled = checked
                    }
                    Label { text: "Callsign-list file" }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            objectName: "localCallsignDatabasePathField"
                            Layout.fillWidth: true
                            readOnly: true
                            text: appSettings.localCallsignDatabasePath
                            placeholderText: "No local callsign list selected"
                            ToolTip.visible: hovered && text.length > 0
                            ToolTip.text: text
                        }
                        Button {
                            objectName: "browseLocalCallsignDatabaseButton"
                            text: "Browse…"
                            enabled: !callsignDatabaseUpdater.managedEnabled
                            onClicked: localCallsignDatabaseDialog.open()
                            ToolTip.visible: hovered
                            ToolTip.text: "Choose an operator-supplied master.scp or Call History file"
                        }
                        Button {
                            text: "Clear"
                            enabled: !callsignDatabaseUpdater.managedEnabled
                                     && appSettings.localCallsignDatabasePath.length > 0
                            onClicked: appSettings.clearLocalCallsignDatabase()
                            ToolTip.visible: hovered
                            ToolTip.text: "Remove the operator-supplied callsign list from this profile"
                        }
                    }
                    Label { text: "Correct near misses" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        CheckBox {
                            objectName: "callsignDatabaseCorrectionCheck"
                            text: "Correct near-miss callsigns from the list"
                            // Requires a loaded list: with none there is
                            // nothing to correct against, and a control that
                            // silently does nothing is worse than one that
                            // says why it cannot act.
                            enabled: replayController.offlineCallsignDatabaseState === "ready"
                            checked: appSettings.callsignDatabaseCorrectionEnabled
                            onToggled: appSettings.callsignDatabaseCorrectionEnabled = checked
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: replayController.offlineCallsignDatabaseState === "ready"
                                   ? "#91a0b1" : "#f3bd55"
                            text: replayController.offlineCallsignDatabaseState === "ready"
                                  ? "When a decoded callsign is within two characters of a single entry in the list, suggest that entry instead. Off by default: two listed stations can differ by one character, so a correction can name a station that was never sent. The suggestion stays advisory either way and never changes the transcript or the confirmed callsign."
                                  : "Unavailable until a callsign list is loaded. Enable the managed list above, or select an operator-supplied file, and this becomes available once its state reads ready."
                        }
                    }

                    Label { text: "Local-list state" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            objectName: "localCallsignDatabaseStatusLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: replayController.offlineCallsignDatabaseStatus
                            color: replayController.offlineCallsignDatabaseState
                                   === "error" ? "#ef7d85"
                                   : (replayController.offlineCallsignDatabaseState
                                      === "ready" ? "#80cbc4" : "#8290a0")
                        }
                        Button {
                            objectName: "reloadLocalCallsignDatabaseButton"
                            text: "Reload local file"
                            enabled: appSettings.localCallsignDatabaseEnabled
                                     && !callsignDatabaseUpdater.managedEnabled
                                     && appSettings.localCallsignDatabasePath.length > 0
                            onClicked: appSettings.reloadLocalCallsignDatabase()
                            ToolTip.visible: hovered
                            ToolTip.text: enabled
                                ? "Reload and validate the selected local callsign file"
                                : "Enable and select an operator-supplied file first"
                        }
                    }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 12
                    anchors.margins: 22
                    Label { text: "Radio participation" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["No radio — receive-only (SWL)", "Radio enabled"]
                        currentIndex: appSettings.radioEnabled ? 1 : 0
                        onActivated: appSettings.radioEnabled = currentIndex === 1
                    }
                    Label { text: "Detected online radio" }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            Layout.fillWidth: true
                            model: appSettings.detectedRadioNames
                            enabled: count > 0
                            currentIndex: appSettings.detectedRadioIndex
                            displayText: count > 0 ? currentText : "None detected"
                            onActivated: appSettings.selectDetectedRadio(currentIndex)
                        }
                        Button {
                            text: "Refresh"; onClicked: appSettings.refreshDetectedRadios()
                            ToolTip.visible: hovered
                            ToolTip.text: "Refresh positively identified radios from configured integrations"
                        }
                    }
                    Label { text: "Manual radio template" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: appSettings.referenceRigNames
                        currentIndex: appSettings.referenceRigIndex
                        onActivated: appSettings.selectReferenceRig(currentIndex)
                    }
                    Label { text: "Frequency control" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["OmniRig (Windows)", "Hamlib", "CAT4OM network service"]
                        currentIndex: appSettings.frequencyBackendIndex
                        onActivated: appSettings.frequencyBackendIndex = currentIndex
                    }
                    Label { text: "RX tuning step" }
                    LabeledSlider {
                        objectName: "radioTuningStepSlider"
                        Layout.fillWidth: true
                        caption: "kHz"
                        from: 1
                        to: 100
                        stepSize: 1
                        decimals: 0
                        value: appSettings.radioTuningStepHz / 1000
                        onMoved: value => appSettings.radioTuningStepHz = Math.round(value * 1000)
                    }
                    Label { text: "OmniRig radio slot"; visible: appSettings.frequencyBackendIndex === 0 }
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
                    Label { text: "Hamlib rigctld host"; visible: appSettings.frequencyBackendIndex === 1 }
                    TextField { Layout.fillWidth: true; visible: appSettings.frequencyBackendIndex === 1; text: appSettings.hamlibHost; placeholderText: "127.0.0.1"; onEditingFinished: appSettings.hamlibHost = text }
                    Label { text: "Hamlib rigctld port"; visible: appSettings.frequencyBackendIndex === 1 }
                    SpinBox { visible: appSettings.frequencyBackendIndex === 1; editable: true; from: 1; to: 65535; value: appSettings.hamlibPort; onValueModified: appSettings.hamlibPort = value }
                    Label { text: "Hamlib VFO mapping"; visible: appSettings.frequencyBackendIndex === 1 }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: appSettings.frequencyBackendIndex === 1
                        TextField { Layout.fillWidth: true; text: appSettings.hamlibRxVfo; placeholderText: "VFOA (RX)"; onEditingFinished: appSettings.hamlibRxVfo = text }
                        TextField { Layout.fillWidth: true; text: appSettings.hamlibTxVfo; placeholderText: "VFOB (TX)"; onEditingFinished: appSettings.hamlibTxVfo = text }
                    }
                    Label { text: "Hamlib control"; visible: appSettings.frequencyBackendIndex === 1 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: appSettings.frequencyBackendIndex === 1
                        CheckBox {
                            text: "Allow frequency, mode, and split writes"
                            checked: appSettings.hamlibWritable
                            onToggled: appSettings.hamlibWritable = checked
                        }
                        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#91a0b1"; text: appSettings.hamlibState }
                        RowLayout {
                            Button { text: "Connect"; onClicked: appSettings.connectHamlib(); ToolTip.visible: hovered; ToolTip.text: "Connect to the configured rigctld endpoint; PTT and KEY are never exposed through this provider" }
                            Button { text: "Disconnect"; onClicked: appSettings.disconnectHamlib(); ToolTip.visible: hovered; ToolTip.text: "Close the Hamlib radio-control connection" }
                        }
                    }
                    Label { text: "CAT4OM Control URL"; visible: appSettings.frequencyBackendIndex === 2 }
                    TextField { Layout.fillWidth: true; visible: appSettings.frequencyBackendIndex === 2; text: appSettings.cat4omUrl; placeholderText: "ws://127.0.0.1:5001/"; onEditingFinished: appSettings.cat4omUrl = text }
                    Label { text: "CAT4OM radio ID"; visible: appSettings.frequencyBackendIndex === 2 }
                    TextField { Layout.fillWidth: true; visible: appSettings.frequencyBackendIndex === 2; text: appSettings.cat4omRadioId; placeholderText: "Empty selects the first visible radio"; onEditingFinished: appSettings.cat4omRadioId = text }
                    Label { text: "CAT4OM password"; visible: appSettings.frequencyBackendIndex === 2 }
                    TextField { Layout.fillWidth: true; visible: appSettings.frequencyBackendIndex === 2; echoMode: TextInput.Password; placeholderText: "Session only — never saved"; onTextEdited: appSettings.cat4omPassword = text }
                    Label { text: "CAT4OM connection"; visible: appSettings.frequencyBackendIndex === 2 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: appSettings.frequencyBackendIndex === 2
                        Label { text: appSettings.cat4omState; color: "#91a0b1"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        Label { text: appSettings.cat4omFrequencySummary; color: "#43c6ac" }
                        RowLayout {
                            Button {
                                text: "Test read-only"; onClicked: appSettings.testCat4omConnection()
                                ToolTip.visible: hovered
                                ToolTip.text: "Connect as an observer without requesting radio-control ownership"
                            }
                            Button {
                                text: "Connect control"; onClicked: appSettings.connectCat4omControl()
                                ToolTip.visible: hovered
                                ToolTip.text: "Connect and negotiate the configured control capability"
                            }
                            Button {
                                text: "Request ownership"; enabled: !appSettings.cat4omCanWrite
                                onClicked: appSettings.requestCat4omOwnership()
                                ToolTip.visible: hovered
                                ToolTip.text: enabled
                                    ? "Request the service's exclusive radio-control lease"
                                    : "This connection already has write capability"
                            }
                            Button {
                                text: "Disconnect"; onClicked: appSettings.disconnectCat4om()
                                ToolTip.visible: hovered
                                ToolTip.text: "Close the CAT4OM connection and release its control state"
                            }
                        }
                    }
                    Label { text: "Split operation" }
                    CheckBox { text: "Use independent TX VFO"; checked: appSettings.splitEnabled; onToggled: appSettings.splitEnabled = checked }
                    Label { text: "RX transverter offset (Hz)" }
                    TextField { Layout.fillWidth: true; text: appSettings.rxTransverterOffsetHz.toString(); placeholderText: "Signed value, e.g. 116000000"; onEditingFinished: appSettings.rxTransverterOffsetHz = Number(text) }
                    Label { text: "TX transverter offset (Hz)" }
                    TextField { Layout.fillWidth: true; text: appSettings.txTransverterOffsetHz.toString(); placeholderText: "Signed value, e.g. 407000000"; onEditingFinished: appSettings.txTransverterOffsetHz = Number(text) }
                    Label { text: "CW audio-to-RF mapping" }
                    ComboBox {
                        Layout.fillWidth: true
                        model: ["CW-U / USB: RF rises with audio tone", "CW-L / LSB: RF falls with audio tone"]
                        currentIndex: appSettings.cwToneSidebandIndex
                        onActivated: appSettings.cwToneSidebandIndex = currentIndex
                    }
                    Label { text: "" }
                    RowLayout {
                        Button {
                            text: "Restore radio defaults"; onClicked: appSettings.resetToReferenceDefaults()
                            ToolTip.visible: hovered
                            ToolTip.text: "Restore the selected reference rig's editable CAT defaults"
                        }
                    }
                    Label { text: "" }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: appSettings.radioEnabled
                              ? (appSettings.frequencyBackendIndex === 0
                                 ? "OmniRig owns radio-model and serial framing configuration; CW Buddy selects only the OmniRig slot. Direct key/PTT remains an independent connection."
                                 : appSettings.frequencyBackendIndex === 1
                                   ? "rigctld owns the physical radio and serial framing. CW Buddy configures only its loopback endpoint, VFO mapping, and write permission."
                                   : "CAT4OM owns the physical radio and serial framing. CW Buddy configures only its Control service connection; passwords are never saved.")
                              : "SWL mode processes receiver audio without CAT or key/PTT. Stored radio values are retained in case this profile is switched back to radio operation."
                    }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 12
                    anchors.margins: 22
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Join one DX cluster or Reverse Beacon Network node and receive what other listening stations report hearing. Spots place markers on the spectrum at the reported frequencies and give decoded callsigns a second opinion. Whether a report was made by an automatic skimmer or typed by an operator arrives with the spot itself; there is nothing to choose here. The link is receive-only: nothing is ever published, your station is never announced, and no spot can start transmission."
                    }
                    Label { text: "Cluster node" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        ComboBox {
                            id: dxClusterServerCombo
                            objectName: "dxClusterServerCombo"
                            Layout.fillWidth: true
                            // The last entry is always Custom, so its position
                            // follows the loaded list rather than a constant
                            // that a shorter server file would put out of step.
                            readonly property int customIndex: count - 1
                            readonly property var selectedServer:
                                (appSettings.dxClusterServerIndex >= 0
                                 && appSettings.dxClusterServerIndex < appSettings.dxClusterServers.length)
                                ? appSettings.dxClusterServers[appSettings.dxClusterServerIndex]
                                : null
                            model: {
                                var names = []
                                for (var i = 0; i < appSettings.dxClusterServers.length; ++i)
                                    names.push(appSettings.dxClusterServers[i].name)
                                names.push("Custom…")
                                return names
                            }
                            currentIndex: appSettings.dxClusterServerIndex < 0
                                          ? customIndex
                                          : Math.min(appSettings.dxClusterServerIndex, customIndex)
                            onActivated: appSettings.dxClusterServerIndex =
                                         (currentIndex === customIndex ? -1 : currentIndex)
                            ToolTip.visible: hovered
                            ToolTip.text: "Read from dictionaries/dx-cluster-servers.txt; edit that file to add or correct a node"
                        }
                        Label {
                            objectName: "dxClusterServerNoteLabel"
                            Layout.fillWidth: true
                            visible: dxClusterServerCombo.currentIndex !== dxClusterServerCombo.customIndex
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: dxClusterServerCombo.selectedServer
                                  ? dxClusterServerCombo.selectedServer.host
                                    + ":" + dxClusterServerCombo.selectedServer.port
                                    + " — " + dxClusterServerCombo.selectedServer.note
                                  : "No servers could be read from dictionaries/dx-cluster-servers.txt; choose Custom and enter a node."
                        }
                    }
                    Label {
                        text: "Custom node"
                        visible: dxClusterServerCombo.currentIndex === dxClusterServerCombo.customIndex
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: dxClusterServerCombo.currentIndex === dxClusterServerCombo.customIndex
                        RowLayout {
                            spacing: 8
                            Layout.fillWidth: true
                            TextField {
                                id: dxClusterCustomHostField
                                objectName: "dxClusterCustomHostField"
                                Layout.fillWidth: true
                                text: appSettings.dxClusterCustomHost
                                placeholderText: "cluster.example.org"
                                inputMethodHints: Qt.ImhUrlCharactersOnly
                                                  | Qt.ImhNoPredictiveText
                                validator: RegularExpressionValidator {
                                    regularExpression: /[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?/
                                }
                                onEditingFinished: {
                                    if (acceptableInput)
                                        appSettings.dxClusterCustomHost = text
                                    else
                                        text = appSettings.dxClusterCustomHost
                                }
                            }
                            SpinBox {
                                objectName: "dxClusterCustomPortSpin"
                                editable: true
                                from: 1
                                to: 65535
                                value: appSettings.dxClusterCustomPort
                                onValueModified: appSettings.dxClusterCustomPort = value
                                ToolTip.visible: hovered
                                ToolTip.text: "Cluster nodes commonly listen on 23, 7300, 7373 or 8000; the Reverse Beacon Network CW stream is on 7000"
                            }
                        }
                        Label {
                            objectName: "dxClusterCustomHintLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: appSettings.dxClusterCustomHost.length > 0
                                   ? "#91a0b1" : "#f3bd55"
                            text: appSettings.dxClusterCustomHost.length > 0
                                  ? "Joined over plain telnet, which is what cluster software speaks; there is no encrypted alternative to offer."
                                  : "Enter the host name of the node to join, for example cluster.example.org"
                        }
                    }
                    Label { text: "Cluster link" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            CheckBox {
                                objectName: "dxClusterEnabledCheck"
                                text: "Join the selected node and receive its spots"
                                // A cluster login is the station callsign, and
                                // there is no anonymous one, so the switch is
                                // not reachable until a callsign has been set.
                                enabled: appSettings.ownCallsign.length > 0
                                checked: appSettings.dxClusterEnabled
                                onToggled: appSettings.dxClusterEnabled = checked
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: "Connection SSID"
                                color: appSettings.ownCallsign.length > 0
                                       ? "#c7d2df" : "#667586"
                            }
                            SpinBox {
                                objectName: "dxClusterLoginSsidSpin"
                                editable: true
                                from: 0
                                to: 99
                                stepSize: 1
                                enabled: appSettings.ownCallsign.length > 0
                                value: appSettings.dxClusterLoginSsid
                                onValueModified: appSettings.dxClusterLoginSsid = value
                                // 0 is not a number the operator picks, it is
                                // the absence of an SSID, so it is shown as
                                // what it means rather than as a zero that
                                // would read like a chosen connection number.
                                validator: RegularExpressionValidator {
                                    regularExpression: /(none|-?[0-9]{1,2})/
                                }
                                textFromValue: function(value, locale) {
                                    return value === 0 ? "none" : "-" + value
                                }
                                valueFromText: function(text, locale) {
                                    var digits = text.replace(/[^0-9]/g, "")
                                    if (digits.length === 0)
                                        return 0
                                    return Math.max(0, Math.min(99, parseInt(digits, 10)))
                                }
                                ToolTip.visible: hovered && enabled
                                ToolTip.text: "Cluster nodes tell one of your connections from another by the SSID: log in as CALL-1 here to leave CALL or CALL-2 to your logging program. Leave it at none for a single connection."
                            }
                        }
                        // Plainly stated rather than tucked into a tooltip:
                        // this is the one place the application speaks on the
                        // network, and what it sends is on screen before the
                        // switch above can be used.
                        Label {
                            objectName: "dxClusterLoginLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: appSettings.dxClusterLoginCallsign.length > 0
                                  ? "Connects as " + appSettings.dxClusterLoginCallsign
                                    + "; cluster logins are sent unencrypted. Nothing else is sent: no spots, no announcements, no replies."
                                  : "Set your callsign on the Station tab first. A cluster login is sent as your callsign and cannot be made anonymously."
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "One node is joined at a time. A node that refuses or drops the link is left alone for longer each time rather than retried in a loop."
                        }
                    }

                    Rectangle {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: "#2b3541"
                    }
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: "Spots on the spectrum"
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                    Label { text: "Spot retention" }
                    RowLayout {
                        spacing: 8
                        SpinBox {
                            objectName: "dxSpotsRetentionMinutesSpin"
                            editable: true
                            from: 1
                            to: 60
                            stepSize: 1
                            enabled: appSettings.dxClusterEnabled
                            value: appSettings.dxSpotsRetentionMinutes
                            onValueModified: appSettings.dxSpotsRetentionMinutes = value
                            ToolTip.visible: hovered && enabled
                            ToolTip.text: "How long a received spot stays on the spectrum before it is dropped. A station that has moved on leaves a marker that is no longer true."
                        }
                        Label {
                            text: "minutes"
                            color: appSettings.dxClusterEnabled ? "#91a0b1" : "#667586"
                        }
                    }
                    Label { text: "Frequency match tolerance" }
                    RowLayout {
                        spacing: 8
                        SpinBox {
                            objectName: "dxSpotsToleranceHzSpin"
                            editable: true
                            from: 50
                            to: 1000
                            stepSize: 25
                            enabled: appSettings.dxClusterEnabled
                            value: appSettings.dxSpotsToleranceHz
                            onValueModified: appSettings.dxSpotsToleranceHz = value
                            ToolTip.visible: hovered && enabled
                            ToolTip.text: "How far a spot may sit from a tracked signal and still be treated as the same station. Wider settings match more spots, and match more of the wrong ones."
                        }
                        Label {
                            text: "Hz"
                            color: appSettings.dxClusterEnabled ? "#91a0b1" : "#667586"
                        }
                    }
                    Label { text: "Spectrum labels" }
                    CheckBox {
                        objectName: "dxSpotsShowLabelsCheck"
                        text: "Show spot labels on the spectrum"
                        enabled: appSettings.dxClusterEnabled
                        checked: appSettings.dxSpotsShowLabels
                        onToggled: appSettings.dxSpotsShowLabels = checked
                        ToolTip.visible: hovered && enabled
                        ToolTip.text: "Draw the spotted callsign beside its marker; turn this off to keep the markers without the text"
                    }
                    Label { text: "" }
                    Label {
                        objectName: "dxSpotsAuthorityLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#f3bd55"
                        text: "A spot is corroboration and never authority. It can lower how much evidence a decoded callsign needs before it is offered, and it can flag that an outside source disagrees with what was decoded here, but it never replaces a decoded callsign with a spotted one: reverse-beacon reports carry a measured error rate approaching two per cent per receiver, so a spot that contradicts good copy is as likely to be the mistaken one. What is decoded from the air remains the only source of the transcript."
                    }
                }
            }

            // Network tab. Its position in this StackLayout is the position
            // of "Network" in the TabBar above: fifth page, fifth button.
            ScrollView {
                contentWidth: availableWidth
                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 12
                    anchors.margins: 22
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Offer this station's diagnostics as a stream of JSON records, one record per line, so a station can be watched while it runs from another machine. The stream is emit-only: bytes sent to it are discarded, no connected reader can reach a setting, a control or the transmitter, and a reader that tries to talk to it is disconnected."
                    }
                    Label { text: "Diagnostics stream" }
                    CheckBox {
                        objectName: "diagnosticsEnabledCheck"
                        text: "Publish diagnostics on the selected addresses"
                        checked: appSettings.diagnosticsServerEnabled
                        onToggled: appSettings.diagnosticsServerEnabled = checked
                        ToolTip.visible: hovered
                        ToolTip.text: "Off unless asked for. Nothing is published until at least one address below is ticked."
                    }
                    Label { text: "Addresses" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: "#91a0b1"
                                text: appSettings.diagnosticsServerAvailableAddresses.length > 0
                                      ? "Tick every address the stream should be offered on. A loopback address keeps it on this machine; any other address puts it on that network."
                                      : "No addresses were found on this machine's interfaces. Refresh once the network is up."
                            }
                            Button {
                                objectName: "diagnosticsRefreshAddressesButton"
                                text: "Refresh"
                                onClicked: appSettings.refreshNetworkAddresses()
                                ToolTip.visible: hovered
                                ToolTip.text: "Re-read the addresses of this machine's network interfaces. An address that has gone away stays ticked but cannot be bound."
                            }
                        }
                        // The machine's own addresses, offered rather than
                        // guessed at: only the operator knows which of their
                        // networks is the one they meant.
                        ColumnLayout {
                            objectName: "diagnosticsAddressList"
                            Layout.fillWidth: true
                            spacing: 2
                            Repeater {
                                model: appSettings.diagnosticsServerAvailableAddresses
                                delegate: RowLayout {
                                    id: diagnosticsAddressRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 8
                                    CheckBox {
                                        objectName: "diagnosticsAddressCheck_"
                                                    + diagnosticsAddressRow.modelData.address
                                        text: diagnosticsAddressRow.modelData.address
                                        checked: appSettings.diagnosticsServerAddresses.indexOf(
                                                     diagnosticsAddressRow.modelData.address) >= 0
                                        // QStringList arrives as a copy, so the
                                        // chosen list is edited and assigned
                                        // back whole rather than pushed into.
                                        onToggled: {
                                            var chosen = appSettings.diagnosticsServerAddresses.slice()
                                            var at = chosen.indexOf(diagnosticsAddressRow.modelData.address)
                                            if (checked && at < 0)
                                                chosen.push(diagnosticsAddressRow.modelData.address)
                                            else if (!checked && at >= 0)
                                                chosen.splice(at, 1)
                                            appSettings.diagnosticsServerAddresses = chosen
                                        }
                                        ToolTip.visible: hovered
                                        ToolTip.text: diagnosticsAddressRow.modelData.description
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: "#91a0b1"
                                        text: diagnosticsAddressRow.modelData.interfaceName
                                              + (diagnosticsAddressRow.modelData.loopback
                                                 ? " — loopback, reachable only from this machine"
                                                 : " — reachable from that network")
                                    }
                                }
                            }
                        }
                    }
                    // Who may connect at all, as a list the operator can read
                    // line by line rather than as one comma-separated field.
                    // The rule is the strongest of the two gates and the
                    // cheapest -- a peer's address is known before a byte is
                    // exchanged -- so it is worth being able to see exactly
                    // what it says.
                    Label { text: "Allowed computers" }
                    ColumnLayout {
                        id: diagnosticsPeerEditor
                        Layout.fillWidth: true
                        spacing: 6

                        // The rows exactly as the operator is editing them.
                        // Held here rather than read straight from the setting
                        // because a row has to be allowed to be blank -- that
                        // is what "+ Add" produces -- and the setting drops a
                        // blank entry the moment it is written.
                        property var rows: []
                        // What the setting said when these rows were last
                        // seeded or committed. Compared on every settings
                        // change, so a list altered elsewhere -- a profile
                        // switch, a reset -- re-seeds the editor while the
                        // editor's own writes leave a blank row that is still
                        // being typed into alone.
                        property var storedRows: []
                        // True for exactly as long as commit() is writing.
                        // Without it the write's own settingsChanged would
                        // re-seed the rows from inside the commit that caused
                        // it, throwing away the blank row the operator is
                        // typing into and rebuilding every row twice.
                        property bool committing: false

                        function storedPeers() {
                            return appSettings.diagnosticsAllowedPeers.slice()
                        }
                        function sameRows(left, right) {
                            if (left.length !== right.length)
                                return false
                            for (var i = 0; i < left.length; ++i) {
                                if (("" + left[i]) !== ("" + right[i]))
                                    return false
                            }
                            return true
                        }
                        function reseed() {
                            diagnosticsPeerEditor.storedRows = diagnosticsPeerEditor.storedPeers()
                            diagnosticsPeerEditor.rows = diagnosticsPeerEditor.storedRows.slice()
                        }
                        // Writes the filled-in rows to the setting, then shows
                        // exactly what the setting kept, with the blank rows
                        // put back. An entry the setting dropped -- a
                        // duplicate, or one past the limit -- must not stay on
                        // screen looking as though it were in force.
                        //
                        // QStringList arrives in QML as a copy, so the whole
                        // list is assigned back rather than pushed into, the
                        // way the address checkboxes above do it.
                        function commit() {
                            var kept = []
                            var blanks = 0
                            for (var i = 0; i < diagnosticsPeerEditor.rows.length; ++i) {
                                var entry = ("" + diagnosticsPeerEditor.rows[i]).trim()
                                if (entry.length > 0)
                                    kept.push(entry)
                                else
                                    ++blanks
                            }
                            diagnosticsPeerEditor.committing = true
                            appSettings.diagnosticsAllowedPeers = kept
                            diagnosticsPeerEditor.committing = false
                            var shown = diagnosticsPeerEditor.storedPeers()
                            for (var blank = 0; blank < blanks; ++blank)
                                shown.push("")
                            diagnosticsPeerEditor.storedRows = diagnosticsPeerEditor.storedPeers()
                            if (!diagnosticsPeerEditor.sameRows(shown, diagnosticsPeerEditor.rows))
                                diagnosticsPeerEditor.rows = shown
                        }
                        function setRow(index, value) {
                            var next = diagnosticsPeerEditor.rows.slice()
                            next[index] = value
                            diagnosticsPeerEditor.rows = next
                            diagnosticsPeerEditor.commit()
                        }
                        function removeRow(index) {
                            var next = diagnosticsPeerEditor.rows.slice()
                            next.splice(index, 1)
                            diagnosticsPeerEditor.rows = next
                            diagnosticsPeerEditor.commit()
                        }
                        // Not committed: an empty row is not a rule, and the
                        // setting would drop it before the operator had typed
                        // anything into it.
                        function appendBlankRow() {
                            var next = diagnosticsPeerEditor.rows.slice()
                            next.push("")
                            diagnosticsPeerEditor.rows = next
                        }
                        function appendRow(value) {
                            var next = diagnosticsPeerEditor.rows.slice()
                            next.push(value)
                            diagnosticsPeerEditor.rows = next
                            diagnosticsPeerEditor.commit()
                        }
                        function hasRow(value) {
                            var wanted = ("" + value).trim().toLowerCase()
                            for (var i = 0; i < diagnosticsPeerEditor.rows.length; ++i) {
                                if (("" + diagnosticsPeerEditor.rows[i]).trim().toLowerCase() === wanted)
                                    return true
                            }
                            return false
                        }

                        // A hint, never the rule. DiagnosticsServer decides
                        // what an entry permits, and one it cannot read
                        // permits nothing -- so a typo narrows access rather
                        // than widening it. What it must not do is narrow it
                        // silently: an operator staring at a service that
                        // refuses everyone should have been told which line is
                        // the reason.
                        function ipv4LooksWellFormed(text, allowAbbreviated) {
                            var parts = text.split(".")
                            if (parts.length > 4)
                                return false
                            if (parts.length < 4 && !allowAbbreviated)
                                return false
                            for (var i = 0; i < parts.length; ++i) {
                                if (!/^[0-9]{1,3}$/.test(parts[i]))
                                    return false
                                if (parseInt(parts[i], 10) > 255)
                                    return false
                            }
                            return true
                        }
                        function ipv6LooksWellFormed(text) {
                            if (text.indexOf(":::") >= 0)
                                return false
                            if (text.split("::").length > 2)
                                return false
                            var groups = text.split(":")
                            if (groups.length < 3 && text.indexOf("::") < 0)
                                return false
                            for (var i = 0; i < groups.length; ++i) {
                                if (groups[i].length === 0)
                                    continue
                                if (groups[i].indexOf(".") >= 0) {
                                    // The trailing IPv4 form, which can only
                                    // be the last group.
                                    if (i !== groups.length - 1)
                                        return false
                                    if (!diagnosticsPeerEditor.ipv4LooksWellFormed(groups[i], false))
                                        return false
                                    continue
                                }
                                if (!/^[0-9A-Fa-f]{1,4}$/.test(groups[i]))
                                    return false
                            }
                            return true
                        }
                        function peerEntryIsReadable(text) {
                            var entry = ("" + text).trim()
                            // A row nobody has filled in yet is not a rule and
                            // is not marked as a broken one.
                            if (entry.length === 0)
                                return true
                            if (entry.toLowerCase() === "any")
                                return true
                            var slash = entry.indexOf("/")
                            var host = slash < 0 ? entry : entry.substring(0, slash)
                            if (host.length === 0)
                                return false
                            if (slash >= 0) {
                                var prefixText = entry.substring(slash + 1)
                                if (!/^[0-9]{1,3}$/.test(prefixText))
                                    return false
                                if (parseInt(prefixText, 10) > (host.indexOf(":") >= 0 ? 128 : 32))
                                    return false
                            }
                            if (host.indexOf(":") >= 0)
                                return diagnosticsPeerEditor.ipv6LooksWellFormed(host)
                            // The abbreviated IPv4 forms -- 10/8, 192.168/16 --
                            // are read only when a prefix is present, which is
                            // the same distinction the server makes.
                            return diagnosticsPeerEditor.ipv4LooksWellFormed(host, slash >= 0)
                        }

                        // Whether a ticked address is one only this machine can
                        // reach. The machine's own answer is preferred; the
                        // literal test is the fallback for an address that was
                        // ticked and has since gone away.
                        function addressIsLoopback(address) {
                            var known = appSettings.diagnosticsServerAvailableAddresses
                            for (var i = 0; i < known.length; ++i) {
                                if (known[i].address === address)
                                    return known[i].loopback === true
                            }
                            return address === "::1" || address.indexOf("127.") === 0
                        }
                        // The segment behind the first ticked address that
                        // reaches past this machine, or empty when there is
                        // none to offer. Nothing is added from it without the
                        // operator pressing the button, and what is added is a
                        // line in the list like any other.
                        readonly property string localNetworkCandidate: {
                            var chosen = appSettings.diagnosticsServerAddresses
                            for (var i = 0; i < chosen.length; ++i) {
                                if (diagnosticsPeerEditor.addressIsLoopback(chosen[i]))
                                    continue
                                var segment = appSettings.localNetworkForAddress(chosen[i])
                                if (segment && segment.length > 0)
                                    return segment
                            }
                            return ""
                        }

                        Component.onCompleted: diagnosticsPeerEditor.reseed()
                        Connections {
                            target: appSettings
                            function onSettingsChanged() {
                                // The editor's own write is finished by
                                // commit() itself. This is here for a list
                                // changed from somewhere else -- a profile
                                // switch, a reset -- which must be shown.
                                if (diagnosticsPeerEditor.committing)
                                    return
                                var stored = diagnosticsPeerEditor.storedPeers()
                                if (!diagnosticsPeerEditor.sameRows(
                                        stored, diagnosticsPeerEditor.storedRows))
                                    diagnosticsPeerEditor.reseed()
                            }
                        }

                        Label {
                            objectName: "diagnosticsAllowedPeersHintLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "This list is the computers allowed to connect — one per line, each a host such as 192.168.1.50, a network such as 192.168.1.0/24, or the word any — and an empty list means only this computer."
                        }
                        ColumnLayout {
                            objectName: "diagnosticsAllowedPeersList"
                            Layout.fillWidth: true
                            spacing: 4
                            Repeater {
                                model: diagnosticsPeerEditor.rows
                                delegate: RowLayout {
                                    id: diagnosticsPeerRow
                                    required property int index
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 8
                                    readonly property bool entryIsReadable:
                                        diagnosticsPeerEditor.peerEntryIsReadable(
                                            diagnosticsPeerField.text)
                                    TextField {
                                        id: diagnosticsPeerField
                                        objectName: "diagnosticsPeerField"
                                        Layout.fillWidth: true
                                        text: "" + diagnosticsPeerRow.modelData
                                        placeholderText: "192.168.1.0/24, 192.168.1.50, or any"
                                        inputMethodHints: Qt.ImhNoPredictiveText
                                        // The style's own text colour while the
                                        // line is readable, so only a line that
                                        // permits nobody is tinted.
                                        color: diagnosticsPeerRow.entryIsReadable
                                               ? diagnosticsPeerField.palette.text
                                               : "#f3bd55"
                                        onEditingFinished: diagnosticsPeerEditor.setRow(
                                                               diagnosticsPeerRow.index, text)
                                    }
                                    Label {
                                        objectName: "diagnosticsPeerValidityLabel"
                                        Layout.preferredWidth: 230
                                        wrapMode: Text.WordWrap
                                        color: "#f3bd55"
                                        visible: !diagnosticsPeerRow.entryIsReadable
                                        text: "Not a host, a network or the word any. This line permits nobody."
                                    }
                                    Button {
                                        objectName: "diagnosticsRemovePeerButton"
                                        text: "Remove"
                                        onClicked: diagnosticsPeerEditor.removeRow(
                                                       diagnosticsPeerRow.index)
                                        ToolTip.visible: hovered
                                        ToolTip.text: "Delete this line. With no lines left, only this computer may connect."
                                    }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Button {
                                objectName: "diagnosticsAddPeerButton"
                                text: "+ Add"
                                onClicked: diagnosticsPeerEditor.appendBlankRow()
                                ToolTip.visible: hovered
                                ToolTip.text: "Add an empty line to type a host or a network into."
                            }
                            Button {
                                objectName: "diagnosticsAddLocalNetworkButton"
                                text: "Add this network"
                                enabled: diagnosticsPeerEditor.localNetworkCandidate.length > 0
                                         && !diagnosticsPeerEditor.hasRow(
                                                diagnosticsPeerEditor.localNetworkCandidate)
                                onClicked: diagnosticsPeerEditor.appendRow(
                                               diagnosticsPeerEditor.localNetworkCandidate)
                                ToolTip.visible: hovered
                                ToolTip.text: "Put the network of the first ticked address into the list above, as an ordinary line."
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: "#91a0b1"
                                text: diagnosticsPeerEditor.localNetworkCandidate.length === 0
                                      ? "Add this network needs an address ticked above that is not loopback; it then offers the network that address sits on."
                                      : (diagnosticsPeerEditor.hasRow(diagnosticsPeerEditor.localNetworkCandidate)
                                         ? diagnosticsPeerEditor.localNetworkCandidate + " is already in the list."
                                         : "Adds " + diagnosticsPeerEditor.localNetworkCandidate + " as a line you can read, edit or delete.")
                            }
                        }
                    }
                    Label { text: "Port" }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        TextField {
                            id: diagnosticsPortField
                            objectName: "diagnosticsPortField"
                            Layout.preferredWidth: 120
                            text: String(appSettings.diagnosticsServerPort)
                            inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhNoPredictiveText
                            validator: IntValidator { bottom: 1; top: 65535 }
                            onEditingFinished: {
                                if (acceptableInput)
                                    appSettings.diagnosticsServerPort = parseInt(text, 10)
                                else
                                    text = String(appSettings.diagnosticsServerPort)
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "The same port is used on every ticked address. 17300 is the default, chosen clear of the ports amateur software already claims — rigctld on 4532, cluster nodes on 7300, 7373 and 8000."
                        }
                    }
                    Label { text: "Access token" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            TextField {
                                id: diagnosticsTokenField
                                objectName: "diagnosticsTokenField"
                                Layout.fillWidth: true
                                text: appSettings.diagnosticsServerToken
                                placeholderText: "Required before a non-loopback address can be bound"
                                inputMethodHints: Qt.ImhNoPredictiveText
                                                  | Qt.ImhSensitiveData
                                onEditingFinished: appSettings.diagnosticsServerToken = text
                            }
                            Button {
                                objectName: "diagnosticsGenerateTokenButton"
                                text: "Generate"
                                onClicked: {
                                    var token = appSettings.generateDiagnosticsToken()
                                    if (token && token.length > 0)
                                        appSettings.diagnosticsServerToken = token
                                    diagnosticsTokenField.text = appSettings.diagnosticsServerToken
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: "Replace the token with a fresh random one. Readers holding the old token are cut off when the service restarts."
                            }
                        }
                        Label {
                            objectName: "diagnosticsTokenHintLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: appSettings.diagnosticsServerToken.length > 0
                                   ? "#91a0b1" : "#f3bd55"
                            text: appSettings.diagnosticsServerToken.length > 0
                                  ? "A reader must present this token before any record is streamed to it."
                                  : "Generate a token. A loopback address can be bound without one, but no other address can, and a token short enough to guess is a token that was never asked for."
                        }
                    }
                    Label { text: "Service state" }
                    Label {
                        objectName: "diagnosticsServiceStateLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: diagnosticsServer.listening ? "#43c6ac" : "#91a0b1"
                        text: (diagnosticsServer.statusMessage || "").length > 0
                              ? diagnosticsServer.statusMessage
                              : (appSettings.diagnosticsServerEnabled
                                 ? "Starting." : "Not running.")
                    }

                    Rectangle {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: "#2b3541"
                    }
                    // Stated once, plainly, in body text: this is what the
                    // operator is choosing, and it is not a decoration around
                    // the controls that choose it.
                    Label {
                        objectName: "diagnosticsExposureLabel"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Binding this to anything but a loopback address makes the station readable from that network. The frequencies it is tuned to, the callsigns it decodes, the identifiers of the audio and SDR devices it is using, and the decoded transcript all travel in the stream, unencrypted, to anything that can reach the address and holds the token."
                    }
                    Label {
                        objectName: "diagnosticsTokenLimitLabel"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "A client must present this token, as one line, before it is sent anything at all; one that presents the wrong token or none receives no record. It is still a single shared secret, so it cannot tell one reader from another or shut out just one — changing it cuts off everybody at once. The stream is not encrypted, so the token and everything it protects cross the network in clear. Use the allowed-peers list above as the first gate and pick a network you would be willing to let read the station."
                    }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 12
                    anchors.margins: 22
                    Label { text: "Hardware keying" }
                    CheckBox {
                        text: "Enable direct RTS/DTR keying"
                        checked: appSettings.directKeyingEnabled
                        enabled: appSettings.radioEnabled
                        onToggled: appSettings.directKeyingEnabled = checked
                        ToolTip.visible: hovered
                        ToolTip.text: enabled
                            ? "Opt in to the dedicated local serial KEY/PTT adapter; every reconnect starts disarmed"
                            : "Enable a radio profile before configuring transmit hardware"
                    }
                    Label { text: "Direct key/PTT port" }
                    ComboBox {
                        Layout.fillWidth: true
                        editable: true
                        model: appSettings.serialPorts
                        currentIndex: find(appSettings.keyingPort)
                        displayText: currentIndex >= 0 ? currentText : appSettings.keyingPort
                        onActivated: appSettings.keyingPort = currentText
                        onAccepted: appSettings.keyingPort = editText
                    }
                    Label { text: "PTT line" }
                    ComboBox { model: ["RTS", "DTR"]; currentIndex: appSettings.pttLineIndex; onActivated: appSettings.pttLineIndex = currentIndex }
                    Label { text: "KEY line" }
                    ComboBox { model: ["RTS", "DTR"]; currentIndex: appSettings.keyLineIndex; onActivated: appSettings.keyLineIndex = currentIndex }
                    Label { text: "PTT polarity" }
                    CheckBox { text: checked ? "Active high" : "Active low"; checked: appSettings.pttActiveHigh; onToggled: appSettings.pttActiveHigh = checked }
                    Label { text: "KEY polarity" }
                    CheckBox { text: checked ? "Active high" : "Active low"; checked: appSettings.keyActiveHigh; onToggled: appSettings.keyActiveHigh = checked }
                    Label { text: "Hardware validation" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        enabled: appSettings.directKeyingEnabled
                                 && appSettings.keyingPort.length > 0
                        CheckBox {
                            id: radioDisconnectedForLoopback
                            objectName: "radioDisconnectedForLoopbackCheck"
                            text: "Radio is physically disconnected; RTS→CTS and DTR→DSR loopbacks are fitted"
                        }
                        Button {
                            objectName: "runDirectKeyingLoopbackButton"
                            text: "Run measured loopback"
                            enabled: radioDisconnectedForLoopback.checked
                            onClicked: {
                                appSettings.runDirectKeyingLoopback(
                                    radioDisconnectedForLoopback.checked)
                                radioDisconnectedForLoopback.checked = false
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: "Test the exact selected port electrically; outputs are released and the port is closed on every result"
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
                        ToolTip.text: currentIndex === 0
                            ? "Snapshot a supported selected-stream WPM when preparing text; otherwise use the fixed fallback"
                            : "Always use the configured fixed transmit speed"
                    }
                    Label { text: "Fixed/fallback TX speed" }
                    LabeledSlider {
                        Layout.fillWidth: true
                        caption: "WPM"
                        from: 5
                        to: 80
                        stepSize: 1
                        value: appSettings.fixedTxWpm
                        onMoved: value => appSettings.fixedTxWpm = Math.round(value)
                    }
                    Label { text: "Quick TX macros" }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        TextField { Layout.fillWidth: true; maximumLength: 64; text: appSettings.txMacro1; placeholderText: "Macro 1"; onEditingFinished: appSettings.txMacro1 = text }
                        TextField { Layout.fillWidth: true; maximumLength: 64; text: appSettings.txMacro2; placeholderText: "Macro 2"; onEditingFinished: appSettings.txMacro2 = text }
                        TextField { Layout.fillWidth: true; maximumLength: 64; text: appSettings.txMacro3; placeholderText: "Macro 3"; onEditingFinished: appSettings.txMacro3 = text }
                        TextField { Layout.fillWidth: true; maximumLength: 64; text: appSettings.txMacro4; placeholderText: "Macro 4"; onEditingFinished: appSettings.txMacro4 = text }
                    }
                    Label { text: "" }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#f3bd55"
                        text: "Ports are enumerated without opening them. The first hardware slice accepts distinct RTS/DTR lines with active-high interfaces only. Opening or changing the adapter always drives KEY then PTT inactive and leaves transmission disarmed. Validate with the radio disconnected, then a physical loopback, before using a dummy load."
                    }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 12
                    anchors.margins: 22
                    Label { text: "Waterfall rendering" }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        CheckBox {
                            objectName: "waterfallRenderingCheck"
                            text: "Draw the spectrum and waterfall"
                            checked: appSettings.waterfallRenderingEnabled
                            onToggled: appSettings.waterfallRenderingEnabled = checked
                            ToolTip.visible: hovered
                            ToolTip.text: "Turn off to stop composing, retaining and painting waterfall rows. Reception, decoding, logging and the diagnostics stream are unaffected."
                        }
                        Label {
                            objectName: "waterfallRenderingNoteLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "A resource control rather than a cosmetic one. With it off the display is detached from the frame feed, so no waterfall row is conditioned, stored or painted and the retained history is released; a station left running as a diagnostics server does not need to draw a waterfall. Every other control on this tab applies again once rendering is back on, and the history restarts from empty."
                        }
                    }
                    Label { text: "Spectrum view" }
                    ComboBox {
                        model: ["Audio spectrum", "CW symbols"]
                        currentIndex: appSettings.spectrumDisplayMode
                        onActivated: appSettings.spectrumDisplayMode = currentIndex
                    }
                    Label { text: "Target FPS" }
                    LabeledSlider { Layout.fillWidth: true; caption: "frames/s"; from: 10; to: 120; value: appSettings.targetFps; onMoved: value => appSettings.targetFps = Math.round(value) }
                    Label { text: "Waterfall lines / second" }
                    LabeledSlider { Layout.fillWidth: true; caption: "rows/s"; from: 1; to: 120; value: appSettings.waterfallRate; onMoved: value => appSettings.waterfallRate = Math.round(value) }
                    Label { text: "Waterfall history (seconds)" }
                    LabeledSlider { Layout.fillWidth: true; caption: "seconds"; from: 5; to: 30; value: appSettings.waterfallTimeSpanSeconds; onMoved: value => appSettings.waterfallTimeSpanSeconds = Math.round(value) }
                    Label { text: "Visual CW reference" }
                    CheckBox { text: "Show red visual boundaries"; checked: appSettings.showCwGuide; onToggled: appSettings.showCwGuide = checked }
                    Label { text: "Visual center tone (Hz)" }
                    LabeledSlider { Layout.fillWidth: true; caption: "Hz"; from: 0; to: appSettings.audioAutomaticBandwidth ? 3000 : Math.max(3000, appSettings.audioUpperFrequencyHz); stepSize: 10; value: appSettings.cwGuideCenterHz; enabled: appSettings.showCwGuide; onMoved: value => appSettings.cwGuideCenterHz = value }
                    Label { text: "Visual width (Hz)" }
                    LabeledSlider { Layout.fillWidth: true; caption: "Hz"; from: 10; to: 5000; stepSize: 10; value: appSettings.cwGuideWidthHz; enabled: appSettings.showCwGuide; onMoved: value => appSettings.cwGuideWidthHz = value }
                    Label { text: "Display level range" }
                    CheckBox { text: "Automatic display scaling"; checked: appSettings.automaticRange; onToggled: appSettings.automaticRange = checked }
                    Label { text: "Lower bound (dB)" }
                    LabeledSlider { Layout.fillWidth: true; caption: "dBFS"; from: -200; to: 40; value: appSettings.lowerBoundDb; enabled: !appSettings.automaticRange; onMoved: value => appSettings.lowerBoundDb = value }
                    Label { text: "Upper bound (dB)" }
                    LabeledSlider { Layout.fillWidth: true; caption: "dBFS"; from: -190; to: 50; value: appSettings.upperBoundDb; enabled: !appSettings.automaticRange; onMoved: value => appSettings.upperBoundDb = value }
                    Label { text: "Automatic span (dB)" }
                    LabeledSlider { Layout.fillWidth: true; caption: "dB"; from: 30; to: 100; value: appSettings.automaticRangeSpanDb; enabled: appSettings.automaticRange; onMoved: value => appSettings.automaticRangeSpanDb = value }
                    Label { text: "Waterfall noise suppression" }
                    CheckBox { text: "Darken bins near the measured noise floor"; checked: appSettings.waterfallNoiseSuppression; onToggled: appSettings.waterfallNoiseSuppression = checked }
                    Label { text: "Noise margin (dB)" }
                    LabeledSlider { Layout.fillWidth: true; caption: "dB"; from: 0; to: 30; value: appSettings.waterfallNoiseMarginDb; enabled: appSettings.waterfallNoiseSuppression && appSettings.spectrumDisplayMode === 0; onMoved: value => appSettings.waterfallNoiseMarginDb = value }
                    Label { text: "Spectrum averaging" }
                    LabeledSlider { Layout.fillWidth: true; caption: "frames"; from: 1; to: 32; value: appSettings.averagingFrames; onMoved: value => appSettings.averagingFrames = Math.round(value) }
                    Label { text: "Reference grid" }
                    CheckBox { text: "Show frequency and level grid"; checked: appSettings.showGrid; onToggled: appSettings.showGrid = checked }
                    Label { text: "Spectrum help" }
                    CheckBox {
                        objectName: "showSpectrumGestureHintsCheck"
                        text: "Show brief spectrum gesture hints"
                        checked: appSettings.showSpectrumGestureHints
                        onToggled: appSettings.showSpectrumGestureHints = checked
                        ToolTip.visible: hovered
                        ToolTip.text: "Show the spectrum pointer legend for at most 10 seconds, no more than once every five minutes"
                    }
                    Label { text: "Decoded signal timeout" }
                    LabeledSlider { Layout.fillWidth: true; caption: "seconds"; from: 5; to: 300; value: appSettings.decodedSignalTimeoutSeconds; onMoved: value => appSettings.decodedSignalTimeoutSeconds = Math.round(value) }
                    Label { text: "" }
                    Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#91a0b1"; text: "Waterfall history is a constant time window: resizing, startup fill, and line density do not stretch or collapse Morse timing. Automatic display scaling uses a stable minimum span so receiver noise stays dark instead of pumping through the palette. Noise suppression affects waterfall colors only; raw spectrum bins remain available to the future decoder. The same operational controls are available directly below the spectrum. A decoded signal's marker and session remain available for the configured timeout after it goes silent, then are removed. Its frequency keeps the same reserved color for at least five minutes so a later pass is visually recognizable." }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    anchors.margins: 22
                    spacing: 14
                    Label { text: "Station configuration profile"; font.pixelSize: 17; font.weight: Font.DemiBold }
                    Label { text: appSettings.profileName; font.pixelSize: 15 }
                    Label { text: "Own station callsign"; font.weight: Font.DemiBold }
                    TextField {
                        objectName: "ownCallsignField"
                        Layout.fillWidth: true
                        text: appSettings.ownCallsign
                        placeholderText: "Example: IU0LFQ or AD2FC"
                        maximumLength: 16
                        inputMethodHints: Qt.ImhUppercaseOnly | Qt.ImhNoPredictiveText
                        onEditingFinished: appSettings.ownCallsign = text
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Saved per station profile. An exact stable decode highlights your callsign and flashes its open decoder card. The value also populates station logging fields; any future closing macro remains separately guarded."
                    }
                    Label { text: "Operating role"; font.weight: Font.DemiBold }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RadioButton {
                            objectName: "operatorRoleMonitorRadio"
                            text: "Monitoring"
                            checked: appSettings.operatorRole !== "search-and-pounce"
                                     && appSettings.operatorRole !== "runner"
                            onToggled: if (checked) appSettings.operatorRole = "monitor"
                        }
                        RadioButton {
                            objectName: "operatorRoleSearchAndPounceRadio"
                            text: "Search and pounce"
                            checked: appSettings.operatorRole === "search-and-pounce"
                            onToggled: if (checked) appSettings.operatorRole = "search-and-pounce"
                        }
                        RadioButton {
                            objectName: "operatorRoleRunnerRadio"
                            text: "Running"
                            checked: appSettings.operatorRole === "runner"
                            onToggled: if (checked) appSettings.operatorRole = "runner"
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                            wrapMode: Text.WordWrap
                            color: "#91a0b1"
                            text: "Which station a stream is expected to carry. Exchange context alone cannot always tell: TU precedes a runner identifying itself and equally the station it has just worked. Hunting, the stream you are listening to is a runner, so its own call is the label; running, it is somebody answering you. Monitoring makes no assumption. Your own callsign never labels another station's stream in any of them."
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Open Profiles from the main toolbar to create or select another station. For dedicated shortcuts or services, launch with --profile \"name\". Separate processes can use separate profiles and radios."
                    }
                    Button {
                        text: "Run setup helper again"; onClicked: root.setupRequested()
                        ToolTip.visible: hovered
                        ToolTip.text: "Open the guided station-profile setup without transmitting"
                    }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    anchors.margins: 22
                    spacing: 14
                    Label { text: "About CW Buddy"; font.pixelSize: 22; font.weight: Font.DemiBold }
                    Label {
                        objectName: "aboutVersionLabel"
                        text: "Version " + Qt.application.version
                        color: "#91a0b1"
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: "#2b3541" }
                    Label { text: "Updates"; font.pixelSize: 17; font.weight: Font.DemiBold }
                    CheckBox {
                        objectName: "autoUpdateCheckToggle"
                        text: "Automatically check for updates"
                        checked: updateChecker.autoCheckEnabled
                        onToggled: updateChecker.autoCheckEnabled = checked
                    }
                    RowLayout {
                        spacing: 10
                        Button {
                            objectName: "checkForUpdatesButton"
                            text: updateChecker.checking ? "Checking…" : "Check for updates"
                            enabled: !updateChecker.checking
                            onClicked: updateChecker.checkForUpdates()
                            ToolTip.visible: hovered
                            ToolTip.text: "Check the published release manifest for a newer application version"
                        }
                        Label {
                            text: "Last checked: " + updateChecker.lastCheckedText
                            color: "#6c7c8e"
                            font.pixelSize: 11
                        }
                    }
                    Label {
                        objectName: "updateStatusLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: updateChecker.statusMessage
                        color: updateChecker.updateAvailable ? "#4dff88" : "#91a0b1"
                    }
                    RowLayout {
                        objectName: "updateActionRow"
                        visible: updateChecker.updateActionVisible
                        spacing: 10
                        Button {
                            objectName: "downloadUpdateButton"
                            visible: updateChecker.downloadActionVisible
                            text: updateChecker.downloading
                                  ? "Downloading… " + Math.round(updateChecker.downloadProgress * 100) + "%"
                                  : "Download update"
                            enabled: !updateChecker.downloading
                            onClicked: updateChecker.downloadUpdate()
                            ToolTip.visible: hovered
                            ToolTip.text: "Download this platform package and verify its SHA-256 checksum"
                        }
                        Button {
                            objectName: "openUpdateButton"
                            visible: updateChecker.verifiedDownloadActionsVisible
                            text: "Open Installer"
                            onClicked: updateChecker.openDownloadedFile()
                            ToolTip.visible: hovered
                            ToolTip.text: "Open the verified package with the operating-system installer"
                        }
                        Button {
                            objectName: "revealUpdateButton"
                            visible: updateChecker.verifiedDownloadActionsVisible
                            text: Qt.platform.os === "osx" ? "Show in Finder"
                                  : Qt.platform.os === "windows"
                                    ? "Show in File Explorer"
                                    : "Show in Folder"
                            flat: true
                            onClicked: updateChecker.revealDownloadFolder()
                            ToolTip.visible: hovered
                            ToolTip.text: "Show the verified package in the file manager"
                        }
                    }
                    Label {
                        visible: updateChecker.updateAvailable && !updateChecker.platformSupported
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#f3bd55"
                        text: "Guided downloads are not available for this platform yet; visit the release page instead."
                        font.pixelSize: 11
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: "#2b3541" }
                    Label { text: "Author"; color: "#8290a0" }
                    Label { text: "Alessio Bravi (IU0LFQ / AD2FC)"; font.pixelSize: 17; font.weight: Font.Medium }
                    Label { text: "Author Website"; color: "#8290a0" }
                    Button {
                        text: "https://iu0lfq.it/"
                        flat: true
                        onClicked: Qt.openUrlExternally("https://iu0lfq.it/")
                        ToolTip.visible: hovered
                        ToolTip.text: "Open the author website in your default browser"
                    }
                    Label { text: "License"; color: "#8290a0" }
                    Label { text: "GNU General Public License v3.0 or later"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#91a0b1"
                        text: "Cross-platform multichannel amateur-radio CW receiver and operator assistant."
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#2b3541" }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            Label { text: appSettings.statusMessage; color: "#91a0b1"; Layout.fillWidth: true; elide: Text.ElideRight }
            Button {
                text: "Apply"; highlighted: true; onClicked: appSettings.apply()
                ToolTip.visible: hovered
                ToolTip.text: "Validate and save all settings in this profile"
            }
        }
    }

    Connections {
        target: appSettings
        // Discovery publishes everything it found through this one
        // notification, which is the only observable end of a scan available
        // today. Clearing on it can end the waiting state early if an
        // unrelated setting changes mid-scan; it can never leave it running
        // after the results are in.
        function onSdrSettingsChanged() {
            root.sdrDiscoveryRunning = false
        }
    }

    FileDialog {
        id: localDecoderModelDialog
        objectName: "localDecoderModelDialog"
        title: "Select local decoder model"
        fileMode: FileDialog.OpenFile
        nameFilters: ["ONNX models (*.onnx)", "All files (*)"]
        onAccepted: appSettings.selectLocalDecoderModel(selectedFile)
    }

    FileDialog {
        id: localDecoderMetadataDialog
        objectName: "localDecoderMetadataDialog"
        title: "Select local decoder metadata"
        fileMode: FileDialog.OpenFile
        nameFilters: ["JSON metadata (*.json)", "All files (*)"]
        onAccepted: appSettings.selectLocalDecoderMetadata(selectedFile)
    }

    FileDialog {
        id: localCallsignDatabaseDialog
        objectName: "localCallsignDatabaseDialog"
        title: "Select local callsign list"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Callsign lists (master.scp *.scp *.txt *.csv)",
                      "All files (*)"]
        onAccepted: appSettings.selectLocalCallsignDatabase(selectedFile)
    }
}
