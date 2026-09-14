import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Dialogs
import CWBuddy 1.0

ApplicationWindow {
    id: window
    width: 1720
    height: 1040
    minimumWidth: 1180
    minimumHeight: 720
    visible: true
    title: "CW BUDDY " + Qt.application.version + " — "
           + appSettings.profileName
    color: "#0d1117"
    Material.theme: Material.Dark
    Material.accent: "#43c6ac"

    function formatFrequency(hz) {
        if (Math.abs(hz) >= 1000000)
            return (hz / 1000000).toFixed(3) + " MHz"
        if (Math.abs(hz) >= 10000)
            return (hz / 1000).toFixed(1) + " kHz"
        return hz.toFixed(0) + " Hz"
    }

    // A VFO-style readout deserves the same decimal/centesimal precision a
    // real rig's display has (e.g. 7016.45 kHz on 40 m), which the coarser
    // formatFrequency() above deliberately does not provide for spectrum
    // axis labels.
    function formatVfoFrequency(hz) {
        if (Math.abs(hz) >= 30000000)
            return (hz / 1000000).toFixed(5) + " MHz"
        return (hz / 1000).toFixed(2) + " kHz"
    }

    function formatRigFrequency(hz) {
        var digits = Math.max(0, Math.round(hz)).toString()
        var grouped = []
        while (digits.length > 3) {
            grouped.unshift(digits.slice(-3))
            digits = digits.slice(0, -3)
        }
        grouped.unshift(digits)
        return grouped.join(".")
    }

    // The controller composes frequencyLabel from raw hertz ("14019655 Hz RF"),
    // which no operator reads at a glance. Present an RF stream the way the VFO
    // readout presents a frequency, and leave an audio tone in plain hertz,
    // where grouping would only invent structure that is not there.
    function formatStreamFrequency(channel) {
        var hz = Number(channel.displayFrequencyHz)
        if (!Number.isFinite(hz))
            return channel.frequencyLabel
        return channel.frequencyKind === "RF"
                ? formatRigFrequency(hz) + " RF"
                : hz.toFixed(0) + " Hz AF"
    }

    function formatVfoInput(hz, unitHz) {
        var decimals = unitHz === 1000000 ? 6 : 3
        return (hz / unitHz).toFixed(decimals)
                .replace(/0+$/, "").replace(/\.$/, "")
    }

    // Maps a spectrum frequency to its horizontal pixel position within the
    // spectrumDisplay item, shared by the TX-slice marker and the
    // verified-signal area highlights so both stay pixel-aligned.
    function hzToX(hz) {
        return spectrumDisplay.x
               + (hz - spectrumDisplay.lowerFrequencyHz)
                 / (spectrumDisplay.upperFrequencyHz
                    - spectrumDisplay.lowerFrequencyHz)
                 * spectrumDisplay.width
    }

    function verificationDiagnosticsSummary(diagnostics) {
        if (!diagnostics || typeof diagnostics.candidateTracks === "undefined")
            return ""
        var parts = []
        if (diagnostics.candidateTracks > 0)
            parts.push(diagnostics.candidateTracks + " candidate")
        if (diagnostics.morseLikelyTracks > 0)
            parts.push(diagnostics.morseLikelyTracks + " morse-likely")
        if (parts.length === 0)
            return "No pre-verification candidates right now."
        var reasons = diagnostics.reasonCounts || {}
        var reasonParts = []
        for (var key in reasons) {
            if (key === "verified" || key === "signal-lost")
                continue
            reasonParts.push(key + " " + reasons[key])
        }
        var text = parts.join(", ") + " not yet verified"
        if (reasonParts.length > 0)
            text += "  •  " + reasonParts.join(", ")
        return text
    }

    function exactCallCount(text, callsign) {
        var wanted = String(callsign).trim().toUpperCase()
        if (wanted.length === 0)
            return 0
        var tokens = String(text).toUpperCase().match(/[A-Z0-9/]+/g) || []
        var count = 0
        for (var index = 0; index < tokens.length; ++index) {
            if (tokens[index] === wanted)
                ++count
        }
        return count
    }

    header: ToolBar {
        height: 64
        background: Rectangle { color: "#151b23" }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 16
            spacing: 16
            ColumnLayout {
                spacing: -2
                Label {
                    text: "CW BUDDY"
                    font.pixelSize: 20
                    font.weight: Font.Bold
                    font.letterSpacing: 0.8
                }
                Label {
                    text: "by IU0LFQ"
                    color: "#8d9aaa"
                    font.pixelSize: 10
                    font.italic: true
                    Layout.alignment: Qt.AlignHCenter
                }
            }
            Label {
                text: "v" + Qt.application.version
                color: "#718092"
                font.pixelSize: 11
                font.family: "monospace"
            }
            Rectangle { width: 1; height: 28; color: "#303a46" }
            ColumnLayout {
                spacing: 0
                Label { text: appSettings.profileName; font.pixelSize: 14 }
                Label { text: appSettings.radioDisplayName; color: "#8d9aaa"; font.pixelSize: 11 }
            }
            Item { Layout.fillWidth: true }
            // Cluster link state, immediately left of the transmit chip so the
            // two read as one status pair. It is deliberately built from the
            // transmit chip's own parts — same height, radius, plate colour,
            // 11 px bold label — and it borrows the amber that chip uses for
            // "armed", but never its red: on this bar red means the
            // transmitter is keyed, and a receive-only link must never be
            // mistaken for one.
            Rectangle {
                id: dxClusterChip
                objectName: "dxClusterStatusChip"
                // Reserve the widest state's width so the transmit chip beside
                // it does not slide sideways every time the link changes.
                implicitWidth: Math.max(dxClusterLabel.implicitWidth,
                                        dxClusterWidest.width) + 24
                implicitHeight: 30
                radius: 15
                color: "#202833"
                readonly property bool linkEnabled: appSettings.dxClusterEnabled
                readonly property bool linkUp:
                    dxClusterChip.linkEnabled
                    && replayController.dxClusterConnected
                // Kept on screen and greyed when the feature is off rather
                // than hidden. An empty spectrum is exactly the moment an
                // operator asks why no spots are drawn, and a chip that has
                // vanished answers nothing; the transmit chip beside it makes
                // the same choice when it says "TX DISARMED".
                ToolTip.visible: dxClusterHover.hovered
                ToolTip.delay: 400
                // Defended with || "" so the chip still renders its own
                // wording in the moment before the controller has published a
                // status line of its own.
                readonly property string linkStatus:
                    replayController.dxClusterStatus || ""
                ToolTip.text: dxClusterChip.linkStatus.length > 0
                              ? dxClusterChip.linkStatus
                              : (dxClusterChip.linkEnabled
                                 ? "Joining the selected cluster node."
                                 : "Cluster spots are switched off in Settings > Decoder > Cluster link.")
                HoverHandler { id: dxClusterHover }
                TextMetrics {
                    id: dxClusterWidest
                    font: dxClusterLabel.font
                    text: "CLUSTER LINKING"
                }
                Label {
                    id: dxClusterLabel
                    anchors.centerIn: parent
                    text: !dxClusterChip.linkEnabled ? "CLUSTER OFF"
                          : dxClusterChip.linkUp ? "CLUSTER LINKED"
                          : "CLUSTER LINKING"
                    // Grey off, amber while it is trying, and the application
                    // accent teal once spots are arriving. Never "#ff6b6b".
                    color: !dxClusterChip.linkEnabled ? "#718092"
                           : dxClusterChip.linkUp ? "#43c6ac" : "#f3bd55"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }
            }
            // Diagnostics service state, immediately right of the cluster
            // chip and deliberately built from the same parts — same height,
            // radius, plate colour and 11 px bold label — so the two read as
            // siblings rather than as two different kinds of thing. Amber: the
            // teal beside it means cluster spots are arriving, and on this bar
            // "#ff6b6b" means the transmitter is keyed, so neither is
            // available to a diagnostics stream.
            Rectangle {
                id: diagnosticsChip
                objectName: "diagnosticsStatusChip"
                readonly property bool serviceListening: diagnosticsServer.listening
                readonly property int readers: diagnosticsServer.clientCount
                // Defended with || "" so the chip still renders its own
                // wording before the service has published a status line.
                readonly property string serviceStatus:
                    diagnosticsServer.statusMessage || ""
                // A service the operator has forgotten is running is the one
                // that will surprise them, so this stays on the bar for as
                // long as the service is up — and while it is coming up or
                // failing to, because an enabled service that never bound is
                // exactly as worth seeing.
                visible: diagnosticsChip.serviceListening
                         || appSettings.diagnosticsServerEnabled
                // Reserve the widest state so the transmit chip beside it does
                // not slide sideways as readers attach and detach.
                implicitWidth: Math.max(diagnosticsLabel.implicitWidth,
                                        diagnosticsWidest.width) + 24
                implicitHeight: 30
                radius: 15
                color: "#202833"
                ToolTip.visible: diagnosticsHover.hovered
                ToolTip.delay: 400
                ToolTip.text: diagnosticsChip.serviceStatus.length > 0
                              ? diagnosticsChip.serviceStatus
                              : (diagnosticsChip.serviceListening
                                 ? "Diagnostics are being streamed. The stream is emit-only and carries station state in clear text."
                                 : "The diagnostics service is switched on in Settings > Network but is not listening.")
                HoverHandler { id: diagnosticsHover }
                TextMetrics {
                    id: diagnosticsWidest
                    font: diagnosticsLabel.font
                    text: "DIAG STARTING"
                }
                Label {
                    id: diagnosticsLabel
                    anchors.centerIn: parent
                    text: !diagnosticsChip.serviceListening ? "DIAG STARTING"
                          : diagnosticsChip.readers > 0
                            ? "DIAG LIVE " + diagnosticsChip.readers
                            : "DIAG LIVE"
                    // Amber while it is up, dimmed amber while it is not, so
                    // "on the air with diagnostics" is never read as TX.
                    color: diagnosticsChip.serviceListening ? "#f3bd55" : "#9c7a3a"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }
            }
            // Receive audio leaving the station, and the switch that allows it.
            //
            // On the bar rather than only in Settings, and clickable, because
            // this is the one disclosure an operator most needs to be able to
            // see and revoke without going and finding a page. Audio carries
            // every signal in the passband, not only what this application
            // decoded, so the chip is visible whenever the diagnostics service
            // is up -- including while audio is off, which is the state that
            // says plainly that it is off.
            //
            // Amber like its sibling while it is merely allowed; the
            // application accent teal only while audio is actually going out,
            // because "permitted" and "leaving this machine right now" are
            // different facts and the second one is the urgent one. Still never
            // "#ff6b6b", which on this bar means the transmitter is keyed.
            Rectangle {
                id: audioStreamChip
                objectName: "audioStreamStatusChip"
                readonly property bool serviceListening: diagnosticsServer.listening
                readonly property bool allowed: diagnosticsServer.audioStreamingEnabled
                // Allowed is not the same as able. The audio half binds the
                // same port as the control half, over UDP, and a port it could
                // not get must never read as a service that is ready.
                readonly property bool ready: diagnosticsServer.audioListening
                readonly property int listeners: diagnosticsServer.audioSubscriberCount
                visible: audioStreamChip.serviceListening
                         || audioStreamChip.allowed
                implicitWidth: Math.max(audioStreamLabel.implicitWidth,
                                        audioStreamWidest.width) + 24
                implicitHeight: 30
                radius: 15
                color: "#202833"
                ToolTip.visible: audioStreamHover.hovered
                ToolTip.delay: 400
                ToolTip.text: audioStreamChip.listeners > 0
                              ? "Receive audio is being sent to " + audioStreamChip.listeners + " remote observer(s), unencrypted, over UDP on the diagnostics port. Click to stop it."
                              : (audioStreamChip.allowed && !audioStreamChip.ready)
                                ? "Receive audio is allowed but this station could not bind the audio port, so none can be sent. Click to withdraw permission."
                                : audioStreamChip.allowed
                                  ? "Remote observers may ask for receive audio. Nothing is being sent yet. Click to withdraw permission."
                                  : "Receive audio stays on this machine. Click to allow remote observers to ask for it."
                HoverHandler { id: audioStreamHover }
                TextMetrics {
                    id: audioStreamWidest
                    font: audioStreamLabel.font
                    text: "AUDIO OUT 4"
                }
                // Withdrawing needs no confirmation and takes effect at once;
                // only granting is asked about, because only granting is the
                // one that cannot be taken back from whoever already heard it.
                TapHandler {
                    onTapped: audioStreamChip.allowed
                              ? diagnosticsServer.audioStreamingEnabled = false
                              : audioStreamConsent.open()
                }
                Label {
                    id: audioStreamLabel
                    anchors.centerIn: parent
                    text: audioStreamChip.listeners > 0
                          ? "AUDIO OUT " + audioStreamChip.listeners
                          : !audioStreamChip.allowed ? "AUDIO OFF"
                          : audioStreamChip.ready ? "AUDIO ALLOWED"
                          : "AUDIO DOWN"
                    // Dimmed amber for allowed-but-unable, the same treatment
                    // the diagnostics chip gives an enabled service that never
                    // bound, so the two failures read the same way.
                    color: audioStreamChip.listeners > 0 ? "#43c6ac"
                           : !audioStreamChip.allowed ? "#718092"
                           : audioStreamChip.ready ? "#f3bd55" : "#9c7a3a"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }
            }
            Rectangle {
                implicitWidth: safeLabel.implicitWidth + 24
                implicitHeight: 30
                radius: 15
                color: "#202833"
                Label {
                    id: safeLabel
                    anchors.centerIn: parent
                    text: transmitController.onAir ? "TX ON AIR"
                          : transmitController.armed ? "TX ARMED"
                          : "TX DISARMED"
                    color: transmitController.onAir ? "#ff6b6b" : "#f3bd55"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }
            }
            ToolButton {
                text: "Profiles"
                onClicked: profileChooser.open()
                ToolTip.visible: hovered
                ToolTip.text: "Create or switch station profiles"
            }
            ToolButton {
                text: "Settings"
                onClicked: settingsDrawer.open()
                ToolTip.visible: hovered
                ToolTip.text: "Configure audio, decoder, radio, display, and station identity"
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 76
            Layout.fillHeight: true
            color: "#111720"
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 18
                spacing: 12
                Repeater {
                    model: ["RX", "CALLS", "QSO", "LOG", "REMOTE"]
                    delegate: Button {
                        required property string modelData
                        width: 60
                        height: 44
                        flat: true
                        text: modelData
                        font.pixelSize: 10
                        onClicked: {
                            if (modelData === "QSO")
                                txDrawer.open()
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: modelData === "RX"
                            ? "Current receiver workspace"
                            : modelData === "QSO"
                              ? "Open guarded transmit and QSO controls"
                              : modelData + " workspace is not available yet"
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 14
            spacing: 12

            RowLayout {
                id: receiverToolbar
                Layout.fillWidth: true
                z: 20
                Label { text: "Receiver workspace"; font.pixelSize: 18; font.weight: Font.DemiBold }
                ComboBox {
                    model: ["Live audio", "WAV replay", "Live SDR"]
                    currentIndex: replayController.sourceMode
                    onActivated: {
                        replayController.sourceMode = currentIndex
                        if (currentIndex === 0)
                            appSettings.receiverInputTypeIndex = 0
                        else if (currentIndex === 2)
                            appSettings.receiverInputTypeIndex = 1
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "Choose sound-card audio, a recorded WAV, or direct SDR IQ reception"
                }
                Rectangle { width: 1; height: 28; color: "#303a46" }
                Label { text: "Listen"; color: "#91a0b1"; font.pixelSize: 11 }
                // None of the three lamps is `checkable`, and that is the
                // whole point. A checkable button toggles its own `checked`
                // the instant it is pressed, before the handler runs, so the
                // lamp showed the click rather than the monitor mode. The
                // binding below only repaints it again when the controller
                // emits monitorChanged() -- and setMonitorMode() returns
                // without emitting anything when the mode it is handed is the
                // one already in force. So pressing REGION while region
                // listening was already on turned the lamp OFF and left the
                // audio ON, and pressing it again turned the lamp back on
                // without changing anything either: a button that visibly
                // toggled while nothing it controlled ever moved, which is
                // indistinguishable from a dead one. Not checkable, the lamp
                // is only ever the controller's own state, so what it shows
                // is what is actually playing.
                ToolButton {
                    objectName: "monitorOffButton"
                    text: "OFF"
                    checked: replayController.monitorMode === 0
                    onClicked: replayController.setMonitorMode(0)
                    ToolTip.visible: hovered
                    ToolTip.text: "Mute receiver monitoring"
                }
                ToolButton {
                    objectName: "monitorReceiverButton"
                    text: "RX"
                    checked: replayController.monitorMode === 1
                    enabled: replayController.activeSource
                             && replayController.sourceMode !== 2
                    onClicked: replayController.setMonitorMode(1)
                    ToolTip.visible: hovered
                    ToolTip.text: replayController.sourceMode === 2
                        ? "Raw wideband IQ is not loudspeaker audio; use a decoder-card speaker to monitor filtered streams"
                        : "Play the complete receiver passband without a stream filter"
                }
                ToolButton {
                    objectName: "monitorRegionButton"
                    text: "REGION"
                    checked: replayController.monitorMode === 3
                    visible: replayController.sourceMode === 2
                    enabled: replayController.activeSource
                             && replayController.sourceMode === 2
                    onClicked: replayController.setMonitorMode(3)
                    ToolTip.visible: hovered
                    ToolTip.text: "Listen to the whole decode region at once, not one stream: every signal inside the window is heard together, each at the pitch its own position in the window gives it, so a higher tone is a station higher in the region"
                }
                Slider {
                    objectName: "monitorLevelSlider"
                    from: 0
                    to: 1
                    value: replayController.monitorLevel
                    enabled: replayController.monitorMode !== 0
                    Layout.preferredWidth: 82
                    onMoved: replayController.setMonitorLevel(value)
                    ToolTip.visible: hovered
                    ToolTip.text: "Monitor level " + Math.round(value * 100) + "%"
                }
                // The controller has always written what the monitor is doing,
                // and every reason it could not do it, into monitorStatus --
                // "Region listening needs direct SDR reception", "Selected
                // monitor output does not support 48000 Hz mono float audio",
                // "Could not start the monitor output", "Monitor output write
                // failed" -- and nothing in this window ever read the
                // property. A refused request and an output device that cannot
                // carry the region's 48 kHz mono float therefore looked
                // exactly like a button that does nothing: the operator
                // pressed REGION, heard silence, and had no way to find out
                // which of the two had happened. Shown unconditionally rather
                // than only on failure, because the working states are worth
                // reading too -- mode 3 names the region width it is playing,
                // which is the one number the gesture that sets it cannot
                // otherwise confirm.
                Label {
                    objectName: "monitorStatusNotice"
                    visible: replayController.monitorStatus.length > 0
                    text: replayController.monitorStatus
                    color: "#8d9aaa"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    Layout.maximumWidth: 300
                    ToolTip.visible: monitorStatusHover.hovered
                    ToolTip.text: replayController.monitorStatus
                    HoverHandler { id: monitorStatusHover }
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: replayController.sourceMode === 0
                          ? appSettings.audioInputDisplayName
                          : (replayController.sourceMode === 2
                             ? appSettings.sdrDeviceDisplayName
                             : (replayController.sourceLoaded
                             ? replayController.sourceName + "  •  " + replayController.sampleRate.toFixed(0) + " Hz"
                             : "No replay source"))
                    color: "#8d9aaa"
                }
                Button {
                    objectName: "startLiveAudioButton"
                    z: 21
                    text: "Start live RX"
                    visible: replayController.sourceMode === 0
                    enabled: !replayController.liveCapturing
                    onClicked: replayController.startLiveAudio()
                    ToolTip.visible: hovered
                    ToolTip.text: enabled
                        ? "Start spectrum analysis and CW decoding from the selected audio input"
                        : "Live receiver processing is already running"
                }
                Button {
                    text: "Stop live RX"
                    visible: replayController.sourceMode === 0
                    enabled: replayController.liveCapturing
                    onClicked: replayController.stopLiveAudio()
                    ToolTip.visible: hovered
                    ToolTip.text: enabled
                        ? "Stop live audio processing"
                        : "Live receiver processing is not running"
                }
                Button {
                    objectName: "startLiveSdrButton"
                    z: 21
                    text: "Start SDR RX"
                    visible: replayController.sourceMode === 2
                    enabled: !replayController.liveCapturing
                             && appSettings.sdrBackendAvailable
                             && appSettings.sdrDeviceIndex >= 0
                    onClicked: replayController.startLiveSdr()
                    ToolTip.visible: hovered
                    ToolTip.text: enabled
                        ? "Start direct IQ reception, wide spectrum analysis, and CW decoding"
                        : appSettings.sdrDiagnostic
                }
                Button {
                    text: "Stop SDR RX"
                    visible: replayController.sourceMode === 2
                    enabled: replayController.liveCapturing
                    onClicked: replayController.stopLiveAudio()
                    ToolTip.visible: hovered
                    ToolTip.text: "Stop direct SDR reception"
                }
                Button {
                    text: "Open WAV"
                    visible: replayController.sourceMode === 1
                    onClicked: wavDialog.open()
                    ToolTip.visible: hovered
                    ToolTip.text: "Choose a PCM or 32-bit float receiver recording"
                }
                Button {
                    text: replayController.playing ? "Pause" : "Play"
                    visible: replayController.sourceMode === 1
                    enabled: replayController.sourceLoaded
                    onClicked: replayController.playing ? replayController.pause() : replayController.play()
                    ToolTip.visible: hovered
                    ToolTip.text: replayController.playing
                        ? "Pause replay at the current position"
                        : "Continue decoding the selected recording"
                }
                Button {
                    text: "Stop"
                    visible: replayController.sourceMode === 1
                    enabled: replayController.sourceLoaded
                    onClicked: replayController.stop()
                    ToolTip.visible: hovered
                    ToolTip.text: "Stop replay and return to its beginning"
                }
            }

            Rectangle {
                id: spectrumPanel
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 10
                color: "#09111a"
                border.color: "#263241"
                clip: true
                z: 0

                SpectrumWaterfall {
                    id: spectrumDisplay
                    objectName: "spectrumDisplay"
                    anchors.fill: parent
                    anchors.margins: 10
                    // Rendering off is a resource decision, so the feed is cut
                    // rather than the item hidden. Every waterfall row is
                    // composed inside the item's frame handler, which is
                    // connected only while a source is set: with no source
                    // nothing is conditioned, no row is appended or retained,
                    // no repaint is scheduled, and the history already held is
                    // released. Hiding alone would leave all of that running
                    // behind an invisible item. A station left up as a
                    // diagnostics server does not need to draw a waterfall.
                    source: appSettings.waterfallRenderingEnabled
                            ? replayController : null
                    visible: appSettings.waterfallRenderingEnabled
                    targetFps: appSettings.targetFps
                    waterfallRate: appSettings.waterfallRate
                    waterfallTimeSpanSeconds: appSettings.waterfallTimeSpanSeconds
                    displayMode: appSettings.spectrumDisplayMode
                    automaticRange: appSettings.automaticRange
                    automaticRangeSpanDb: appSettings.automaticRangeSpanDb
                    lowerBoundDb: appSettings.lowerBoundDb
                    upperBoundDb: appSettings.upperBoundDb
                    noiseSuppression: appSettings.waterfallNoiseSuppression
                    noiseMarginDb: appSettings.waterfallNoiseMarginDb
                    showGrid: appSettings.showGrid
                    // Direct IQ only. An audio card's axis is already the
                    // width of the passband the operator hears, so there is
                    // nothing to narrow; a wide SDR capture is the only case
                    // where the delivered span and the requested one differ.
                    preferredSpanHz: replayController.sourceMode === 2
                                     ? appSettings.sdrSampleRateHz : 0
                }

                Label {
                    objectName: "waterfallRenderingOffNotice"
                    visible: !appSettings.waterfallRenderingEnabled
                    anchors.centerIn: parent
                    width: parent.width - 80
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: "#718092"
                    font.pixelSize: 13
                    z: 12
                    text: "Spectrum and waterfall rendering is off.\n"
                          + "Reception and decoding continue. Settings > Display > Waterfall rendering."
                }
                Rectangle {
                    id: sdrDecoderWindowOverlay
                    objectName: "sdrDecoderWindowOverlay"
                    visible: replayController.sourceMode === 2
                             && spectrumDisplay.upperFrequencyHz
                                > spectrumDisplay.lowerFrequencyHz
                             && upperHz >= spectrumDisplay.lowerFrequencyHz
                             && lowerHz <= spectrumDisplay.upperFrequencyHz
                    property real lowerHz:
                        appSettings.sdrDecoderCenterFrequencyHz
                        - appSettings.sdrDecoderBandwidthHz / 2
                    property real upperHz:
                        appSettings.sdrDecoderCenterFrequencyHz
                        + appSettings.sdrDecoderBandwidthHz / 2
                    x: Math.max(spectrumDisplay.x,
                                Math.min(spectrumDisplay.x
                                         + spectrumDisplay.width,
                                         window.hzToX(lowerHz)))
                    y: spectrumDisplay.y
                    width: Math.max(1, Math.min(spectrumDisplay.x
                                                + spectrumDisplay.width,
                                                window.hzToX(upperHz)) - x)
                    height: spectrumDisplay.height
                    // Transparency belongs in the fill and nowhere else. This
                    // carried an 8.6% alpha in the colour AND opacity 0.34 on
                    // the item, and the two multiplied to under 3% -- so the
                    // band that says where decoding is happening was the
                    // hardest thing on the spectrum to see. Worse, an item
                    // opacity dims its children, so the border and the label
                    // faded with it: the two parts that carry the meaning were
                    // attenuated along with the tint that only has to hint.
                    // The fill stays translucent enough to read the spectrum
                    // through it; the edge and the label are drawn at full
                    // strength, because they are what an operator looks for.
                    color: "#3325c9b0"
                    border.color: "#5fe4c8"
                    border.width: 2
                    z: 4
                    Label {
                        anchors.top: parent.top
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.topMargin: 8
                        text: "CW decode  "
                              + (appSettings.sdrDecoderBandwidthHz / 1000)
                              + " kHz"
                        color: "#7fffe7"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                }

                // Zoom and the decode window are deliberately independent
                // controls, so an operator can zoom somewhere the decoder is
                // not. sdrDecoderWindowOverlay disappears completely in that
                // state, which silently reads as "nothing is being decoded".
                // This edge tab is the persistent proof that decoding
                // continues off-screen, and it points the way back.
                Rectangle {
                    id: sdrDecoderWindowEdgeIndicator
                    objectName: "sdrDecoderWindowEdgeIndicator"
                    readonly property bool below:
                        sdrDecoderWindowOverlay.upperHz
                        < spectrumDisplay.lowerFrequencyHz
                    visible: replayController.sourceMode === 2
                             && spectrumDisplay.upperFrequencyHz
                                > spectrumDisplay.lowerFrequencyHz
                             && (below
                                 || sdrDecoderWindowOverlay.lowerHz
                                    > spectrumDisplay.upperFrequencyHz)
                    width: 26
                    height: 54
                    x: below ? spectrumDisplay.x + 4
                             : spectrumDisplay.x + spectrumDisplay.width
                               - width - 4
                    y: spectrumDisplay.y
                       + (spectrumDisplay.height - height) / 2
                    radius: 4
                    color: edgeIndicatorMouse.containsMouse
                           ? "#d4123028" : "#c00e2220"
                    border.color: "#43c6ac"
                    border.width: 1
                    // Above manualSliceHitArea (z 6), which otherwise covers
                    // the whole spectrum and would swallow the hover.
                    z: 7
                    Column {
                        anchors.centerIn: parent
                        spacing: 0
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: sdrDecoderWindowEdgeIndicator.below ? "‹" : "›"
                            color: "#7fffe7"
                            font.pixelSize: 22
                            font.weight: Font.Bold
                        }
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "CW"
                            color: "#7fffe7"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }
                    MouseArea {
                        id: edgeIndicatorMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        // Pan rather than zoom: the operator chose this span
                        // width, so only the centre is moved. panBy clamps to
                        // the acquired span and is inert when not zoomed.
                        onClicked: spectrumDisplay.panBy(
                                       appSettings.sdrDecoderCenterFrequencyHz
                                       - (spectrumDisplay.lowerFrequencyHz
                                          + spectrumDisplay.upperFrequencyHz) / 2)
                    }
                    ToolTip.visible: edgeIndicatorMouse.containsMouse
                    ToolTip.text: "CW decoding continues "
                                  + (sdrDecoderWindowEdgeIndicator.below
                                     ? "below " : "above ")
                                  + "the visible span, centred on "
                                  + window.formatRigFrequency(
                                      appSettings.sdrDecoderCenterFrequencyHz)
                                  + " Hz. Click to bring it back into view."
                }

                // Every axis label on this panel is drawn straight over live
                // waterfall speckle, so each one carries its own semi-opaque
                // plate. Without it the glyphs disappear into whatever colour
                // the waterfall happens to paint underneath.
                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 10
                    leftPadding: 5
                    rightPadding: 5
                    topPadding: 1
                    bottomPadding: 1
                    text: spectrumDisplay.effectiveUpperBoundDb.toFixed(0) + " dBFS"
                    visible: spectrumDisplay.visible
                    color: "#c6d4e2"
                    font.pixelSize: 12
                    z: 4
                    background: Rectangle { color: "#c8080f16"; radius: 3 }
                }
                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.topMargin: parent.height * 0.34
                    anchors.leftMargin: 10
                    leftPadding: 5
                    rightPadding: 5
                    topPadding: 1
                    bottomPadding: 1
                    text: spectrumDisplay.effectiveLowerBoundDb.toFixed(0) + " dBFS"
                    visible: spectrumDisplay.visible
                    color: "#c6d4e2"
                    font.pixelSize: 12
                    z: 4
                    background: Rectangle { color: "#c8080f16"; radius: 3 }
                }
                Repeater {
                    model: 7
                    delegate: Item {
                        required property int index
                        property real fraction: index / 6.0
                        property real tickX: spectrumDisplay.x
                                             + fraction * spectrumDisplay.width
                        visible: spectrumDisplay.upperFrequencyHz
                                 > spectrumDisplay.lowerFrequencyHz
                        x: tickX
                        // 28 px, not 22: the larger plated label needs the
                        // extra room to stay inside the waterfall.
                        y: spectrumDisplay.y + spectrumDisplay.height - 28
                        z: 4
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 1
                            height: 6
                            color: "#a3b4c6"
                        }
                        Label {
                            x: index === 0 ? 2
                               : (index === 6 ? -implicitWidth - 2
                                  : -implicitWidth / 2)
                            y: 5
                            leftPadding: 5
                            rightPadding: 5
                            topPadding: 1
                            bottomPadding: 1
                            // The axis carries whatever the analyser produced,
                            // which for an audio card is the passband. An
                            // operator reads a band in RF, and the decoder
                            // cards already said RF, so labelling the ruler in
                            // audio named the same signal two ways on one
                            // screen. The VFO formatter is used for RF because
                            // the coarse one rounds to a kilohertz, which is
                            // wider than a CW signal.
                            text: {
                                var axisHz =
                                    spectrumDisplay.lowerFrequencyHz
                                    + fraction
                                      * (spectrumDisplay.upperFrequencyHz
                                         - spectrumDisplay.lowerFrequencyHz)
                                var shownHz =
                                    replayController.axisFrequencyHz(axisHz)
                                return replayController.axisShowsRf
                                    ? window.formatVfoFrequency(shownHz)
                                    : window.formatFrequency(shownHz)
                            }
                            color: "#dce7f2"
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            background: Rectangle { color: "#c8080f16"; radius: 3 }
                        }
                    }
                }

                Item {
                    id: txSliceGuideOverlay
                    objectName: "txSliceGuideOverlay"
                    // The guide follows only authoritative TX-VFO readback.
                    // SDR frames already use absolute RF; sound-card frames
                    // use the inverse of the sideband-aware RF/AF mapping.
                    property real guideCenterHz: {
                        var source = replayController.sourceMode
                        var rx = replayController.radioRxFrequencyHz
                        var tx = appSettings.radioTxVfoFrequencyHz
                        if (tx <= 0 || source === 1
                                || (source === 0 && rx <= 0))
                            return NaN
                        return replayController.rfFrequencyToDisplayHz(tx)
                    }
                    property real guideLowerHz: guideCenterHz
                                                - 0.5 * appSettings.cwGuideWidthHz
                    property real guideUpperHz: guideCenterHz
                                                + 0.5 * appSettings.cwGuideWidthHz
                    readonly property color guideColor: "#ff7b84"
                    visible: appSettings.showCwGuide
                             && replayController.activeSource
                             && Number.isFinite(guideCenterHz)
                             && spectrumDisplay.upperFrequencyHz
                                > spectrumDisplay.lowerFrequencyHz
                             && guideCenterHz >= spectrumDisplay.lowerFrequencyHz
                             && guideCenterHz <= spectrumDisplay.upperFrequencyHz
                    anchors.fill: spectrumDisplay
                    z: 3

                    Repeater {
                        model: [txSliceGuideOverlay.guideLowerHz,
                                txSliceGuideOverlay.guideUpperHz]
                        delegate: Item {
                            required property var modelData
                            property real boundaryHz: Number(modelData)
                            visible: boundaryHz >= spectrumDisplay.lowerFrequencyHz
                                     && boundaryHz <= spectrumDisplay.upperFrequencyHz
                            x: window.hzToX(boundaryHz) - spectrumDisplay.x - 1
                            width: 2
                            height: txSliceGuideOverlay.height

                            Repeater {
                                model: Math.ceil(parent.height / 12)
                                delegate: Rectangle {
                                    required property int index
                                    y: index * 12
                                    width: 2
                                    height: 6
                                    color: txSliceGuideOverlay.guideColor
                                }
                            }
                        }
                    }
                }
                MouseArea {
                    id: manualSliceHitArea
                    objectName: "manualSliceHitArea"
                    x: spectrumDisplay.x
                    y: spectrumDisplay.y
                    width: spectrumDisplay.width
                    height: spectrumDisplay.height
                    z: 6
                    // With the display detached there is no frequency axis,
                    // so a gesture on the panel would point at zero hertz.
                    // The click path already refuses that; switching the hit
                    // area off with the rendering spares the drag path too.
                    enabled: replayController.activeSource
                             && appSettings.waterfallRenderingEnabled
                    hoverEnabled: true
                    // Middle is here because middle-drag pans. It was
                    // handled in onPressed but never accepted, so the handler
                    // could not run and panning did nothing at all.
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                     | Qt.MiddleButton
                    property real panLastX: 0
                    // Whether the pointer actually travelled while the wheel
                    // button was down, which is what separates a pan from a
                    // click on it.
                    property bool panMoved: false
                    property bool decoderSelectionActive: false
                    property bool suppressSelectionClick: false
                    property real decoderSelectionStartX: 0
                    property real decoderSelectionCurrentX: 0
                    // Whether a gesture was a click or a drag is a question
                    // about the pointer, so it is answered in pixels. It used
                    // to be answered in hertz -- 250 to resize at all, and a
                    // second, different 1000 to centre the region on the
                    // middle of the box rather than on the point of release --
                    // and hertz per pixel is precisely what the spectrum zoom
                    // changes. At full span on a two-megahertz capture 250 Hz
                    // is a sixth of a pixel, so a right-click that slipped at
                    // all resized the region; zoomed in far enough to see
                    // individual CW signals, a deliberate forty-pixel drag
                    // measured under 1000 Hz, was read as a click, and the
                    // region jumped to where the button came up at the floor
                    // width while the box the operator had just drawn was
                    // discarded. One gesture, two different meanings depending
                    // on the zoom, and neither of them the drawn rectangle.
                    readonly property real decoderSelectionThresholdPx: 6
                    // What the drag currently in progress will apply, so the
                    // rectangle on screen and the region that results are the
                    // same calculation rather than two that agree by hand.
                    // Reading it while the pointer moves is also what makes
                    // the two clamps visible: a drag wider than 24 kHz or
                    // narrower than 2 kHz used to be silently corrected on
                    // release, which is indistinguishable from the gesture
                    // having mis-measured.
                    function selectionBandwidthHz() {
                        if (Math.abs(decoderSelectionCurrentX
                                     - decoderSelectionStartX)
                                < decoderSelectionThresholdPx) {
                            return appSettings.sdrDecoderBandwidthHz
                        }
                        var draggedHz = Math.abs(
                                    frequencyAtX(decoderSelectionCurrentX)
                                    - frequencyAtX(decoderSelectionStartX))
                        // 2 kHz is the narrowest slice the decimator accepts.
                        // 24 kHz is the ceiling, and it is the same ceiling
                        // the setter clamps to: the region is not only
                        // decoded, it is what the operator listens to, and
                        // 48 kHz audio carries 24 kHz of bandwidth and no
                        // more. So the decode region is the listen window --
                        // one number, and nothing that can be decoded but
                        // never heard.
                        return Math.round(Math.max(2000, Math.min(
                                   24000, draggedHz)) / 100) * 100
                    }
                    function frequencyAtX(positionX) {
                        var fraction = Math.max(0, Math.min(1,
                                                           positionX / width))
                        return spectrumDisplay.lowerFrequencyHz
                                + fraction
                                  * (spectrumDisplay.upperFrequencyHz
                                     - spectrumDisplay.lowerFrequencyHz)
                    }
                    // Lock keys are not intentions. Num Lock sets
                    // Qt.KeypadModifier on Windows and a group switch sets its
                    // own bit, so a strict equality against Qt.ControlModifier
                    // failed for an operator holding exactly Control with Num
                    // Lock on -- and a gesture that depends on which lamps are
                    // lit on the keyboard is a gesture nobody can rely on.
                    // Only the keys a person deliberately holds are compared.
                    readonly property int intentionalModifiers:
                        Qt.ShiftModifier | Qt.ControlModifier
                        | Qt.AltModifier | Qt.MetaModifier
                    function hasExactModifiers(mouse, required) {
                        return (mouse.modifiers & intentionalModifiers)
                                === required
                    }
                    function streamIdAtX(positionX) {
                        if (width <= 0 || spectrumDisplay.upperFrequencyHz
                                <= spectrumDisplay.lowerFrequencyHz) {
                            return 0
                        }
                        var pointedHz = frequencyAtX(positionX)
                        var pixelToleranceHz = 14
                                * (spectrumDisplay.upperFrequencyHz
                                   - spectrumDisplay.lowerFrequencyHz) / width
                        var toleranceHz = Math.max(60, pixelToleranceHz)
                        var nearestDistance = toleranceHz
                        var nearestId = 0
                        for (var index = 0;
                             index < replayController.decoderChannels.length;
                             ++index) {
                            var channel = replayController.decoderChannels[index]
                            if (!channel.verifiedCw
                                    && !channel.operatorSelected) {
                                continue
                            }
                            var distance = Math.abs(
                                        channel.presentationFrequencyHz
                                        - pointedHz)
                            if (distance <= nearestDistance) {
                                nearestDistance = distance
                                nearestId = channel.id
                            }
                        }
                        return nearestId
                    }
                    property var hoveredStreamId: containsMouse
                                                  ? streamIdAtX(mouseX) : 0
                    cursorShape: hoveredStreamId !== 0
                                 ? Qt.PointingHandCursor : Qt.CrossCursor
                    onPressed: function(mouse) {
                        if (mouse.button === Qt.MiddleButton) {
                            panLastX = mouse.x
                            panMoved = false
                        }
                        // Ctrl+Right, with the rest of the RX-decoder
                        // family: plain right points the window, Ctrl+Right
                        // sizes it. A click without a drag keeps the width,
                        // which is the same distinction the sizing already
                        // makes.
                        // Reported unconditionally, into the session log an
                        // operator opts into with --log-file. Three rounds of
                        // inference about why this gesture does not size the
                        // region were each wrong, because every explanation
                        // was consistent with what could be seen from outside.
                        // These are the four values the decision is actually
                        // made from, so the next report answers it instead of
                        // narrowing it.
                        if (mouse.button === Qt.RightButton) {
                            console.log("region-gesture press"
                                        + " modifiers=" + mouse.modifiers
                                        + " intentional="
                                        + (mouse.modifiers
                                           & intentionalModifiers)
                                        + " wantCtrl=" + Qt.ControlModifier
                                        + " sourceMode="
                                        + replayController.sourceMode
                                        + " enabled=" + enabled)
                        }
                        if (mouse.button === Qt.RightButton
                                && hasExactModifiers(mouse,
                                                     Qt.ControlModifier)
                                && replayController.sourceMode === 2) {
                            decoderSelectionStartX = mouse.x
                            decoderSelectionCurrentX = mouse.x
                            decoderSelectionActive = true
                            // Claimed here rather than on release. Every exit
                            // from the release handler has to leave this set,
                            // including the early ones, because the click that
                            // follows is the plain right-click that re-points
                            // the region at its existing width -- which is
                            // precisely the wrong outcome for a gesture whose
                            // whole purpose was to choose a new width, and is
                            // indistinguishable from the drag having been
                            // ignored.
                            suppressSelectionClick = true
                            mouse.accepted = true
                        }
                    }
                    onPositionChanged: function(mouse) {
                        if (decoderSelectionActive
                                && (mouse.buttons & Qt.RightButton)) {
                            decoderSelectionCurrentX = Math.max(
                                0, Math.min(width, mouse.x))
                            return
                        }
                        if ((mouse.buttons & Qt.MiddleButton) === 0
                                || width <= 0) return
                        var deltaPixels = mouse.x - panLastX
                        // A few pixels of slip while pressing the wheel is not
                        // a drag. Without this a middle click would almost
                        // always be read as a one-pixel pan and never centre.
                        if (Math.abs(deltaPixels) > 2) panMoved = true
                        panLastX = mouse.x
                        spectrumDisplay.panBy(
                            -deltaPixels / width
                            * (spectrumDisplay.upperFrequencyHz
                               - spectrumDisplay.lowerFrequencyHz))
                    }
                    onReleased: function(mouse) {
                        // The wheel button, clicked rather than dragged,
                        // retunes the receiver so the clicked frequency
                        // becomes the centre of the acquired spectrum. Direct
                        // IQ only: an audio card has no centre to move, its
                        // passband is whatever the receiver feeding it is
                        // tuned to.
                        if (mouse.button === Qt.MiddleButton) {
                            if (panMoved || replayController.sourceMode !== 2
                                    || !replayController.activeSource
                                    || width <= 0) {
                                return
                            }
                            var centreHz = Math.round(frequencyAtX(mouse.x))
                            if (centreHz > 0)
                                appSettings.sdrCenterFrequencyHz = centreHz
                            return
                        }
                        if (!decoderSelectionActive
                                || mouse.button !== Qt.RightButton)
                            return
                        // The gesture ends where the pointer was last TRACKED,
                        // not where the release event says it was.
                        //
                        // This used to begin by overwriting the tracked
                        // position with the release coordinate, and that was
                        // the only point in the gesture where a good value was
                        // discarded. A station reported a region that moved to
                        // where the button came up but kept the width it
                        // already had, and reverted on release rather than
                        // after any delay. One value produces both halves at
                        // once: `dragged` below coming out false falls the
                        // width back to the setting already in force and makes
                        // the centre the release point -- which still reads as
                        // "it moved to where I dropped it" while the box that
                        // was drawn is thrown away. For that to happen after a
                        // forty-pixel drag, the release coordinate has to be
                        // back at the press position.
                        //
                        // Nothing is lost by not consulting it. The tracked
                        // value is what onPositionChanged put there, which is
                        // the rectangle the operator watched and the figure
                        // the kHz readout quoted; a position the release could
                        // report that the tracking never saw is a position no
                        // rectangle was ever drawn at, so applying it would
                        // apply a selection nobody made. A click with no
                        // motion leaves it at the press coordinate, within the
                        // click threshold of the release either way.
                        var firstHz = frequencyAtX(decoderSelectionStartX)
                        var lastHz = frequencyAtX(decoderSelectionCurrentX)
                        var dragged = Math.abs(decoderSelectionCurrentX
                                               - decoderSelectionStartX)
                                      >= decoderSelectionThresholdPx
                        // The width is whatever was dragged, to the nearest
                        // hundred hertz, clamped by the same function the
                        // rectangle on screen was reading while the pointer
                        // moved. A gesture below the click threshold still
                        // means "put the window here" and leaves the width
                        // alone.
                        var bandwidthHz = dragged
                                ? selectionBandwidthHz()
                                : appSettings.sdrDecoderBandwidthHz
                        // One rule for the centre: the middle of what was
                        // drawn. A second threshold used to decide this
                        // separately, so a drag between the two sizes the
                        // region to the floor width and then centred it on the
                        // release point instead of on the box -- the region
                        // landed beside the signal the operator had just
                        // bracketed. A drag narrower than the floor still gets
                        // the floor, but it stays centred where it was drawn.
                        var selectedCenterHz = Math.round(
                            dragged ? (firstHz + lastHz) / 2 : lastHz)
                        // The window, and only the window. This also opened a
                        // manual decode at the centre of whatever was dragged,
                        // so choosing where to listen silently created a
                        // stream the operator had not asked for -- at a
                        // frequency that is merely the middle of a selection,
                        // which is rarely where a signal is. Deciding what to
                        // decode is CTRL+RIGHT on the signal itself.
                        console.log("region-gesture release"
                                    + " startX=" + decoderSelectionStartX
                                    + " endX=" + decoderSelectionCurrentX
                                    + " dragged=" + dragged
                                    + " bandwidthHz=" + bandwidthHz
                                    + " centreHz=" + selectedCenterHz)
                        appSettings.setSdrDecoderWindow(selectedCenterHz,
                                                        bandwidthHz)
                        decoderSelectionActive = false
                        suppressSelectionClick = true
                        mouse.accepted = true
                    }
                    onCanceled: decoderSelectionActive = false
                    onContainsMouseChanged: {
                        if (containsMouse)
                            spectrumPointerHelp.offer()
                    }
                    onWheel: function(wheel) {
                        if (!replayController.activeSource) return
                        spectrumDisplay.zoomAt(
                            frequencyAtX(wheel.x),
                            wheel.angleDelta.y > 0 ? 0.72 : 1.38)
                        wheel.accepted = true
                    }
                    onClicked: function(mouse) {
                        if (suppressSelectionClick) {
                            suppressSelectionClick = false
                            return
                        }
                        if (!replayController.activeSource || width <= 0
                                || spectrumDisplay.upperFrequencyHz
                                   <= spectrumDisplay.lowerFrequencyHz) {
                            return
                        }
                        if (mouse.button === Qt.LeftButton
                                && hasExactModifiers(mouse,
                                                     Qt.ControlModifier)) {
                            // Guarded, because enabling split on a radio that
                            // does not offer it is a command that should not
                            // be sent speculatively. Spoken rather than
                            // silent, because a gesture that does nothing at
                            // all reads as the feature being broken.
                            if (!appSettings.radioPointedTxFrequencyAvailable) {
                                appSettings.reportPointedTxUnavailable()
                                return
                            }
                            var txRfHz = replayController.displayFrequencyToRfHz(
                                frequencyAtX(mouse.x))
                            if (txRfHz > 0)
                                appSettings.setControlledTxFrequencyHz(txRfHz)
                            return
                        }
                        // Alt+Left opens a decode where the operator points,
                        // for a signal the detector has not picked up. Left
                        // alone opens one it has.
                        if (mouse.button === Qt.LeftButton
                                && hasExactModifiers(mouse, Qt.AltModifier)) {
                            replayController.openManualDecoderSession(
                                frequencyAtX(mouse.x))
                            return
                        }
                        if (mouse.button === Qt.LeftButton
                                && hasExactModifiers(mouse,
                                                     Qt.NoModifier)) {
                            var streamId = streamIdAtX(mouse.x)
                            if (streamId !== 0)
                                replayController.openDecoderSession(streamId)
                            return
                        }
                        // Right-click moved the decode window AND opened a
                        // manual session in one gesture, so an operator asking
                        // for one always got the other. They are separate
                        // intentions -- where to look, and what to decode --
                        // and now take separate gestures.
                        if (mouse.button !== Qt.RightButton) return
                        // Ctrl+Right belongs to the region drag, which press
                        // and release already handled. Acting again here would
                        // give one gesture two outcomes, which is the thing
                        // this mapping exists to stop.
                        //
                        // Whether a selection happened is remembered from the
                        // press, above, rather than re-read from the modifiers
                        // this click reports: a release can arrive with
                        // Control already let go, and then the test below sees
                        // a bare right-click and re-points the region at the
                        // width the operator had just replaced. What the
                        // gesture was is decided when it begins.
                        if (decoderSelectionActive) {
                            // The release handler did not run to completion --
                            // it returns early if the release reports a button
                            // or a state it does not recognise. Clear the
                            // selection here so the rubber band cannot be left
                            // drawn on screen with no gesture behind it, and
                            // still refuse to re-point the region, because a
                            // selection was begun whatever the release said.
                            decoderSelectionActive = false
                            return
                        }
                        if (hasExactModifiers(mouse, Qt.ControlModifier))
                            return
                        if (!hasExactModifiers(mouse, Qt.NoModifier)) return
                        // Plain right-click points the received spectrum. On
                        // an audio card there is no window to move, so the
                        // gesture has nothing to do rather than falling back
                        // to the manual session it used to also perform.
                        if (replayController.sourceMode === 2) {
                            console.log("region-gesture repoint"
                                        + " modifiers=" + mouse.modifiers
                                        + " -- the plain right-click moved the"
                                        + " region and left its width alone")
                            appSettings.sdrDecoderCenterFrequencyHz =
                                Math.round(frequencyAtX(mouse.x))
                        }
                    }
                }
                Rectangle {
                    objectName: "sdrDecoderDragSelection"
                    visible: manualSliceHitArea.decoderSelectionActive
                    x: spectrumDisplay.x + Math.min(
                           manualSliceHitArea.decoderSelectionStartX,
                           manualSliceHitArea.decoderSelectionCurrentX)
                    y: spectrumDisplay.y
                    width: Math.max(2, Math.abs(
                               manualSliceHitArea.decoderSelectionCurrentX
                               - manualSliceHitArea.decoderSelectionStartX))
                    height: spectrumDisplay.height
                    color: "#2639d7bd"
                    border.color: "#7fffe7"
                    border.width: 2
                    opacity: 0.52
                    z: 8
                }
                // What the drag in progress will actually apply, read off the
                // same function that applies it. A spectrum spanning the whole
                // capture puts the entire 2-24 kHz range of this setting
                // inside about fifteen pixels, so without a figure the
                // operator is aiming a gesture whose result they cannot
                // predict and only discovers the clamp afterwards.
                Rectangle {
                    objectName: "sdrDecoderDragReadout"
                    visible: manualSliceHitArea.decoderSelectionActive
                    x: Math.max(spectrumDisplay.x,
                                Math.min(spectrumDisplay.x
                                         + spectrumDisplay.width - width,
                                         spectrumDisplay.x
                                         + (manualSliceHitArea
                                            .decoderSelectionStartX
                                            + manualSliceHitArea
                                              .decoderSelectionCurrentX) / 2
                                         - width / 2))
                    y: spectrumDisplay.y + 10
                    width: dragReadoutLabel.implicitWidth + 14
                    height: dragReadoutLabel.implicitHeight + 8
                    radius: 3
                    color: "#d2071b18"
                    border.color: "#7fffe7"
                    border.width: 1
                    z: 9
                    Label {
                        id: dragReadoutLabel
                        anchors.centerIn: parent
                        text: (manualSliceHitArea.selectionBandwidthHz()
                               / 1000).toFixed(1) + " kHz decode region"
                        color: "#7fffe7"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                }
                ToolButton {
                    objectName: "resetSpectrumZoomButton"
                    visible: spectrumDisplay.zoomed
                    anchors.right: spectrumDisplay.right
                    anchors.bottom: spectrumDisplay.bottom
                    anchors.margins: 12
                    z: 10
                    text: "Full span"
                    onClicked: spectrumDisplay.resetZoom()
                    ToolTip.visible: hovered
                    ToolTip.text: "Reset spectrum and waterfall zoom"
                }
                Rectangle {
                    id: spectrumPointerHelp
                    objectName: "spectrumPointerHelp"
                    property bool hintActive: false
                    function offer() {
                        if (!appSettings.showSpectrumGestureHints
                                || pointerHintCooldown.running)
                            return
                        hintActive = true
                        pointerHintLifetime.restart()
                    }
                    visible: appSettings.showSpectrumGestureHints
                             && hintActive
                             && manualSliceHitArea.containsMouse
                    z: 9
                    anchors.left: spectrumDisplay.left
                    anchors.bottom: spectrumDisplay.bottom
                    anchors.leftMargin: 12
                    anchors.bottomMargin: 12
                    width: Math.min(pointerHelpText.implicitWidth + 22,
                                    spectrumDisplay.width - 24)
                    height: pointerHelpText.implicitHeight + 14
                    radius: 5
                    color: "#dd111720"
                    border.color: "#526172"
                    border.width: 1
                    Label {
                        id: pointerHelpText
                        anchors.centerIn: parent
                        // Kept in step with the handlers above. Right-click
                        // points the received spectrum; CTRL+RIGHT opens a
                        // manual decode. They were one gesture and an operator
                        // asking for either always got both.
                        text: manualSliceHitArea.hoveredStreamId !== 0
                              ? "LEFT: open   •   RIGHT: point RX   •   CTRL+RIGHT: decode region   •   ALT+LEFT: manual decode   •   CTRL+LEFT: TX   •   WHEEL BTN: centre"
                              : "RIGHT: point RX   •   CTRL+RIGHT: decode region   •   ALT+LEFT: manual decode   •   WHEEL BTN: centre   •   WHEEL: zoom"
                        color: "#d4dbe4"
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Timer {
                        id: pointerHintLifetime
                        interval: 10000
                        repeat: false
                        onTriggered: {
                            spectrumPointerHelp.hintActive = false
                            pointerHintCooldown.restart()
                        }
                    }
                    Timer {
                        id: pointerHintCooldown
                        interval: 300000
                        repeat: false
                    }
                }
                ToolButton {
                    id: tuneRxDownButton
                    objectName: "tuneRxDownButton"
                    visible: (replayController.sourceMode === 0
                              && replayController.radioFrequencyAvailable
                              && appSettings.radioFrequencyWritable)
                             || (replayController.sourceMode === 2
                                 && appSettings.sdrBackendAvailable
                                 && appSettings.sdrDeviceIndex >= 0)
                    anchors.left: spectrumDisplay.left
                    y: spectrumDisplay.y + spectrumDisplay.height * 0.68
                       - height / 2
                    width: 38
                    height: 58
                    z: 8
                    text: "<"
                    font.pixelSize: 24
                    Accessible.name: replayController.sourceMode === 2
                                     ? "Tune SDR RX down" : "Tune RX down"
                    Accessible.description: "Decrease the receive frequency by "
                                            + ((replayController.sourceMode === 2
                                                ? appSettings.sdrTuningStepHz
                                                : appSettings.radioTuningStepHz) / 1000)
                                            + " kilohertz"
                    onClicked: replayController.sourceMode === 2
                               ? appSettings.stepSdrRxFrequency(-1)
                               : appSettings.stepControlledRxFrequency(-1)
                    ToolTip.visible: hovered
                    ToolTip.delay: 250
                    ToolTip.text: (replayController.sourceMode === 2
                                   ? "Move the SDR acquisition window down "
                                   : "Tune RX down ")
                                  + ((replayController.sourceMode === 2
                                      ? appSettings.sdrTuningStepHz
                                      : appSettings.radioTuningStepHz) / 1000)
                                  + " kHz. TX and mode stay unchanged."
                }
                ToolButton {
                    id: tuneRxUpButton
                    objectName: "tuneRxUpButton"
                    visible: (replayController.sourceMode === 0
                              && replayController.radioFrequencyAvailable
                              && appSettings.radioFrequencyWritable)
                             || (replayController.sourceMode === 2
                                 && appSettings.sdrBackendAvailable
                                 && appSettings.sdrDeviceIndex >= 0)
                    anchors.right: spectrumDisplay.right
                    y: spectrumDisplay.y + spectrumDisplay.height * 0.68
                       - height / 2
                    width: 38
                    height: 58
                    z: 8
                    text: ">"
                    font.pixelSize: 24
                    Accessible.name: replayController.sourceMode === 2
                                     ? "Tune SDR RX up" : "Tune RX up"
                    Accessible.description: "Increase the receive frequency by "
                                            + ((replayController.sourceMode === 2
                                                ? appSettings.sdrTuningStepHz
                                                : appSettings.radioTuningStepHz) / 1000)
                                            + " kilohertz"
                    onClicked: replayController.sourceMode === 2
                               ? appSettings.stepSdrRxFrequency(1)
                               : appSettings.stepControlledRxFrequency(1)
                    ToolTip.visible: hovered
                    ToolTip.delay: 250
                    ToolTip.text: (replayController.sourceMode === 2
                                   ? "Move the SDR acquisition window up "
                                   : "Tune RX up ")
                                  + ((replayController.sourceMode === 2
                                      ? appSettings.sdrTuningStepHz
                                      : appSettings.radioTuningStepHz) / 1000)
                                  + " kHz. TX and mode stay unchanged."
                }
                Repeater {
                    model: replayController.decoderChannels
                    delegate: Item {
                        id: channelMarker
                        required property var modelData
                        required property int index
                        property real channelHz: modelData.presentationFrequencyHz
                        // Keep presentation geometry independent of the
                        // decoder's adaptive 60/120/240 Hz analysis filter.
                        // That filter may legitimately change while decoding,
                        // but making the clickable marker follow it causes a
                        // distracting size flicker and falsely suggests that
                        // the transmitted carrier itself is changing width.
                        readonly property real markerWidthHz: 120
                        property real areaWidthPx: Math.max(28,
                            window.hzToX(channelHz + markerWidthHz / 2)
                            - window.hzToX(channelHz - markerWidthHz / 2))
                        visible: (modelData.verifiedCw
                                  || modelData.operatorSelected)
                                 && channelHz >= spectrumDisplay.lowerFrequencyHz
                                 && channelHz <= spectrumDisplay.upperFrequencyHz
                                 && spectrumDisplay.upperFrequencyHz
                                    > spectrumDisplay.lowerFrequencyHz
                        x: window.hzToX(channelHz) - width / 2
                        y: spectrumDisplay.y
                        width: areaWidthPx
                        height: spectrumDisplay.height
                        z: 5
                        property bool pointerHovered:
                            manualSliceHitArea.hoveredStreamId === modelData.id
                        Rectangle {
                            anchors.fill: parent
                            color: modelData.color
                            opacity: modelData.verifiedCw
                                     ? (modelData.active ? 0.28 : 0.0)
                                     : 0.10
                            border.color: modelData.color
                            border.width: modelData.verifiedCw
                                          ? (modelData.active ? 1 : 0) : 1
                        }
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: modelData.keyDown ? 3 : 1
                            height: parent.height
                            color: modelData.color
                            visible: modelData.verifiedCw && modelData.active
                            opacity: 0.9
                        }
                        Rectangle {
                            // A retained but inactive stream leaves the plot
                            // clear and keeps only a short identity-colored
                            // mark on the frequency axis.
                            visible: modelData.verifiedCw && !modelData.active
                            y: parent.height * 0.36 - 1
                            width: parent.width
                            height: 3
                            color: modelData.color
                            opacity: 0.9
                        }
                        Label {
                            anchors.left: parent.horizontalCenter
                            anchors.leftMargin: 7
                            y: spectrumDisplay.height * 0.36 - height - 12
                            transformOrigin: Item.BottomLeft
                            rotation: -90
                            text: modelData.callingOwnStation
                                  ? "\u25CF CALLING YOU"
                                  : !modelData.verifiedCw
                                  ? window.formatStreamFrequency(modelData)
                                    + " • manual"
                                  : modelData.callsign.length > 0
                                  ? (modelData.callsign
                                     + (modelData.callsignInDatabase ? " \u2713" : ""))
                                  : modelData.callsignSuggestion.length > 0
                                  ? "≈ " + modelData.callsignSuggestion
                                  : window.formatStreamFrequency(modelData)
                            color: modelData.color
                            font.pixelSize: channelMarker.pointerHovered ? 22 : 18
                            font.weight: Font.Bold
                            leftPadding: 4
                            rightPadding: 4
                            topPadding: 2
                            bottomPadding: 2
                            z: 2
                            background: Rectangle {
                                id: channelLabelBackground
                                objectName: "channelLabelBackground"
                                // A callsign corroborated by the operator's
                                // offline list is drawn as a solid chip. One
                                // that was only decoded keeps the plain
                                // background, because an unlisted station is
                                // ordinary rather than suspect. A stream that
                                // is calling the operator overrides both: it
                                // is the one thing here the operator must not
                                // miss, and it has to be visible on the
                                // spectrum, before any card is opened.
                                color: modelData.callingOwnStation
                                       ? "#5a1420"
                                       : (modelData.callsignInDatabase
                                          ? "#16241a" : "#e6091018")
                                border.color: modelData.callingOwnStation
                                              ? "#ff6b6b" : modelData.color
                                border.width: modelData.callingOwnStation
                                              ? 2
                                              : (modelData.callsignInDatabase
                                                 || channelMarker.pointerHovered ? 1 : 0)
                                radius: 3
                                SequentialAnimation on opacity {
                                    running: modelData.callingOwnStation
                                    loops: Animation.Infinite
                                    alwaysRunToEnd: true
                                    NumberAnimation { from: 1.0; to: 0.35; duration: 420 }
                                    // The cycle ends fully opaque and is
                                    // allowed to finish, so the marker never
                                    // freezes half faded when the call stops.
                                    NumberAnimation { from: 0.35; to: 1.0; duration: 420 }
                                }
                            }
                            Behavior on font.pixelSize {
                                NumberAnimation { duration: 90 }
                            }
                        }
                        ToolTip.visible: channelMarker.pointerHovered
                        ToolTip.delay: 450
                        ToolTip.text: (modelData.callsign.length > 0
                                       ? modelData.callsign + "\n"
                                       : (modelData.callsignSuggestion.length > 0
                                          ? (modelData.callsignSuggestionSource
                                             === "offline-directory"
                                             ? "Offline suggestion: "
                                             : "Acoustic suggestion: ")
                                            + modelData.callsignSuggestion
                                            + " from "
                                            + modelData.callsignSuggestionRawSpan
                                            + "\n" : ""))
                                      + window.formatStreamFrequency(modelData)
                                        + "\n"
                                      + (!modelData.verifiedCw
                                         ? "Manual slice • awaiting ordinary CW verification\n"
                                         : "")
                                      + modelData.audioFrequencyHz.toFixed(1)
                                        + " Hz audio • "
                                      + modelData.filterWidthHz.toFixed(0)
                                        + " Hz filter • "
                                      + modelData.driftHzPerSecond.toFixed(1)
                                        + " Hz/s drift"
                    }
                }
                // Stations that other receivers report hearing. A spot is
                // somebody else's evidence, never this receiver's: it does not
                // become a decoded stream, it never fills in or alters a
                // decoded callsign, and it is drawn in one neutral instrument
                // colour on the spectrum/waterfall separator so an operator
                // cannot read it as a signal verified here. The decoded-stream
                // markers keep z 5 for the same reason: where an external
                // report and a local decode land on the same hertz, this
                // receiver's own evidence wins the pixels.
                Item {
                    id: dxSpotOverlay
                    objectName: "dxSpotOverlay"
                    anchors.fill: spectrumDisplay
                    // The cluster switch is the one master switch for the
                    // feature, and it is the same switch that decides whether
                    // the link is joined at all, so the overlay is shown for
                    // exactly as long as spots can arrive. The label switch
                    // hides only the callsigns: a marker still says a station
                    // was reported there, which is the part worth keeping when
                    // a crowded band makes the text unreadable.
                    visible: appSettings.dxClusterEnabled
                             && replayController.activeSource
                             && spectrumDisplay.upperFrequencyHz
                                > spectrumDisplay.lowerFrequencyHz
                    // A zoomed or panned view must not paint a spot off-plot.
                    // The span filter in layoutSpots() drops everything that
                    // is out of view, and clipping stops a marker sitting on
                    // the very first or last hertz from bleeding into the
                    // panel margin.
                    clip: true
                    z: 4

                    // The renderer puts the trace in the top 0.36 of the panel
                    // and starts the waterfall 8 px below it. The spot ticks
                    // live in the lower half of that gutter, clear of the
                    // retained-stream marks just above it, and the callsigns
                    // hang beneath them on the waterfall side.
                    readonly property real separatorY: Math.round(height * 0.36)
                    // Deliberately one flat colour from the axis/chrome family
                    // rather than anything in the 24 decoded-stream identity
                    // colours, the teal decode window or the pink TX slice.
                    readonly property color spotColor: "#9fb3c8"
                    // Source is carried by colour, not by a glyph beside the
                    // call. A square and a circle asked the operator to
                    // remember which shape meant which, cost width next to
                    // every callsign, and read as punctuation rather than as
                    // information. Both hues stay in the chrome family --
                    // outside the 24 decoded-stream identities, the teal
                    // decode window and the pink TX slice -- so a spot can
                    // never be mistaken for something this receiver copied.
                    //
                    // A reverse-beacon report is a receiver's measurement and
                    // a cluster spot is a person's claim; when both agree the
                    // plate carries the two colours together, which is more
                    // legible than two marks and takes no extra room.
                    readonly property color spotRbnColor: "#8fd0ff"
                    readonly property color spotClusterColor: "#d2b48c"
                    // A spot is fresh for two minutes and fully faded after
                    // half an hour, so age reads off the overlay itself
                    // without opening a tooltip.
                    readonly property real freshSeconds: 120
                    readonly property real staleSeconds: 1800
                    readonly property real fadedStrength: 0.38
                    // Density limits. Markers stay long after labels stop
                    // fitting, because the marker is the part that carries the
                    // frequency; CALL-008 owns real decluttering.
                    readonly property int maximumMarkers: 240
                    readonly property int maximumLabels: 26
                    readonly property real labelGapPx: 6
                    readonly property real tickWidthPx: 15

                    // Every vertical number in this band comes from one
                    // measurement of the callsign's own font, because the bar
                    // and the plate it is supposed to contain used to be
                    // counted by hand against each other and did not agree.
                    // The bar was a flat 26 px deep; the plate was built from
                    // the label's own line height plus 8, which is 26 or more
                    // on any platform whose 13 px DemiBold line box runs to
                    // 18 px or taller. So on a real station the callsign hung
                    // six to nine pixels out of the bottom of the band that
                    // exists to give it a home, and was drawn on the waterfall
                    // below it -- which is the state the bar was added to fix.
                    // Measured once and used for both, the two cannot disagree
                    // whatever the platform font turns out to be.
                    FontMetrics {
                        id: spotLabelMetrics
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.4
                    }
                    readonly property real spotLabelLineHeight:
                        Math.ceil(spotLabelMetrics.height)
                    // 3 px of air above the callsign and 3 below it, then the
                    // 2 px source stripe on the plate's own bottom edge.
                    readonly property real spotPlateHeight:
                        spotLabelLineHeight + 8
                    readonly property real spotBarTopY: separatorY + 2
                    // Bar-relative, because the tick and the plate are drawn
                    // inside the bar now rather than beside it.
                    readonly property real spotTickOffsetY: 2
                    readonly property real spotPlateOffsetY: 8
                    readonly property real spotBarHeight:
                        spotPlateOffsetY + spotPlateHeight + 2
                    readonly property real spotBarBottomY:
                        spotBarTopY + spotBarHeight

                    function ageStrength(ageSeconds) {
                        var age = Number(ageSeconds)
                        if (!Number.isFinite(age) || age <= freshSeconds)
                            return 1.0
                        if (age >= staleSeconds)
                            return fadedStrength
                        return 1.0 - (1.0 - fadedStrength)
                                     * (age - freshSeconds)
                                       / (staleSeconds - freshSeconds)
                    }

                    function formatSpotAge(ageSeconds) {
                        var age = Math.max(0, Number(ageSeconds) || 0)
                        if (age < 90)
                            return Math.round(age) + " s ago"
                        if (age < 5400)
                            return Math.round(age / 60) + " min ago"
                        return (age / 3600).toFixed(1) + " h ago"
                    }

                    function spotSourceText(spot) {
                        if (spot.reverseBeacon && spot.cluster)
                            return "reverse beacon and cluster agree"
                        if (spot.reverseBeacon)
                            return "reverse beacon"
                        if (spot.cluster)
                            return "cluster"
                        return "source not stated"
                    }

                    // The room this callsign will really occupy, asked of the
                    // font that will really draw it. It used to be reserved as
                    // 12 + 7.4 px per character plus 11 px apiece for a
                    // reverse-beacon and a cluster mark -- marks that have not
                    // sat beside the callsign since they became the stripe
                    // underneath it. Charging for them over-reserved by up to
                    // 22 px and lost labels that would have fitted, while
                    // 7.4 px per character under-measures DemiBold capitals,
                    // so two neighbours could both be told there was room and
                    // then print into each other.
                    //
                    // advanceWidth() is a call, not a property read: a shared
                    // TextMetrics whose text every plate had to assign in turn
                    // would make each plate's width binding invalidate every
                    // other one. The 10 px is the plate's own 5 px inset on
                    // each side, and the extra pixel covers the difference
                    // between an advance and the laid-out item's own implicit
                    // width, which is what the plate is finally sized by.
                    function measuredLabelWidth(spot) {
                        return 11 + Math.ceil(
                                   spotLabelMetrics.advanceWidth(
                                       spot.callsign))
                    }

                    // Strongest evidence first: two independent sources that
                    // agree, then the freshest report, then the most repeated
                    // one. The frequency tie-break keeps the result stable
                    // from frame to frame instead of letting equal spots swap
                    // their labels while the operator reads them.
                    function compareSpotEvidence(first, second) {
                        var firstBoth = first.reverseBeacon && first.cluster
                                        ? 1 : 0
                        var secondBoth = second.reverseBeacon && second.cluster
                                         ? 1 : 0
                        if (firstBoth !== secondBoth)
                            return secondBoth - firstBoth
                        if (first.ageSeconds !== second.ageSeconds)
                            return first.ageSeconds - second.ageSeconds
                        if (first.observations !== second.observations)
                            return second.observations - first.observations
                        return first.displayHz - second.displayHz
                    }

                    // Reserves the horizontal room one callsign needs, or
                    // leaves it unlabelled when a stronger neighbour already
                    // holds that space.
                    function claimLabelSpace(spot, taken, plotWidth) {
                        var labelWidth = measuredLabelWidth(spot)
                        var left = Math.max(0, Math.min(
                            plotWidth - labelWidth,
                            spot.pixelX - labelWidth / 2))
                        var right = left + labelWidth
                        for (var other = 0; other < taken.length; ++other) {
                            if (left < taken[other].right + labelGapPx
                                    && right + labelGapPx > taken[other].left)
                                return false
                        }
                        spot.showLabel = true
                        taken.push({ left: left, right: right })
                        return true
                    }

                    function layoutSpots(source, lowerHz, upperHz, plotWidth) {
                        var placed = []
                        if (!source || plotWidth <= 0 || upperHz <= lowerHz)
                            return placed
                        for (var index = 0; index < source.length; ++index) {
                            var spot = source[index]
                            if (!spot)
                                continue
                            var displayHz = Number(spot.displayFrequencyHz)
                            if (!Number.isFinite(displayHz)
                                    || displayHz < lowerHz
                                    || displayHz > upperHz)
                                continue
                            var callsign = String(spot.callsign || "")
                                           .trim().toUpperCase()
                            if (callsign.length === 0)
                                continue
                            var reportedHz = Number(spot.frequencyHz)
                            var ageSeconds = Number(spot.ageSeconds)
                            placed.push({
                                callsign: callsign,
                                reverseBeacon: spot.reverseBeacon === true,
                                cluster: spot.cluster === true,
                                observations: Math.max(0, Math.round(
                                    Number(spot.observations) || 0)),
                                ageSeconds: Number.isFinite(ageSeconds)
                                            ? Math.max(0, ageSeconds) : 0,
                                displayHz: displayHz,
                                // A spot is always reported against real RF,
                                // so it is presented the way an RF stream is.
                                frequencyText: Number.isFinite(reportedHz)
                                    ? window.formatStreamFrequency({
                                          displayFrequencyHz: reportedHz,
                                          frequencyKind: "RF"
                                      })
                                    : window.formatFrequency(displayHz),
                                pixelX: window.hzToX(displayHz)
                                        - spectrumDisplay.x,
                                showLabel: false
                            })
                        }
                        var ranked = placed.slice()
                        ranked.sort(compareSpotEvidence)
                        if (ranked.length > maximumMarkers)
                            ranked = ranked.slice(0, maximumMarkers)
                        // Thin the labels instead of stacking them: walk the
                        // strongest evidence first and keep only the callsigns
                        // that still have room of their own. Everything that
                        // loses its label keeps its marker, so a dense band
                        // stays honest about how many stations were reported.
                        var taken = []
                        for (var rank = 0;
                             rank < ranked.length
                             && taken.length < maximumLabels;
                             ++rank) {
                            claimLabelSpace(ranked[rank], taken, plotWidth)
                        }
                        // Left to right, so the paint order on screen matches
                        // the order on the axis.
                        ranked.sort(function(first, second) {
                            return first.pixelX - second.pixelX
                        })
                        return ranked
                    }

                    readonly property var placedSpots:
                        visible
                        ? layoutSpots(replayController.dxSpots,
                                      spectrumDisplay.lowerFrequencyHz,
                                      spectrumDisplay.upperFrequencyHz,
                                      width)
                        : []

                    // Hover is read from the existing spectrum hit area rather
                    // than a MouseArea of its own, exactly as the decoded
                    // stream markers do it. This overlay stays read-only: it
                    // never accepts a click and never covers the probe, pan,
                    // zoom or decoder-span gestures underneath it.
                    readonly property int hoveredIndex: {
                        if (!manualSliceHitArea.containsMouse
                                || manualSliceHitArea.hoveredStreamId !== 0)
                            return -1
                        // The band a spot can be pointed at is the bar, taken
                        // from the bar rather than counted out again: the
                        // hand-counted version stopped 18 px under the
                        // waterfall top, which is above the bottom of a plate
                        // on most platforms, so pointing at the lower part of
                        // a callsign produced no tooltip.
                        var pointerY = manualSliceHitArea.mouseY
                        if (pointerY < separatorY || pointerY > spotBarBottomY)
                            return -1
                        var pointerX = manualSliceHitArea.mouseX
                        var nearest = -1
                        var nearestDistance = tickWidthPx / 2 + 3
                        for (var index = 0;
                             index < placedSpots.length; ++index) {
                            var distance = Math.abs(
                                placedSpots[index].pixelX - pointerX)
                            if (distance <= nearestDistance) {
                                nearestDistance = distance
                                nearest = index
                            }
                        }
                        return nearest
                    }

                    // The band the callsigns sit in.
                    //
                    // They used to hang loose over the top of the waterfall,
                    // which made them compete with the texture behind them and
                    // left nothing to tell an operator that the row belongs to
                    // reports rather than to this receiver. A quiet bar gives
                    // them a home, and it is drawn only when there is
                    // something to put in it.
                    // The ticks and the callsigns are CHILDREN of the bar, and
                    // that is the fix rather than a tidying of it. They used
                    // to be a sibling Repeater positioned from the overlay's
                    // own coordinates, so nothing but two sets of hand-counted
                    // offsets kept them in the band -- and when those offsets
                    // disagreed, as they did, there was no clip anywhere in
                    // the chain to stop a callsign being drawn outside the bar
                    // entirely. Inside it and clipped, a plate cannot leave
                    // the band whatever a platform's font metrics turn out to
                    // be; the bar is sized from those same metrics so that
                    // nothing has to be cut off for that to hold.
                    Rectangle {
                        objectName: "dxSpotBar"
                        visible: dxSpotOverlay.placedSpots.length > 0
                        x: 0
                        y: dxSpotOverlay.spotBarTopY
                        width: dxSpotOverlay.width
                        height: dxSpotOverlay.spotBarHeight
                        clip: true
                        color: "#b00b1420"
                        Rectangle {
                            anchors.top: parent.top
                            width: parent.width
                            height: 1
                            color: "#2b3947"
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: "#2b3947"
                        }

                        Repeater {
                            model: dxSpotOverlay.placedSpots
                            delegate: Item {
                                id: dxSpotMarker
                                required property var modelData
                                required property int index
                                readonly property bool pointerHovered:
                                    dxSpotOverlay.hoveredIndex === index
                                readonly property real ageOpacity:
                                    pointerHovered
                                    ? 1.0
                                    : dxSpotOverlay.ageStrength(
                                          modelData.ageSeconds)
                                // A plain geometric container filling the bar: the
                                // tick sits on the exact frequency while the
                                // callsign below is free to slide along the plot
                                // to stay readable.
                                width: dxSpotOverlay.width
                                height: dxSpotOverlay.spotBarHeight

                                Rectangle {
                                    objectName: "dxSpotTick"
                                    x: modelData.pixelX
                                       - dxSpotOverlay.tickWidthPx / 2
                                    y: dxSpotOverlay.spotTickOffsetY
                                    width: dxSpotOverlay.tickWidthPx
                                    height: 2
                                    color: dxSpotOverlay.spotColor
                                    opacity: dxSpotMarker.ageOpacity
                                    ToolTip.visible: dxSpotMarker.pointerHovered
                                    ToolTip.delay: 450
                                    ToolTip.text:
                                        modelData.callsign + "\n"
                                        + modelData.frequencyText + "\n"
                                        + dxSpotOverlay.spotSourceText(modelData)
                                        + "  •  " + modelData.observations
                                        + (modelData.observations === 1
                                           ? " report" : " reports")
                                        + "  •  "
                                        + dxSpotOverlay.formatSpotAge(
                                              modelData.ageSeconds)
                                        + "\nReported by another receiver. "
                                        + "Nothing here was decoded from this "
                                        + "signal."
                                }
                                Rectangle {
                                    id: dxSpotLabelPlate
                                    objectName: "dxSpotLabelPlate"
                                    // Like every other label on this panel, drawn
                                    // straight over live waterfall speckle, so it
                                    // carries its own semi-opaque plate.
                                    visible: modelData.showLabel
                                             && appSettings.dxSpotsShowLabels
                                    x: Math.max(0, Math.min(
                                           dxSpotOverlay.width - width,
                                           modelData.pixelX - width / 2))
                                    y: dxSpotOverlay.spotPlateOffsetY
                                    // The two evidence marks are placed by hand
                                    // rather than by a positioner so that both
                                    // stay centred on the callsign's own height
                                    // and the plate keeps a constant 5 px inset
                                    // whichever of them is present.
                                    // The source stripe under the call. Two
                                    // sources agreeing split it, so agreement is
                                    // visible without a second mark.
                                    readonly property real stripeHeight: 2
                                    // The width is still this label's own
                                    // laid-out size; the height is the
                                    // overlay's one measurement of the font,
                                    // which is the same number the bar was
                                    // sized from. That is what makes the
                                    // containment arithmetic rather than
                                    // coincidence: the plate cannot be deeper
                                    // than the band that holds it, because
                                    // both are the same expression.
                                    width: 10 + dxSpotCallLabel.implicitWidth
                                    height: dxSpotOverlay.spotPlateHeight
                                    radius: 3
                                    color: "#c8080f16"
                                    border.color: dxSpotMarker.pointerHovered
                                                  ? dxSpotOverlay.spotColor
                                                  : "transparent"
                                    border.width: 1
                                    opacity: dxSpotMarker.ageOpacity
                                    Label {
                                        id: dxSpotCallLabel
                                        x: 5
                                        y: 3
                                        // A fifth larger than before, on the
                                        // owner's reading of it at the previous
                                        // size.
                                        text: modelData.callsign
                                        color: dxSpotOverlay.spotColor
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        font.letterSpacing: 0.4
                                    }
                                    Row {
                                        objectName: "dxSpotSourceStripe"
                                        x: 5
                                        anchors.bottom: parent.bottom
                                        anchors.bottomMargin: 2
                                        width: dxSpotCallLabel.implicitWidth
                                        height: dxSpotLabelPlate.stripeHeight
                                        // Half each when the two sources agree,
                                        // the whole width when only one reported.
                                        readonly property int sources:
                                            (modelData.reverseBeacon ? 1 : 0)
                                            + (modelData.cluster ? 1 : 0)
                                        Rectangle {
                                            objectName: "dxSpotReverseBeaconStripe"
                                            visible: modelData.reverseBeacon
                                            width: parent.sources > 1
                                                   ? parent.width / 2 : parent.width
                                            height: parent.height
                                            color: dxSpotOverlay.spotRbnColor
                                        }
                                        Rectangle {
                                            objectName: "dxSpotClusterStripe"
                                            visible: modelData.cluster
                                            width: parent.sources > 1
                                                   ? parent.width / 2 : parent.width
                                            height: parent.height
                                            color: dxSpotOverlay.spotClusterColor
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // UI-008, second half. Once the main ruler at the foot of the
                // panel reads absolute RF, an operator loses any sense of how
                // wide the visible span is in audio terms -- and the audio
                // passband is exactly what the decoder hears and what the
                // receiver's filter sets. This is that second reference, and
                // it is deliberately subordinate to the main axis: the axis's
                // own untransformed hertz, never passed through
                // axisFrequencyHz(), in smaller and dimmer type.
                Item {
                    id: audioOffsetRuler
                    objectName: "audioOffsetRuler"
                    anchors.fill: spectrumDisplay
                    // Chrome, like the main axis: a decoded-stream marker at
                    // z 5 still wins the pixels where the two meet.
                    z: 4

                    readonly property real spanHz:
                        spectrumDisplay.upperFrequencyHz
                        - spectrumDisplay.lowerFrequencyHz
                    readonly property real midHz:
                        spectrumDisplay.lowerFrequencyHz + spanHz / 2
                    // Drawn only where the two scales genuinely differ.
                    // axisShowsRf on its own is not enough: direct IQ frames
                    // already arrive described in absolute RF, so
                    // axisFrequencyHz() is the identity for them and this
                    // ruler would reprint the RF numbers a second time under
                    // an "AF" heading. Probing the transform also covers the
                    // sound-card span whose RF mapping fails, where the main
                    // axis falls back to audio and a second audio scale would
                    // be pure duplication. The probe re-evaluates on exactly
                    // the same signals the main axis label does.
                    readonly property bool scalesDiffer: {
                        if (!replayController.axisShowsRf)
                            return false
                        var shownHz = replayController.axisFrequencyHz(midHz)
                        return Math.abs(shownHz - midHz) > 1.0
                    }
                    visible: replayController.activeSource
                             && spanHz > 0
                             && scalesDiffer
                             && bandFitsPanel

                    // Vertical band. The 8 px gutter itself is full: the
                    // retained-stream marks straddle the separator line, the
                    // spot ticks take its lower half, and the spot bar below
                    // them holds the callsign plates. The lower "dBFS" plate
                    // straddles the separator on the left edge, and the
                    // rotated stream callsigns end 12 px above it, so the
                    // strip above the separator is not free either. The first
                    // band clear of all of them starts four pixels under the
                    // spot bar. Taken from the bar's own bottom edge rather
                    // than from a count of where the plates were assumed to
                    // reach: that count said "roughly waterfallTopY + 22" and
                    // put this ruler's ticks at + 26, which is inside a plate
                    // on any platform whose 13 px line box runs to 18 px or
                    // more -- the same mis-measurement that let the callsigns
                    // out of the bar in the first place.
                    readonly property real tickTopY:
                        dxSpotOverlay.spotBarBottomY + 4
                    readonly property real tickHeightPx: 4
                    // Plated label depth at this font size, rounded up.
                    readonly property real labelDepthPx: 16
                    readonly property real bandBottomY:
                        tickTopY + tickHeightPx + labelDepthPx
                    // The band must also clear the tune arrows, which are
                    // centred on 0.68 of the panel height and stand 58 px
                    // tall at either edge. Every panel this window can produce
                    // is tall enough, but the check is cheap and a collision
                    // there would be ugly.
                    readonly property bool bandFitsPanel:
                        bandBottomY < height * 0.68 - 30

                    // Fewer ticks than the main axis's 7, and fewer still on a
                    // narrow panel. A plated label runs about 70 px at this
                    // size, so 110 px between tick centres is the closest two
                    // may sit without their plates touching at any span this
                    // axis can show.
                    readonly property int tickCount:
                        width >= 440 ? 5 : (width >= 220 ? 3 : 2)

                    Repeater {
                        model: audioOffsetRuler.tickCount
                        delegate: Item {
                            id: audioOffsetTick
                            required property int index
                            readonly property real fraction:
                                index / (audioOffsetRuler.tickCount - 1)
                            readonly property bool firstTick: index === 0
                            readonly property bool lastTick:
                                index === audioOffsetRuler.tickCount - 1
                            x: fraction * audioOffsetRuler.width
                            y: audioOffsetRuler.tickTopY
                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 1
                                height: audioOffsetRuler.tickHeightPx
                                color: "#8290a0"
                            }
                            Label {
                                objectName: "audioOffsetRulerLabel"
                                x: audioOffsetTick.firstTick
                                   ? 2
                                   : (audioOffsetTick.lastTick
                                      ? -implicitWidth - 2
                                      : -implicitWidth / 2)
                                y: audioOffsetRuler.tickHeightPx + 1
                                leftPadding: 4
                                rightPadding: 4
                                topPadding: 1
                                bottomPadding: 1
                                // Like every other label on this panel this is
                                // drawn straight over live waterfall speckle,
                                // so it carries its own semi-opaque plate.
                                // "AF" rides the first label: these numbers
                                // must never be mistaken for the RF ruler at
                                // the foot of the panel.
                                text: (audioOffsetTick.firstTick ? "AF  " : "")
                                      + window.formatFrequency(
                                          spectrumDisplay.lowerFrequencyHz
                                          + audioOffsetTick.fraction
                                            * audioOffsetRuler.spanHz)
                                color: "#91a0b1"
                                font.pixelSize: 10
                                background: Rectangle {
                                    color: "#c8080f16"
                                    radius: 3
                                }
                            }
                        }
                    }
                }
                Label {
                    visible: txSliceGuideOverlay.visible
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 14
                    text: "TX slice  "
                          + window.formatVfoFrequency(
                              appSettings.radioTxVfoFrequencyHz)
                          + "  •  " + appSettings.cwGuideWidthHz.toFixed(0)
                          + " Hz wide"
                    color: "#ff7b84"
                    font.pixelSize: 10
                    z: 4
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    visible: !replayController.activeSource
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: replayController.sourceMode === 0
                              ? "Live receiver audio"
                              : (replayController.sourceMode === 2
                                 ? "Direct SDR receiver"
                                 : "Replay a receiver recording")
                        font.pixelSize: 17
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: replayController.sourceMode === 0
                              ? "Start live RX to process the selected audio input"
                              : (replayController.sourceMode === 2
                                 ? appSettings.sdrDiagnostic
                                 : "Open a PCM or 32-bit float WAV file to inspect its real spectrum and waterfall")
                        color: "#8290a0"
                    }
                    Button {
                        objectName: "emptyStateStartButton"
                        Layout.alignment: Qt.AlignHCenter
                        text: replayController.sourceMode === 0
                              ? "Start live RX"
                              : (replayController.sourceMode === 2
                                 ? "Start SDR RX" : "Choose WAV recording")
                        enabled: replayController.sourceMode !== 2
                                 || (appSettings.sdrBackendAvailable
                                     && appSettings.sdrDeviceIndex >= 0)
                        onClicked: replayController.sourceMode === 0
                                   ? replayController.startLiveAudio()
                                   : (replayController.sourceMode === 2
                                      ? replayController.startLiveSdr()
                                      : wavDialog.open())
                        ToolTip.visible: hovered
                        ToolTip.text: replayController.sourceMode === 0
                            ? "Start spectrum analysis and CW decoding"
                            : (replayController.sourceMode === 2
                               ? appSettings.sdrDiagnostic
                               : "Choose a receiver WAV recording")
                    }
                }
            }

            Frame {
                id: liveControlsFrame
                objectName: "liveControlsFrame"
                property bool pinned: false
                readonly property bool expanded: pinned || controlsHover.hovered
                                                 || viewSelector.popup.visible
                Layout.fillWidth: true
                Layout.preferredHeight: expanded
                                        ? controlsContent.implicitHeight + 16
                                        : controlsHeader.implicitHeight + 16
                padding: 8
                clip: true
                background: Rectangle {
                    radius: 8
                    color: "#151b23"
                    border.color: "#2b3541"
                }
                ColumnLayout {
                    id: controlsContent
                    width: parent.width
                    spacing: 4
                    RowLayout {
                        id: controlsHeader
                        Layout.fillWidth: true
                        Label {
                            text: liveControlsFrame.expanded
                                  ? "Live spectrum controls"
                                  : "Live spectrum controls — hover to open"
                            font.weight: Font.DemiBold
                        }
                        TabBar {
                            id: liveControlTabs
                            Layout.preferredWidth: 190
                            TabButton { text: "Signal" }
                            TabButton { text: "Display" }
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: "Save profile"
                            onClicked: appSettings.apply()
                            ToolTip.visible: hovered
                            ToolTip.text: "Save the current live spectrum controls in this profile"
                        }
                        ToolButton {
                            objectName: "pinLiveControlsButton"
                            text: liveControlsFrame.pinned ? "Unpin" : "Pin"
                            checkable: true
                            checked: liveControlsFrame.pinned
                            onToggled: liveControlsFrame.pinned = checked
                            ToolTip.visible: hovered
                            ToolTip.text: checked
                                          ? "Restore automatic hiding"
                                          : "Keep controls open"
                        }
                    }
                    StackLayout {
                        Layout.fillWidth: true
                        enabled: liveControlsFrame.expanded
                        opacity: liveControlsFrame.expanded ? 1.0 : 0.0
                        currentIndex: liveControlTabs.currentIndex
                        ScrollView {
                            Layout.fillWidth: true
                            implicitHeight: signalControls.implicitHeight + 4
                            contentWidth: signalControls.implicitWidth
                            contentHeight: signalControls.implicitHeight
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
                            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                            RowLayout {
                                id: signalControls
                                spacing: 8
                            CheckBox {
                                text: "DC rejection"
                                checked: appSettings.audioDcRejection
                                onToggled: appSettings.audioDcRejection = checked
                            }
                            CheckBox {
                                text: "Auto gain"
                                checked: appSettings.audioAutomaticGain
                                onToggled: appSettings.audioAutomaticGain = checked
                            }
                            Label { text: appSettings.audioAutomaticGain ? "Target" : "Gain" }
                            SpinBox {
                                editable: true
                                from: -40
                                to: appSettings.audioAutomaticGain ? -1 : 40
                                value: Math.round(appSettings.audioAutomaticGain
                                                  ? appSettings.audioAutomaticGainTargetDbfs
                                                  : appSettings.audioGainDb)
                                onValueModified: {
                                    if (appSettings.audioAutomaticGain)
                                        appSettings.audioAutomaticGainTargetDbfs = value
                                    else
                                        appSettings.audioGainDb = value
                                }
                            }
                            Label { text: "dB" + (appSettings.audioAutomaticGain ? "FS" : "") }
                            CheckBox {
                                text: "Auto bandwidth"
                                checked: appSettings.audioAutomaticBandwidth
                                onToggled: appSettings.audioAutomaticBandwidth = checked
                            }
                            Label { text: "Low"; visible: !appSettings.audioAutomaticBandwidth }
                            SpinBox {
                                editable: true
                                from: 0
                                to: 95950
                                value: Math.round(appSettings.audioLowerFrequencyHz)
                                visible: !appSettings.audioAutomaticBandwidth
                                onValueModified: appSettings.audioLowerFrequencyHz = value
                            }
                            Label { text: "High"; visible: !appSettings.audioAutomaticBandwidth }
                            SpinBox {
                                editable: true
                                from: 50
                                to: 96000
                                value: Math.round(appSettings.audioUpperFrequencyHz)
                                visible: !appSettings.audioAutomaticBandwidth
                                onValueModified: appSettings.audioUpperFrequencyHz = value
                            }
                            Label { text: "Hz"; visible: !appSettings.audioAutomaticBandwidth }
                            }
                        }
                        GridLayout {
                            id: displayControls
                            Layout.fillWidth: true
                            columns: width >= 1200 ? 6 : 4
                            columnSpacing: 14
                            rowSpacing: 4

                            ColumnLayout {
                                Label { text: "View"; font.pixelSize: 11 }
                                ComboBox {
                                    id: viewSelector
                                    objectName: "viewSelector"
                                    Layout.preferredWidth: 150
                                    model: ["Audio spectrum", "CW symbols"]
                                    currentIndex: appSettings.spectrumDisplayMode
                                    onActivated: appSettings.spectrumDisplayMode = currentIndex
                                }
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                caption: "FPS"
                                from: 10; to: 120; stepSize: 1
                                value: appSettings.targetFps
                                onMoved: value => appSettings.targetFps = Math.round(value)
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                caption: "Lines / second"
                                from: 1; to: 120; stepSize: 1
                                value: appSettings.waterfallRate
                                onMoved: value => appSettings.waterfallRate = Math.round(value)
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                caption: "Averaging"
                                from: 1; to: 32; stepSize: 1
                                value: appSettings.averagingFrames
                                onMoved: value => appSettings.averagingFrames = Math.round(value)
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                caption: "History (seconds)"
                                from: 5; to: 30; stepSize: 1
                                value: appSettings.waterfallTimeSpanSeconds
                                onMoved: value => appSettings.waterfallTimeSpanSeconds = Math.round(value)
                            }
                            CheckBox {
                                objectName: "liveAutomaticLevelsCheck"
                                text: "Auto levels"
                                checked: appSettings.automaticRange
                                onToggled: appSettings.automaticRange = checked
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                visible: appSettings.automaticRange
                                caption: "Automatic span (dB)"
                                from: 30; to: 100; stepSize: 1
                                value: appSettings.automaticRangeSpanDb
                                onMoved: value => appSettings.automaticRangeSpanDb = value
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                visible: !appSettings.automaticRange
                                caption: "Floor (dBFS)"
                                from: -200; to: 40; stepSize: 1
                                value: appSettings.lowerBoundDb
                                onMoved: value => appSettings.lowerBoundDb = value
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                visible: !appSettings.automaticRange
                                caption: "Ceiling (dBFS)"
                                from: -190; to: 50; stepSize: 1
                                value: appSettings.upperBoundDb
                                onMoved: value => appSettings.upperBoundDb = value
                            }
                            CheckBox {
                                objectName: "liveCwGuideCheck"
                                text: "TX slice guide"
                                checked: appSettings.showCwGuide
                                onToggled: appSettings.showCwGuide = checked
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                caption: "CW reference tone (Hz)"
                                from: 0
                                to: Math.max(3000, spectrumDisplay.upperFrequencyHz)
                                stepSize: 10
                                value: appSettings.cwGuideCenterHz
                                onMoved: value => appSettings.cwGuideCenterHz = value
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                caption: "TX slice width (Hz)"
                                from: 10; to: 5000; stepSize: 10
                                value: appSettings.cwGuideWidthHz
                                enabled: appSettings.showCwGuide
                                onMoved: value => appSettings.cwGuideWidthHz = value
                            }
                            CheckBox {
                                objectName: "liveNoiseSuppressionCheck"
                                text: "Suppress audio noise"
                                checked: appSettings.waterfallNoiseSuppression
                                enabled: appSettings.spectrumDisplayMode === 0
                                onToggled: appSettings.waterfallNoiseSuppression = checked
                            }
                            LabeledSlider {
                                Layout.fillWidth: true
                                caption: "Audio margin (dB)"
                                from: 0; to: 30; stepSize: 1
                                value: appSettings.waterfallNoiseMarginDb
                                enabled: appSettings.spectrumDisplayMode === 0
                                         && appSettings.waterfallNoiseSuppression
                                onMoved: value => appSettings.waterfallNoiseMarginDb = value
                            }
                            Label {
                                text: "Noise " + spectrumDisplay.estimatedNoiseFloorDb.toFixed(0)
                                      + " dBFS"
                                color: "#8290a0"
                            }
                        }
                    }
                }
                HoverHandler { id: controlsHover }
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: replayController.statusText; color: "#91a0b1"; elide: Text.ElideRight; Layout.preferredWidth: 300 }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: replayController.sourceMode === 1
                    from: 0
                    to: Math.max(0.001, replayController.durationSeconds)
                    value: replayController.positionSeconds
                }
                Label {
                    text: replayController.sourceMode !== 1
                          ? "Input overruns: " + replayController.inputOverruns + "  •  "
                            + appSettings.targetFps + " FPS  •  " + appSettings.waterfallRate + " rows/s"
                          : replayController.positionSeconds.toFixed(1) + " / "
                            + replayController.durationSeconds.toFixed(1) + " s  •  "
                            + appSettings.targetFps + " FPS  •  " + appSettings.waterfallRate + " rows/s"
                    color: "#667789"
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 470
            Layout.minimumWidth: 430
            Layout.fillHeight: true
            color: "#111720"
            border.color: "#263241"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                RowLayout {
                    Layout.fillWidth: true
                    visible: sdrRadioDisplay.visible
                    Label {
                        text: "SDR Radio Control"
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: appSettings.sdrDeviceDisplayName
                        color: "#6f8396"
                        font.pixelSize: 10
                        elide: Text.ElideRight
                        Layout.maximumWidth: 245
                    }
                }
                Rectangle {
                    id: sdrRadioDisplay
                    objectName: "sdrRadioDisplay"
                    property bool frequencyEditing: false
                    property bool invalidFrequency: false
                    // Radio Control's tile geometry, mirrored here so both
                    // faceplates draw the same square hand-drawn controls.
                    readonly property int controlButtonSize: 52
                    readonly property var rateModel: appSettings.sdrSampleRateOptions.length > 0
                                                     ? appSettings.sdrSampleRateOptions
                                                     : [62500, 96000, 125000,
                                                        192000, 250000, 500000,
                                                        1000000, 2000000]
                    readonly property var bandwidthModel: appSettings.sdrBandwidthOptions.length > 1
                                                          ? appSettings.sdrBandwidthOptions
                                                          : [0, 200000, 300000,
                                                             600000, 1536000,
                                                             5000000, 8000000]
                    visible: replayController.sourceMode === 2
                             && appSettings.sdrBackendAvailable
                             && appSettings.sdrDeviceIndex >= 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: 188
                    Layout.minimumHeight: 188
                    radius: 8
                    color: "#111821"
                    border.color: "#326875"
                    border.width: 1
                    clip: true

                    // A faceplate tile has room for a magnitude, not for
                    // six digits: 1536000 reads as "1.54M", not "1536k". The
                    // exact value stays in each control's tooltip.
                    function compactHz(hz) {
                        return hz >= 1000000
                                ? Number((hz / 1000000).toFixed(2)) + "M"
                                : Number((hz / 1000).toFixed(1)) + "k"
                    }

                    function beginFrequencyEdit() {
                        sdrFrequencyField.text = window.formatVfoInput(
                                    appSettings.sdrDecoderCenterFrequencyHz,
                                    1000)
                        invalidFrequency = false
                        frequencyEditing = true
                        sdrFrequencyField.forceActiveFocus()
                        sdrFrequencyField.selectAll()
                    }
                    function dismissFrequencyEdit() {
                        invalidFrequency = false
                        frequencyEditing = false
                    }
                    function acceptFrequencyEdit() {
                        var value = Number(sdrFrequencyField.text.replace(",", "."))
                        var frequencyHz = Math.round(value * 1000)
                        if (Number.isFinite(frequencyHz) && frequencyHz > 0
                                && appSettings.requestSdrRxFrequencyHz(
                                    frequencyHz)) {
                            dismissFrequencyEdit()
                            window.contentItem.forceActiveFocus()
                        } else {
                            invalidFrequency = true
                            sdrFrequencyField.forceActiveFocus()
                            sdrFrequencyField.selectAll()
                        }
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 7
                        spacing: 5
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 62
                            spacing: 6
                            ToolButton {
                                objectName: "sdrFrequencyDownButton"
                                Layout.preferredWidth: 48
                                Layout.preferredHeight: 56
                                text: "<"
                                font.pixelSize: 22
                                onClicked: appSettings.stepSdrRxFrequency(-1)
                                ToolTip.visible: hovered
                                ToolTip.text: "Move the SDR acquisition window down "
                                              + (appSettings.sdrTuningStepHz / 1000)
                                              + " kHz"
                            }
                            Item {
                                id: sdrFrequencyEditor
                                Layout.fillWidth: true
                                Layout.preferredHeight: 56
                                Rectangle {
                                    anchors.fill: parent
                                    radius: 4
                                    color: "#031014"
                                    border.color: sdrRadioDisplay.invalidFrequency
                                                  ? "#ff7b84" : "#397b87"
                                    border.width: sdrRadioDisplay.frequencyEditing
                                                  ? 2 : 1
                                }
                                Label {
                                    objectName: "sdrRxFrequencyLabel"
                                    anchors.left: parent.left
                                    anchors.leftMargin: 10
                                    anchors.right: parent.right
                                    anchors.rightMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: !sdrRadioDisplay.frequencyEditing
                                    text: window.formatRigFrequency(
                                              appSettings.sdrDecoderCenterFrequencyHz)
                                    color: "#75f0e0"
                                    font.family: "monospace"
                                    font.pixelSize: 27
                                    fontSizeMode: Text.Fit
                                    minimumPixelSize: 15
                                    font.weight: Font.Bold
                                    font.letterSpacing: 2
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 9
                                    anchors.top: parent.top
                                    anchors.topMargin: 4
                                    visible: !sdrRadioDisplay.frequencyEditing
                                    text: "SDR · RX"
                                    color: "#64dff0"
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    visible: !sdrRadioDisplay.frequencyEditing
                                    hoverEnabled: true
                                    cursorShape: Qt.IBeamCursor
                                    onClicked: sdrRadioDisplay.beginFrequencyEdit()
                                    ToolTip.visible: containsMouse
                                    ToolTip.text: "Click to enter the SDR tuned RX frequency in kHz; the profile LO offset is applied to the hardware acquisition centre"
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    visible: sdrRadioDisplay.frequencyEditing
                                    Label {
                                        text: "RX"
                                        color: "#64dff0"
                                        font.weight: Font.Bold
                                    }
                                    TextField {
                                        id: sdrFrequencyField
                                        objectName: "sdrFrequencyField"
                                        Layout.fillWidth: true
                                        selectByMouse: true
                                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                                        validator: RegularExpressionValidator {
                                            regularExpression: /[0-9]{1,8}([.,][0-9]{0,3})?/
                                        }
                                        color: sdrRadioDisplay.invalidFrequency
                                               ? "#ff7b84" : "#e8fffb"
                                        selectionColor: "#2dd4a7"
                                        selectedTextColor: "#03100b"
                                        font.family: "monospace"
                                        font.pixelSize: 20
                                        font.weight: Font.Bold
                                        background: Rectangle {
                                            radius: 3
                                            color: "#02090b"
                                            border.color: sdrRadioDisplay.invalidFrequency
                                                          ? "#ff7b84" : "#4c8c94"
                                        }
                                        Keys.onPressed: function(event) {
                                            if (event.key === Qt.Key_Escape) {
                                                sdrRadioDisplay.dismissFrequencyEdit()
                                                window.contentItem.forceActiveFocus()
                                                event.accepted = true
                                            } else if (event.key === Qt.Key_Return
                                                       || event.key === Qt.Key_Enter) {
                                                sdrRadioDisplay.acceptFrequencyEdit()
                                                event.accepted = true
                                            }
                                        }
                                        onActiveFocusChanged: {
                                            if (!activeFocus
                                                    && sdrRadioDisplay.frequencyEditing)
                                                sdrRadioDisplay.dismissFrequencyEdit()
                                        }
                                    }
                                    Label { text: "kHz"; color: "#7fb6b2" }
                                }
                            }
                            ToolButton {
                                objectName: "sdrFrequencyUpButton"
                                Layout.preferredWidth: 48
                                Layout.preferredHeight: 56
                                text: ">"
                                font.pixelSize: 22
                                onClicked: appSettings.stepSdrRxFrequency(1)
                                ToolTip.visible: hovered
                                ToolTip.text: "Move the SDR acquisition window up "
                                              + (appSettings.sdrTuningStepHz / 1000)
                                              + " kHz"
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            // Radio Sync is a square Radio Control tile, so
                            // this row is one tile tall. 62 + 52 + 40 plus two
                            // 5 px gaps still fits the 174 px of faceplate
                            // height left inside the 188 px panel.
                            Layout.preferredHeight: sdrRadioDisplay.controlButtonSize
                            spacing: 5
                            ComboBox {
                                id: sdrModeCombo
                                objectName: "sdrOperatingModeCombo"
                                Layout.fillWidth: true
                                // No Layout.preferredWidth: setting one below
                                // the implicit width is exactly what elided
                                // these boxes. Material keeps the drop
                                // indicator inside the right padding, so
                                // roughly 58 px of every combo is never
                                // available to glyphs; only a minimum width
                                // floors that. The minimums in this faceplate
                                // are budgeted against the narrowest panel
                                // (Layout.minimumWidth 430, less 32 px of panel
                                // margins and 14 px of faceplate margins,
                                // leaves 384 px per row): 132 + 150 + 52 tile
                                // + 10 px of gaps = 344. 132 also holds the
                                // widest fallback the driver can name, a mode
                                // with no code at all ("MODE Default").
                                Layout.minimumWidth: 132
                                // The dense faceplate font buys back the glyph
                                // room the drop indicator takes away, and
                                // matches the 9-13 px Radio Control tiles.
                                font.pixelSize: 11
                                model: appSettings.sdrOperatingModeNames
                                currentIndex: appSettings.sdrOperatingModeIndex
                                visible: model.length > 1
                                // The driver names a mode
                                // "ST — Single tuner (recommended)". Only the
                                // code fits a faceplate, so the full name stays
                                // in the popup and the tooltip. The model
                                // itself is untouched.
                                displayText: "MODE " + currentText.split(" — ")[0]
                                // A short display text must not shrink the
                                // popup that carries the full mode names.
                                Component.onCompleted: popup.width = Qt.binding(
                                    function() {
                                        return Math.max(sdrModeCombo.width, 260)
                                    })
                                onActivated: appSettings.selectSdrOperatingMode(
                                                 currentIndex)
                                ToolTip.visible: hovered
                                ToolTip.text: "Receiver operating mode exposed by the SDR driver: "
                                              + currentText
                            }
                            ComboBox {
                                id: sdrAntennaCombo
                                objectName: "sdrControlAntennaCombo"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 150
                                font.pixelSize: 11
                                model: appSettings.sdrAntennaNames
                                currentIndex: appSettings.sdrAntennaIndex
                                visible: model.length > 0
                                // Antenna, port and tuner-input names are free
                                // text from the driver and are routinely wider
                                // than the control, so the popup is widened
                                // rather than the selection being guessed at.
                                Component.onCompleted: popup.width = Qt.binding(
                                    function() {
                                        return Math.max(sdrAntennaCombo.width, 220)
                                    })
                                onActivated: appSettings.selectSdrAntenna(
                                                 currentIndex)
                                ToolTip.visible: hovered
                                ToolTip.text: "Select the SDR antenna, port, or tuner input: "
                                              + currentText
                            }
                            Rectangle {
                                id: sdrSyncTile
                                objectName: "sdrCatSyncButton"
                                Layout.preferredWidth: sdrRadioDisplay.controlButtonSize
                                Layout.preferredHeight: sdrRadioDisplay.controlButtonSize
                                radius: 5
                                // Hand-drawn like every Radio Control tile
                                // instead of a Material Button: a checked
                                // Button keeps its pill shape and repaints only
                                // its label, so an engaged sync read as
                                // disengaged. Colour now carries the state and
                                // the sub-label carries the direction, which
                                // also removes the baked-in ellipsis of the old
                                // "SYNC …" caption.
                                readonly property bool engaged: appSettings.sdrFollowRadioVfo
                                readonly property bool waiting:
                                    appSettings.sdrRadioSyncStatus
                                    .indexOf("waiting") >= 0
                                readonly property bool bidirectional:
                                    appSettings.sdrRadioSyncStatus
                                    .indexOf("bidirectional") >= 0
                                color: !engaged
                                       ? (sdrSyncMouse.containsMouse
                                          ? "#414a55" : "#353b43")
                                       // Engaged but not yet locked onto the
                                       // radio stays a step down the same
                                       // green, so the tile never claims a
                                       // sync it does not have.
                                       : waiting
                                         ? (sdrSyncMouse.containsMouse
                                            ? "#36a08d" : "#2b8474")
                                         : (sdrSyncMouse.containsMouse
                                            ? "#63e0c7" : "#43c6ac")
                                Column {
                                    anchors.centerIn: parent
                                    spacing: 1
                                    Label {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: "SYNC"
                                        color: sdrSyncTile.engaged
                                               ? "#111318"
                                               : (appSettings.radioEnabled
                                                  ? "#7b8794" : "#5a636d")
                                        font.pixelSize: 13
                                        font.weight: Font.Bold
                                    }
                                    Label {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: !sdrSyncTile.engaged
                                              ? "OFF"
                                              : sdrSyncTile.waiting
                                                ? "WAIT"
                                                : sdrSyncTile.bidirectional
                                                  ? "↔" : "←"
                                        color: sdrSyncTile.engaged
                                               ? "#123f38"
                                               : (appSettings.radioEnabled
                                                  ? "#5f6b78" : "#4a525b")
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                    }
                                }
                                MouseArea {
                                    id: sdrSyncMouse
                                    anchors.fill: parent
                                    // Hover stays live while CAT is off so the
                                    // tooltip can say why the tile is inert;
                                    // only the click itself is gated.
                                    hoverEnabled: true
                                    cursorShape: appSettings.radioEnabled
                                                 ? Qt.PointingHandCursor
                                                 : Qt.ArrowCursor
                                    onClicked: {
                                        if (!appSettings.radioEnabled)
                                            return
                                        appSettings.sdrFollowRadioVfo =
                                                !appSettings.sdrFollowRadioVfo
                                    }
                                }
                                ToolTip.visible: sdrSyncMouse.containsMouse
                                ToolTip.text: !appSettings.radioEnabled
                                    ? "Radio Sync needs CAT radio control enabled in Settings"
                                    : appSettings.sdrRadioSyncStatus
                                      + (sdrSyncTile.engaged
                                         ? "\nClick to stop following the radio VFO"
                                         : "\nClick to follow the radio VFO")
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            spacing: 5
                            // Three controls share the 384 px this row has in
                            // the narrowest panel; their minimums total 342.
                            ComboBox {
                                objectName: "sdrControlSampleRateCombo"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 112
                                font.pixelSize: 11
                                model: sdrRadioDisplay.rateModel
                                currentIndex: model.indexOf(
                                                  appSettings.sdrSampleRateHz)
                                displayText: sdrRadioDisplay.compactHz(
                                                 appSettings.sdrSampleRateHz)
                                             + "S/s"
                                onActivated: appSettings.sdrSampleRateHz = currentValue
                                ToolTip.visible: hovered
                                // The removed decimation badge only ever
                                // carried this sentence, and it belongs on the
                                // control that actually decides decimation.
                                ToolTip.text: "Effective IQ sample rate ("
                                              + appSettings.sdrSampleRateHz
                                              + " Hz); changing it may restart SDR reception. The driver derives hardware decimation from the selected effective IQ rate; SoapySDR exposes no independent decimation control"
                            }
                            ComboBox {
                                objectName: "sdrControlBandwidthCombo"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 112
                                font.pixelSize: 11
                                model: sdrRadioDisplay.bandwidthModel
                                currentIndex: model.indexOf(
                                                  appSettings.sdrBandwidthHz)
                                displayText: appSettings.sdrBandwidthHz === 0
                                             ? "RF BW AUTO"
                                             : "RF BW "
                                               + sdrRadioDisplay.compactHz(
                                                   appSettings.sdrBandwidthHz)
                                onActivated: appSettings.sdrBandwidthHz = currentValue
                                ToolTip.visible: hovered
                                ToolTip.text: appSettings.sdrBandwidthHz === 0
                                    ? "Hardware RF filter bandwidth, chosen by the driver; changing it may restart reception"
                                    : "Hardware RF filter bandwidth ("
                                      + appSettings.sdrBandwidthHz
                                      + " Hz); changing it may restart reception"
                            }
                            ComboBox {
                                objectName: "sdrTuningStepCombo"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 118
                                font.pixelSize: 11
                                model: [100, 500, 1000, 2500, 5000, 10000,
                                        25000, 50000, 100000]
                                currentIndex: model.indexOf(
                                                  appSettings.sdrTuningStepHz)
                                displayText: "STEP "
                                             + (appSettings.sdrTuningStepHz >= 1000
                                                ? (appSettings.sdrTuningStepHz / 1000)
                                                  + "k"
                                                : appSettings.sdrTuningStepHz)
                                onActivated: appSettings.sdrTuningStepHz = currentValue
                                ToolTip.visible: hovered
                                ToolTip.text: "Frequency step used by the SDR < and > controls"
                            }
                        }
                    }
                }
                Rectangle {
                    visible: sdrRadioDisplay.visible
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#263241"
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: vfoDisplay.visible
                    Label {
                        text: "Radio Control"
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: appSettings.radioDisplayName
                        color: "#6f8396"
                        font.pixelSize: 10
                        elide: Text.ElideRight
                        Layout.maximumWidth: 220
                    }
                }
                Rectangle {
                    id: vfoDisplay
                    objectName: "vfoDisplay"
                    property int controlButtonSize: 52
                    // Radio control is independent from the receive source.
                    // A direct SDR may receive while a separate CAT radio owns
                    // the TX VFO in a full-duplex station profile.
                    visible: replayController.radioFrequencyAvailable
                    Layout.fillWidth: true
                    Layout.preferredHeight: 188
                    Layout.minimumHeight: 188
                    radius: 8
                    color: "#111821"
                    border.color: "#3b5267"
                    border.width: 1
                    clip: true

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 7
                        spacing: 8
                        ColumnLayout {
                            Layout.preferredWidth: vfoDisplay.controlButtonSize
                            Layout.alignment: Qt.AlignTop
                            spacing: 5
                            Rectangle {
                                id: onAirIndicator
                                objectName: "onAirIndicator"
                                // Bound only to guarded local KEY state.
                                property bool active: transmitController.onAir
                                Layout.preferredWidth: vfoDisplay.controlButtonSize
                                Layout.preferredHeight: vfoDisplay.controlButtonSize
                                color: "transparent"
                                border.width: 0
                                Image {
                                    anchors.centerIn: parent
                                    source: "qrc:/icons/on-air-active.png"
                                    width: 28
                                    height: 28
                                    fillMode: Image.PreserveAspectFit
                                    opacity: onAirIndicator.active ? 1.0 : 0.16
                                }
                                ToolTip.visible: onAirMouse.containsMouse
                                ToolTip.delay: 300
                                ToolTip.text: active
                                    ? "KEY is authoritatively asserted"
                                    : "Not transmitting"
                                MouseArea {
                                    id: onAirMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                }
                            }
                            Rectangle {
                                objectName: "radioTuneButton"
                                Layout.preferredWidth: vfoDisplay.controlButtonSize
                                Layout.preferredHeight: vfoDisplay.controlButtonSize
                                radius: 5
                                color: transmitController.tuning ? "#ff6a24"
                                       : transmitController.armed
                                         ? (radioTuneMouse.containsMouse
                                            ? "#ff9a45" : "#e56b1f")
                                         : "#2b2520"
                                border.color: transmitController.armed
                                              ? "#ffb05c" : "#493321"
                                border.width: transmitController.tuning ? 2 : 1
                                Label {
                                    anchors.centerIn: parent
                                    text: transmitController.tuning
                                          ? "TUNE " + Math.max(0, Math.ceil(
                                                transmitController.txRemainingSeconds))
                                          : "TUNE"
                                    color: transmitController.armed
                                           ? "#fff3df" : "#806c5b"
                                    font.pixelSize: transmitController.tuning ? 9 : 10
                                    font.weight: Font.Bold
                                }
                                MouseArea {
                                    id: radioTuneMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: transmitController.armed
                                    cursorShape: enabled ? Qt.PointingHandCursor
                                                         : Qt.ArrowCursor
                                    onClicked: transmitController.toggleTune()
                                }
                                ToolTip.visible: radioTuneMouse.containsMouse
                                ToolTip.delay: 300
                                ToolTip.text: transmitController.armed
                                    ? "Operator-only KEY/PTT tune with a hard 15-second watchdog"
                                    : "Arm TX in the QSO panel before using TUNE"
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 5

                            Item {
                                id: vfoRxEditor
                                objectName: "vfoRxEditor"
                                property bool editing: false
                                property bool invalidEntry: false
                                property int inputUnitHz: 1000
                                property string inputUnitLabel: inputUnitHz === 1000000
                                                                ? "MHz" : "kHz"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 60
                                Layout.minimumHeight: 60
                                function beginEdit() {
                                    if (!appSettings.radioFrequencyWritable)
                                        return
                                    inputUnitHz = replayController.radioRxFrequencyHz
                                                  >= 30000000 ? 1000000 : 1000
                                    vfoRxFrequencyField.text = window.formatVfoInput(
                                                replayController.radioRxFrequencyHz,
                                                inputUnitHz)
                                    invalidEntry = false
                                    editing = true
                                    vfoRxFrequencyField.forceActiveFocus()
                                    vfoRxFrequencyField.selectAll()
                                }
                                function dismissEdit() {
                                    invalidEntry = false
                                    editing = false
                                }
                                function cancelEdit() {
                                    dismissEdit()
                                    window.contentItem.forceActiveFocus()
                                }
                                function acceptEdit() {
                                    if (appSettings.setControlledRxFrequency(
                                                vfoRxFrequencyField.text,
                                                inputUnitHz)) {
                                        invalidEntry = false
                                        editing = false
                                        window.contentItem.forceActiveFocus()
                                    } else {
                                        invalidEntry = true
                                        vfoRxFrequencyField.forceActiveFocus()
                                        vfoRxFrequencyField.selectAll()
                                    }
                                }
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    anchors.right: parent.right
                                    anchors.rightMargin: vfoDisplay.controlButtonSize + 5
                                    radius: 3
                                    color: "#050b10"
                                    border.color: vfoRxEditor.invalidEntry
                                                  ? "#ff7b84" : "#46515e"
                                    border.width: vfoRxEditor.editing ? 2 : 1
                                }
                                Label {
                                    id: vfoRxLabel
                                    objectName: "vfoRxLabel"
                                    anchors.left: parent.left
                                    anchors.leftMargin: 12
                                    anchors.right: parent.right
                                    anchors.rightMargin: 76
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: !vfoRxEditor.editing
                                    text: window.formatRigFrequency(
                                              replayController.radioRxFrequencyHz)
                                    color: "#f5f8fb"
                                    font.family: "monospace"
                                    font.pixelSize: 28
                                    fontSizeMode: Text.Fit
                                    minimumPixelSize: 16
                                    font.weight: Font.Bold
                                    font.letterSpacing: 2
                                    horizontalAlignment: Text.AlignRight
                                    elide: Text.ElideNone
                                }
                                Label {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 10
                                    anchors.top: parent.top
                                    anchors.topMargin: 6
                                    visible: !vfoRxEditor.editing
                                    text: "VFO " + appSettings.radioRxVfo + "  ·  RX"
                                    color: "#64dff0"
                                    font.pixelSize: 11
                                    font.weight: Font.Bold
                                }
                                Rectangle {
                                    objectName: "vfoRxModeBadge"
                                    z: 2
                                    anchors.right: parent.right
                                    anchors.rightMargin: 0
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: vfoDisplay.controlButtonSize
                                    height: vfoDisplay.controlButtonSize
                                    radius: 5
                                    color: appSettings.radioRxMode === "?"
                                           ? "#242d38"
                                           : rxModeMouse.containsMouse
                                             ? "#eff8ff" : "#dbe8f4"
                                    border.color: appSettings.radioRxModeWritable
                                                  ? "#64dff0" : "#46515e"
                                    Label {
                                        anchors.centerIn: parent
                                        text: appSettings.radioRxMode
                                        color: appSettings.radioRxMode === "?"
                                               ? "#718092" : "#07121b"
                                        font.pixelSize: 13
                                        font.weight: Font.Bold
                                    }
                                    MouseArea {
                                        id: rxModeMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        enabled: appSettings.radioRxModeWritable
                                        cursorShape: enabled ? Qt.PointingHandCursor
                                                             : Qt.ArrowCursor
                                        onClicked: appSettings.cycleControlledRxMode()
                                    }
                                    ToolTip.visible: rxModeMouse.containsMouse
                                    ToolTip.text: appSettings.radioRxModeWritable
                                        ? "Click to select the next RX mode"
                                        : "RX mode is provider read-only or unavailable"
                                }
                                MouseArea {
                                    id: vfoRxEditHitArea
                                    objectName: "vfoRxEditHitArea"
                                    anchors.fill: parent
                                    anchors.rightMargin: 72
                                    visible: !vfoRxEditor.editing
                                    enabled: appSettings.radioFrequencyWritable
                                    hoverEnabled: true
                                    activeFocusOnTab: enabled
                                    cursorShape: enabled ? Qt.IBeamCursor
                                                         : Qt.ArrowCursor
                                    Accessible.role: Accessible.Button
                                    Accessible.name: "Edit RX frequency"
                                    Accessible.description: "Enter an exact receive frequency"
                                    Keys.onPressed: function(event) {
                                        if (event.key === Qt.Key_Return
                                                || event.key === Qt.Key_Enter
                                                || event.key === Qt.Key_Space) {
                                            vfoRxEditor.beginEdit()
                                            event.accepted = true
                                        }
                                    }
                                    onClicked: {
                                        forceActiveFocus()
                                        vfoRxEditor.beginEdit()
                                    }
                                }
                                RowLayout {
                                    id: vfoRxEditRow
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 8
                                    visible: vfoRxEditor.editing
                                    spacing: 6
                                    Label {
                                        text: "RX"
                                        color: "#64dff0"
                                        font.family: "monospace"
                                        font.pixelSize: 18
                                        font.weight: Font.Bold
                                    }
                                    TextField {
                                        id: vfoRxFrequencyField
                                        objectName: "vfoRxFrequencyField"
                                        Layout.fillWidth: true
                                        selectByMouse: true
                                        color: vfoRxEditor.invalidEntry
                                               ? "#ff7b84" : "#f5f8fb"
                                        selectionColor: "#2dd4a7"
                                        selectedTextColor: "#03100b"
                                        font.family: "monospace"
                                        font.pixelSize: 22
                                        font.weight: Font.Bold
                                        font.letterSpacing: 2
                                        background: Rectangle {
                                            radius: 3
                                            color: "#02070a"
                                            border.color: vfoRxEditor.invalidEntry
                                                          ? "#ff7b84" : "#647180"
                                            border.width: 1
                                        }
                                        Keys.onPressed: function(event) {
                                            if (event.key === Qt.Key_Escape) {
                                                vfoRxEditor.cancelEdit()
                                                event.accepted = true
                                            } else if (event.key === Qt.Key_Return
                                                       || event.key === Qt.Key_Enter) {
                                                vfoRxEditor.acceptEdit()
                                                event.accepted = true
                                            }
                                        }
                                        onActiveFocusChanged: {
                                            if (!activeFocus && vfoRxEditor.editing)
                                                vfoRxEditor.dismissEdit()
                                        }
                                    }
                                    Label {
                                        text: vfoRxEditor.inputUnitLabel
                                        color: "#91a0b1"
                                        font.family: "monospace"
                                        font.pixelSize: 13
                                    }
                                }
                                ToolTip.visible: vfoRxEditHitArea.containsMouse
                                ToolTip.delay: 300
                                ToolTip.text: appSettings.radioFrequencyWritable
                                    ? "Click to enter an exact RX frequency"
                                    : "The linked provider reports frequency read-only"
                                Connections {
                                    target: appSettings
                                    function onRadioFrequencyControlChanged() {
                                        if (!appSettings.radioFrequencyWritable) {
                                            vfoRxEditor.invalidEntry = false
                                            vfoRxEditor.editing = false
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: vfoDisplay.controlButtonSize
                                Layout.minimumHeight: vfoDisplay.controlButtonSize
                                spacing: 5
                                Item {
                                    id: vfoTxEditor
                                    objectName: "vfoTxEditor"
                                    property bool editing: false
                                    property bool invalidEntry: false
                                    property int inputUnitHz: 1000
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Layout.minimumWidth: 150
                                    function beginEdit() {
                                        if (!appSettings.radioTxFrequencyWritable)
                                            return
                                        inputUnitHz = appSettings.radioTxVfoFrequencyHz
                                                      >= 30000000 ? 1000000 : 1000
                                        vfoTxFrequencyField.text = window.formatVfoInput(
                                                    appSettings.radioTxVfoFrequencyHz,
                                                    inputUnitHz)
                                        invalidEntry = false
                                        editing = true
                                        vfoTxFrequencyField.forceActiveFocus()
                                        vfoTxFrequencyField.selectAll()
                                    }
                                    function dismissEdit() {
                                        invalidEntry = false
                                        editing = false
                                    }
                                    function acceptEdit() {
                                        if (appSettings.setControlledTxFrequency(
                                                    vfoTxFrequencyField.text,
                                                    inputUnitHz)) {
                                            dismissEdit()
                                            window.contentItem.forceActiveFocus()
                                        } else {
                                            invalidEntry = true
                                            vfoTxFrequencyField.forceActiveFocus()
                                            vfoTxFrequencyField.selectAll()
                                        }
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: 5
                                        color: "#110d08"
                                        border.color: vfoTxEditor.invalidEntry
                                                      ? "#ff7b84" : "#59452f"
                                    }
                                    Label {
                                        objectName: "vfoTxLabel"
                                        anchors.left: parent.left
                                        anchors.leftMargin: 7
                                        anchors.right: parent.right
                                        anchors.rightMargin: 7
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: !vfoTxEditor.editing
                                        text: window.formatRigFrequency(
                                                  appSettings.radioTxVfoFrequencyHz > 0
                                                  ? appSettings.radioTxVfoFrequencyHz
                                                  : replayController.radioRxFrequencyHz)
                                        color: replayController.radioSplitActive
                                               ? "#ff6a24" : "#9b694e"
                                        font.family: "monospace"
                                        font.pixelSize: 20
                                        fontSizeMode: Text.Fit
                                        minimumPixelSize: 14
                                        font.weight: Font.Bold
                                        font.letterSpacing: 1
                                        horizontalAlignment: Text.AlignRight
                                        elide: Text.ElideNone
                                    }
                                    Label {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 7
                                        anchors.top: parent.top
                                        anchors.topMargin: 3
                                        visible: !vfoTxEditor.editing
                                        text: "VFO " + appSettings.radioTxVfo + " · TX"
                                        color: replayController.radioSplitActive
                                               ? "#d38b59" : "#705848"
                                        font.pixelSize: 8
                                        font.weight: Font.Bold
                                    }
                                    MouseArea {
                                        id: txFrequencyMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        visible: !vfoTxEditor.editing
                                        enabled: appSettings.radioTxFrequencyWritable
                                        cursorShape: enabled ? Qt.IBeamCursor : Qt.ArrowCursor
                                        onClicked: vfoTxEditor.beginEdit()
                                    }
                                    ToolTip.visible: txFrequencyMouse.containsMouse
                                    ToolTip.text: appSettings.radioTxFrequencyWritable
                                        ? "Click to enter the VFO B / TX frequency; this enables split if needed"
                                        : "TX frequency is provider read-only or unavailable"
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 6
                                        visible: vfoTxEditor.editing
                                        Label {
                                            text: "TX"
                                            color: "#ff9b54"
                                            font.weight: Font.Bold
                                        }
                                        TextField {
                                            id: vfoTxFrequencyField
                                            objectName: "vfoTxFrequencyField"
                                            Layout.fillWidth: true
                                            selectByMouse: true
                                            color: vfoTxEditor.invalidEntry ? "#ff7b84" : "#fff3df"
                                            selectionColor: "#ff9b54"
                                            selectedTextColor: "#1b0d03"
                                            font.family: "monospace"
                                            font.pixelSize: 17
                                            font.weight: Font.Bold
                                            background: Rectangle {
                                                radius: 3
                                                color: "#050302"
                                                border.color: vfoTxEditor.invalidEntry
                                                              ? "#ff7b84" : "#795f40"
                                            }
                                            Keys.onPressed: function(event) {
                                                if (event.key === Qt.Key_Escape) {
                                                    vfoTxEditor.dismissEdit()
                                                    window.contentItem.forceActiveFocus()
                                                    event.accepted = true
                                                } else if (event.key === Qt.Key_Return
                                                           || event.key === Qt.Key_Enter) {
                                                    vfoTxEditor.acceptEdit()
                                                    event.accepted = true
                                                }
                                            }
                                            onActiveFocusChanged: {
                                                if (!activeFocus && vfoTxEditor.editing)
                                                    vfoTxEditor.dismissEdit()
                                            }
                                        }
                                        Label {
                                            text: vfoTxEditor.inputUnitHz === 1000000 ? "MHz" : "kHz"
                                            color: "#b59c80"
                                        }
                                    }
                                }
                                Rectangle {
                                    objectName: "vfoTxModeBadge"
                                    Layout.preferredWidth: vfoDisplay.controlButtonSize
                                    Layout.preferredHeight: vfoDisplay.controlButtonSize
                                    radius: 5
                                    color: appSettings.radioTxModeConfirmed
                                           ? (txModeMouse.containsMouse
                                              ? "#ffd18e" : "#ffbd63")
                                           : (txModeMouse.containsMouse
                                              ? "#443620" : "#332918")
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 1
                                        Label {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: appSettings.radioTxModeTarget
                                            color: appSettings.radioTxModeConfirmed
                                                   ? "#1d1004" : "#ffbd63"
                                            font.pixelSize: 13
                                            font.weight: Font.Bold
                                        }
                                        Label {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: appSettings.radioTxModeConfirmed
                                                  ? "CONFIRMED" : "TARGET"
                                            color: appSettings.radioTxModeConfirmed
                                                   ? "#48300e" : "#d69a48"
                                            font.pixelSize: 7
                                            font.weight: Font.DemiBold
                                        }
                                    }
                                    ToolTip.visible: txModeMouse.containsMouse
                                    ToolTip.text: appSettings.radioTxModeConfirmed
                                        ? "The provider confirms this TX mode. Click to toggle CW / CW-R."
                                        : appSettings.radioTxModeWritable
                                          ? "Operator TX target; provider readback is "
                                            + appSettings.radioTxMode
                                            + ". Click to request the other CW mode."
                                          : "Operator TX target; this provider cannot apply or confirm the TX-VFO mode. Click to toggle CW / CW-R."
                                    MouseArea {
                                        id: txModeMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: appSettings.toggleControlledTxMode()
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: vfoDisplay.controlButtonSize
                                Layout.minimumHeight: vfoDisplay.controlButtonSize
                                spacing: 5
                                Item { Layout.fillWidth: true }
                                Rectangle {
                                    objectName: "vfoSplitBadge"
                                    Layout.preferredWidth: vfoDisplay.controlButtonSize
                                    Layout.preferredHeight: vfoDisplay.controlButtonSize
                                    radius: 5
                                    color: replayController.radioSplitActive
                                           ? (splitModeMouse.containsMouse
                                              ? "#ffff4c" : "#f0f21c")
                                           : (splitModeMouse.containsMouse
                                              ? "#414a55" : "#353b43")
                                    Label {
                                        id: splitBadgeLabel
                                        anchors.centerIn: parent
                                        text: !appSettings.radioSplitKnown ? "SPLIT ?"
                                              : replayController.radioSplitActive
                                                ? "SPLIT" : "SIMPLEX"
                                        color: replayController.radioSplitActive
                                               ? "#111318" : "#7b8794"
                                        font.pixelSize: 9
                                        font.weight: Font.Bold
                                    }
                                    MouseArea {
                                        id: splitModeMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        enabled: appSettings.radioSplitWritable
                                        cursorShape: enabled ? Qt.PointingHandCursor
                                                             : Qt.ArrowCursor
                                        onClicked: appSettings.setControlledSplit(
                                                       !replayController.radioSplitActive)
                                    }
                                    ToolTip.visible: splitModeMouse.containsMouse
                                    ToolTip.text: appSettings.radioSplitWritable
                                        ? (replayController.radioSplitActive
                                           ? "Click to return the radio to simplex"
                                           : "Click to enable independent RX/TX VFO split")
                                        : "Split state is provider read-only or unavailable"
                                }
                                Rectangle {
                                    objectName: "vfoFrequencySyncButton"
                                    Layout.preferredWidth: vfoDisplay.controlButtonSize
                                    Layout.preferredHeight: vfoDisplay.controlButtonSize
                                    radius: 5
                                    color: syncFrequencyMouse.containsMouse
                                           ? "#243746" : "#1d2833"
                                    border.color: appSettings.radioTxFrequencySyncAvailable
                                                  ? "#6d91a8" : "#394550"
                                    border.width: 1
                                    Label {
                                        anchors.centerIn: parent
                                        text: "A=B"
                                        color: appSettings.radioTxFrequencySyncAvailable
                                               ? "#d8edf7" : "#65727e"
                                        font.pixelSize: 12
                                        font.weight: Font.Bold
                                    }
                                    MouseArea {
                                        id: syncFrequencyMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        enabled: appSettings.radioTxFrequencySyncAvailable
                                        cursorShape: enabled ? Qt.PointingHandCursor
                                                             : Qt.ArrowCursor
                                        onClicked: appSettings.syncControlledTxFrequencyToRx()
                                    }
                                    ToolTip.visible: syncFrequencyMouse.containsMouse
                                    ToolTip.text: appSettings.radioTxFrequencySyncAvailable
                                        ? "Copy VFO A / RX frequency to VFO B / TX; TX mode is not copied"
                                        : "Frequency sync requires known RX state plus writable TX frequency and split control"
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                }
                Label {
                    objectName: "vfoRxEditErrorLabel"
                    visible: vfoDisplay.visible &&
                             (vfoRxEditor.invalidEntry || vfoTxEditor.invalidEntry)
                    text: appSettings.statusMessage
                    color: "#ff7b84"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Rectangle {
                    visible: vfoDisplay.visible
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#263241"
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: "CW Decoder"
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    ToolButton {
                        id: diagnosticsToggle
                        objectName: "diagnosticsToggle"
                        text: checked ? "Hide diagnostics" : "Diagnostics"
                        checkable: true
                        font.pixelSize: 10
                        ToolTip.visible: hovered
                        ToolTip.text: checked
                            ? "Hide decoder verification diagnostics"
                            : "Show why tracks are accepted or rejected"
                    }
                    ToolButton {
                        objectName: "debugCaptureButton"
                        text: replayController.debugCaptureActive ? "Stop capture" : "Debug capture"
                        font.pixelSize: 10
                        enabled: replayController.debugCaptureActive || replayController.liveCapturing
                        onClicked: replayController.debugCaptureActive
                                   ? replayController.stopDebugCapture()
                                   : replayController.startDebugCapture()
                        ToolTip.visible: hovered
                        ToolTip.delay: 300
                        ToolTip.text: "Records raw live audio and per-track decoder internals to disk for troubleshooting a signal that will not decode. Stops itself after the limit set in Settings → Decoder. Review the saved files before sharing them — the audio is whatever the selected input picked up."
                    }
                }
                Label {
                    visible: replayController.debugCaptureActive || replayController.debugCapturePath.length > 0
                    text: replayController.debugCaptureActive
                          ? "Capturing… " + replayController.debugCaptureElapsedSeconds.toFixed(0) + "s / " + appSettings.debugCaptureMaximumSeconds + "s max — " + replayController.debugCapturePath
                          : "Last capture: " + replayController.debugCaptureNote + " — " + replayController.debugCapturePath
                    color: replayController.debugCaptureActive ? "#f3bd55" : "#6c7c8e"
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                    Layout.fillWidth: true
                }
                Label {
                    text: replayController.decoderChannelCount > 0
                          ? replayController.decoderChannelCount
                            + " signal(s) detected • "
                            + replayController.decoderSessionCount
                            + " session(s) open"
                          : replayController.decoderSessionCount > 0
                            ? "Manual slice open • awaiting CW verification"
                          : "Scanning the complete processed passband"
                    color: "#8290a0"
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    id: diagnosticsSummaryLabel
                    objectName: "diagnosticsSummaryLabel"
                    property string summary: ""
                    visible: diagnosticsToggle.checked && summary.length > 0
                    text: summary
                    color: "#6c7c8e"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Timer {
                        // Sampled at most once per second so this is a
                        // troubleshooting snapshot, not a flickering readout.
                        interval: 1000
                        running: diagnosticsToggle.checked
                        repeat: true
                        triggeredOnStart: true
                        onTriggered: diagnosticsSummaryLabel.summary =
                            window.verificationDiagnosticsSummary(
                                replayController.verificationDiagnostics)
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#263241" }
                Label {
                    visible: replayController.decoderSessionCount === 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    text: replayController.decoderChannelCount > 0
                          ? "Click a colored signal marker in the spectrum or waterfall to open its decoded session here. Closed sessions continue decoding and can be reopened."
                          : "Listening for CW signals across the spectrum…\n\nThe red TX-slice boundaries follow authoritative VFO B readback and do not limit decoding."
                    color: "#667789"
                    font.pixelSize: 15
                    wrapMode: Text.Wrap
                    verticalAlignment: Text.AlignTop
                }
                ListView {
                    id: decoderChannelList
                    objectName: "decoderChannelList"
                    visible: replayController.decoderSessionCount > 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    // A list taller than the space it is given scrolls, but
                    // without a bar there is nothing to say so: an operator
                    // with more open sessions than fit saw the ones that fit
                    // and no sign the rest existed. The bar is the affordance
                    // as much as the control.
                    ScrollBar.vertical: ScrollBar {
                        objectName: "decoderChannelScrollBar"
                        policy: decoderChannelList.contentHeight
                                > decoderChannelList.height
                                ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
                    }
                    // Reordering stays a drag on the card's own grip, which is
                    // a DragHandler scoped to it, so flicking the list body is
                    // unaffected and the wheel still scrolls.
                    boundsBehavior: Flickable.StopAtBounds
                    // Keeping the card the operator is reading in view matters
                    // more than keeping the top of the list in view: sessions
                    // arrive and are reordered underneath them.
                    highlightFollowsCurrentItem: false
                    model: replayController.decoderSessionModel
                    delegate: Rectangle {
                        id: sessionCard
                        required property var modelData
                        required property int index
                        // Lifted while being dragged so the card being moved is
                        // obvious against the ones it passes.
                        opacity: sessionDragHandler.active ? 0.85 : 1.0
                        width: decoderChannelList.width
                        // Derive the delegate height from its actual rows. A
                        // fixed card height let platform font/control metrics
                        // push the transcript, local-model status, or footer
                        // through the rounded border.
                        height: Math.ceil(sessionCardLayout.implicitHeight + 20)
                        radius: 7
                        color: "#151d27"
                        // Key-down is represented by the activity LED. Do not
                        // resize/repaint the card border at Morse cadence.
                        border.width: 1
                        border.color: modelData.color
                        clip: true
                        z: sessionDragHandler.active ? 10 : 1
                        property string rawDecodedText: modelData.text.length > 0
                            ? modelData.text
                            : (modelData.provisionalText.length > 0
                               ? modelData.provisionalText
                               : (modelData.elements.length > 0
                                  ? modelData.elements
                                  : (!modelData.verifiedCw
                                     ? "Analyzing the selected frequency…"
                                     : "Listening…")))
                        // Prefer the turn-aware presentation. It changes only
                        // conservative word boundaries and separates completed
                        // transmissions; raw and phase-consensus evidence stay
                        // available to diagnostics without modification.
                        property string correctedDecodedText:
                            modelData.contextualText.length > 0
                            ? modelData.contextualText
                            : (modelData.refinedText.length > 0
                               ? modelData.refinedText : modelData.text)
                        // Show the current acoustic character immediately,
                        // then let stable/contextual output replace it in
                        // place. Depending on decoder lock, provisionalText
                        // is either the whole raw prefix or only its suffix.
                        property string displayedDecodedText: {
                            var corrected = correctedDecodedText
                            var pending = modelData.provisionalText
                            if (pending.length === 0)
                                return corrected.length > 0
                                    ? corrected : rawDecodedText
                            var stableRaw = modelData.text
                            var suffix = pending.indexOf(stableRaw) === 0
                                ? pending.substring(stableRaw.length)
                                : pending
                            return corrected + suffix
                        }
                        property string callsignEvidenceText:
                            rawDecodedText + " " + modelData.refinedText
                            + " " + modelData.contextualText
                        property string ownCallEvidenceText:
                            callsignEvidenceText + " " + localModelStableText
                        property string localModelState:
                            modelData.localModelState
                        property string localModelStatus:
                            modelData.localModelStatus
                        property string localModelStableText:
                            modelData.localModelText
                        property string localModelCallsign:
                            modelData.localModelCallsign
                        property string advisoryCallsignSuggestion:
                            modelData.callsignSuggestion
                        property bool streamMonitored:
                            replayController.monitorMode === 2
                            && replayController.isMonitorChannelEnabled(
                                modelData.id)
                        property string callsignSuggestionSource:
                            modelData.callsignSuggestionSource
                        property bool localModelHasText:
                            localModelState === "ready"
                            && localModelStableText.length > 0
                        property int ownCallMatches: window.exactCallCount(
                            ownCallEvidenceText, appSettings.ownCallsign)
                        property int previousOwnCallMatches: 0
                        onOwnCallMatchesChanged: {
                            if (ownCallMatches > previousOwnCallMatches)
                                ownCallFlashAnimation.restart()
                            previousOwnCallMatches = ownCallMatches
                        }
                        Rectangle {
                            id: ownCallFlash
                            anchors.fill: parent
                            radius: sessionCard.radius
                            color: "transparent"
                            border.color: "#ffd54f"
                            border.width: 4
                            opacity: 0
                            z: 30
                        }
                        SequentialAnimation {
                            id: ownCallFlashAnimation
                            loops: 5
                            NumberAnimation {
                                target: ownCallFlash
                                property: "opacity"
                                from: 0
                                to: 0.9
                                duration: 160
                            }
                            NumberAnimation {
                                target: ownCallFlash
                                property: "opacity"
                                from: 0.9
                                to: 0
                                duration: 240
                            }
                        }
                        ColumnLayout {
                            id: sessionCardLayout
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                Rectangle {
                                    width: 12
                                    height: 12
                                    radius: 6
                                    color: modelData.active
                                           ? modelData.color : "#354352"
                                    border.width: modelData.active ? 2 : 1
                                    border.color: modelData.active
                                                  ? "#dffeff" : "#566576"
                                    ToolTip.visible: activityHover.hovered
                                    ToolTip.text: modelData.active
                                        ? "Signal active; listening and decoding"
                                        : "Signal retained; waiting for activity"
                                    HoverHandler { id: activityHover }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: {
                                        if (modelData.qsoParticipants.length >= 2) {
                                            return "QSO  "
                                                + modelData.qsoParticipants[0]
                                                + " ↔ "
                                                + modelData.qsoParticipants[1]
                                        }
                                        var station = modelData.callsign
                                        if (station.length === 0) {
                                            station = sessionCard.localModelCallsign
                                        }
                                        if (station.length === 0
                                                && sessionCard.advisoryCallsignSuggestion.length > 0) {
                                            station = "≈ " + sessionCard.advisoryCallsignSuggestion
                                        }
                                        return station.length > 0
                                            ? station : "Identifying…"
                                    }
                                    color: modelData.color
                                    font.weight: Font.Bold
                                    font.pixelSize: 16
                                    minimumPixelSize: 12
                                    fontSizeMode: Text.Fit
                                    elide: Text.ElideNone
                                }
                                Label {
                                    objectName: "callsignDatabaseBadge"
                                    visible: modelData.callsign.length > 0
                                             && modelData.callsignDatabaseLoaded
                                    text: modelData.callsignInDatabase
                                          ? "\u2713 LISTED" : "DECODED"
                                    color: modelData.callsignInDatabase
                                           ? "#0b1a10" : "#91a0b1"
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                    leftPadding: 5
                                    rightPadding: 5
                                    topPadding: 2
                                    bottomPadding: 2
                                    background: Rectangle {
                                        radius: 3
                                        color: modelData.callsignInDatabase
                                               ? "#7fd18a" : "transparent"
                                        border.color: modelData.callsignInDatabase
                                                      ? "#7fd18a" : "#3a4756"
                                        border.width: 1
                                    }
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 400
                                    ToolTip.text: modelData.callsignInDatabase
                                        ? "This callsign appears in the offline callsign list."
                                        : "Decoded from the air. It is not in the offline callsign list, which is normal for an unlisted station."
                                    property bool hovered: badgeHover.hovered
                                    HoverHandler { id: badgeHover }
                                }
                                Label {
                                    visible: modelData.callsign.length === 0
                                             && sessionCard.localModelCallsign.length > 0
                                    text: "MODEL"
                                    color: "#80cbc4"
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                }
                                Label {
                                    objectName: "advisoryCallsignSuggestionBadge"
                                    visible: modelData.callsign.length === 0
                                             && sessionCard.localModelCallsign.length === 0
                                             && sessionCard.advisoryCallsignSuggestion.length > 0
                                    text: sessionCard.callsignSuggestionSource
                                          === "offline-directory" ? "DB" : "AUDIO"
                                    color: sessionCard.callsignSuggestionSource
                                           === "offline-directory"
                                           ? "#f3bd55" : "#80cbc4"
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                    Accessible.name: "Advisory callsign suggestion"
                                    Accessible.description: "Advisory match "
                                                            + sessionCard.advisoryCallsignSuggestion
                                                            + " for decoded span "
                                                            + modelData.callsignSuggestionRawSpan
                                    ToolTip.visible: hovered
                                    ToolTip.text: (sessionCard.callsignSuggestionSource
                                                   === "offline-directory"
                                                   ? "Advisory offline-directory match for "
                                                   : "Advisory acoustic consensus for ")
                                                  + modelData.callsignSuggestionRawSpan
                                                  + "; decoded text is unchanged"
                                }
                                Label {
                                    visible: sessionCard.ownCallMatches > 0
                                    text: "YOUR CALL HEARD"
                                    color: "#ffd54f"
                                    font.pixelSize: 11
                                    font.weight: Font.Bold
                                }
                                Label {
                                    visible: !modelData.verifiedCw
                                    text: "MANUAL"
                                    color: "#718091"
                                    font.pixelSize: 10
                                }
                                ToolButton {
                                    objectName: "decoderSessionDragHandle"
                                    text: "⠿"
                                    // Cards are reordered by dragging this
                                    // handle. The keyboard path is kept for
                                    // operators who cannot drag, and for
                                    // accessibility: the same control moves the
                                    // card with the arrow keys when focused.
                                    Accessible.name: "Reorder decoded session"
                                    Accessible.description:
                                        "Drag to reposition, or use the up and down arrow keys"
                                    focusPolicy: Qt.StrongFocus
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 500
                                    ToolTip.text: "Drag to reorder this card"
                                    Keys.onUpPressed: if (sessionCard.index > 0)
                                        replayController.moveDecoderSession(
                                            modelData.id, sessionCard.index - 1)
                                    Keys.onDownPressed:
                                        if (sessionCard.index + 1 < decoderChannelList.count)
                                            replayController.moveDecoderSession(
                                                modelData.id, sessionCard.index + 1)
                                    DragHandler {
                                        id: sessionDragHandler
                                        objectName: "decoderSessionDragHandler"
                                        // The list owns delegate placement, so
                                        // the card is not moved directly. The
                                        // travelled distance is converted into
                                        // a position change on release, which
                                        // keeps the list authoritative and
                                        // needs no reparenting.
                                        target: null
                                        xAxis.enabled: false
                                        yAxis.enabled: true
                                        property real pressY: 0
                                        onActiveChanged: {
                                            if (active) {
                                                pressY = centroid.scenePosition.y
                                                return
                                            }
                                            var travelled = centroid.scenePosition.y - pressY
                                            var step = Math.round(
                                                travelled / Math.max(1, sessionCard.height))
                                            if (step === 0) return
                                            var target = Math.max(0, Math.min(
                                                decoderChannelList.count - 1,
                                                sessionCard.index + step))
                                            if (target !== sessionCard.index)
                                                replayController.moveDecoderSession(
                                                    modelData.id, target)
                                        }
                                    }
                                }
                                ToolButton {
                                    objectName: "closeDecoderSessionButton"
                                    text: "×"
                                    z: 40
                                    Accessible.name: "Close decoded session"
                                    onPressed: replayController.closeDecoderSession(
                                                   modelData.id)
                                    ToolTip.visible: hovered
                                    ToolTip.text: "Close this card; decoding continues in the background"
                                }
                            }
                            Label {
                                objectName: "decoderSessionFrequencyLabel"
                                Layout.fillWidth: true
                                text: window.formatStreamFrequency(modelData)
                                color: "#91a0b1"
                                font.pixelSize: 11
                                font.family: "monospace"
                                elide: Text.ElideRight
                            }
                            Label {
                                objectName: "currentSenderLabel"
                                Layout.fillWidth: true
                                visible: modelData.currentSenderCallsign.length > 0
                                text: "CURRENT SENDER  "
                                      + modelData.currentSenderCallsign
                                      + (modelData.currentSenderWpm > 0
                                         ? "  •  "
                                           + modelData.currentSenderWpm.toFixed(0)
                                           + " WPM"
                                         : "")
                                color: "#64dff0"
                                font.pixelSize: 12
                                font.weight: Font.Bold
                                ToolTip.visible: senderHelp.hovered
                                ToolTip.text: "Attributed only from an explicit decoded CALL1 DE CALL2 or CQ DE CALL handover"
                                HoverHandler { id: senderHelp }
                            }
                            Rectangle {
                                id: transcriptFrame
                                objectName: "decoderTranscriptFrame"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 112
                                radius: 4
                                color: "#0b121a"
                                border.color: "#263241"
                                border.width: 1
                                clip: true
                                ScrollView {
                                    id: transcriptScroll
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    clip: true
                                    property bool followTail: true
                                    function maximumContentY() {
                                        if (!contentItem)
                                            return 0
                                        return Math.max(0, contentItem.contentHeight
                                                           - contentItem.height)
                                    }
                                    function isAtTail() {
                                        return !contentItem
                                               || contentItem.contentY
                                                  >= maximumContentY() - 2
                                    }
                                    function pinToTail() {
                                        if (followTail && contentItem)
                                            contentItem.contentY = maximumContentY()
                                    }
                                    function followAppendedText() {
                                        if (decodedTextArea.selectionStart
                                                !== decodedTextArea.selectionEnd) {
                                            followTail = false
                                            return
                                        }
                                        if (!followTail)
                                            return
                                        // Pin immediately so the tail is already
                                        // correct in the frame the text grows in,
                                        // then again once layout settles, because
                                        // the content height for a wrapped line is
                                        // only final after that pass.
                                        pinToTail()
                                        Qt.callLater(function() {
                                            if (transcriptScroll.followTail
                                                    && transcriptScroll.contentItem) {
                                                transcriptScroll.contentItem.contentY =
                                                    transcriptScroll.maximumContentY()
                                            }
                                        })
                                    }
                                    ScrollBar.horizontal: ScrollBar {
                                        policy: ScrollBar.AlwaysOff
                                    }
                                    ScrollBar.vertical: ScrollBar {
                                        id: transcriptVerticalBar
                                        policy: ScrollBar.AsNeeded
                                        onPressedChanged: {
                                            if (!pressed)
                                                transcriptScroll.followTail =
                                                    transcriptScroll.isAtTail()
                                        }
                                    }
                                    Connections {
                                        target: transcriptScroll.contentItem
                                        function onMovementStarted() {
                                            // Wheel/touch scrolling is an explicit
                                            // request to inspect earlier output.
                                            transcriptScroll.followTail = false
                                        }
                                        function onMovementEnded() {
                                            transcriptScroll.followTail =
                                                transcriptScroll.isAtTail()
                                        }
                                        // Growing content would otherwise leave the
                                        // viewport short of the new bottom until
                                        // something else moved it.
                                        function onContentHeightChanged() {
                                            transcriptScroll.pinToTail()
                                        }
                                    }
                                    TextArea {
                                        id: decodedTextArea
                                        objectName: "decodedSessionText"
                                        readOnly: true
                                        selectByMouse: true
                                        width: transcriptScroll.availableWidth
                                        // The fixed outer frame stays in place
                                        // while only this padded text content
                                        // scrolls. Its border can therefore
                                        // never scroll through the first line.
                                        height: Math.max(
                                                    transcriptScroll.availableHeight,
                                                    implicitHeight)
                                        text: ""
                                        textFormat: TextEdit.PlainText
                                        color: modelData.text.length > 0
                                               ? "#edf3f8"
                                               : (modelData.provisionalText.length > 0
                                                  || modelData.elements.length > 0
                                                  ? "#e3ad55" : "#8290a0")
                                        font.pixelSize: 16
                                        font.italic: modelData.text.length === 0
                                        wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                                        leftPadding: 9
                                        rightPadding: 9
                                        topPadding: 9
                                        bottomPadding: 9
                                        leftInset: 0
                                        rightInset: 0
                                        topInset: 0
                                        bottomInset: 0
                                        background: null
                                        ToolTip.visible: transcriptHover.hovered
                                        ToolTip.delay: 500
                                        ToolTip.text: modelData.transmissions.length > 0
                                            ? "A sustained pause starts a new line. Only cadence-supported word gaps are repaired; decoded characters remain visible while later corrections settle."
                                            : "Live decoded text; select and scroll to pause automatic tail following"
                                        HoverHandler { id: transcriptHover }
                                        function applyDecodedText(nextText) {
                                        var oldSelectionStart = selectionStart
                                        var oldSelectionEnd = selectionEnd
                                        var hadSelection = oldSelectionStart
                                                           !== oldSelectionEnd
                                        // The transcript is append-only while a
                                        // station is being copied. Reassigning
                                        // the whole string rebuilds the text
                                        // document, which resets the viewport
                                        // and leaves the card showing a stale
                                        // offset until the next frame restores
                                        // it -- once per decoded character,
                                        // which reads as a constant shudder.
                                        // Insert only the new suffix so the
                                        // existing layout and scroll position
                                        // survive untouched.
                                        if (!hadSelection
                                                && nextText.length > length
                                                && nextText.indexOf(text)
                                                   === 0) {
                                            insert(length,
                                                   nextText.substring(length))
                                        } else {
                                            text = nextText
                                            if (hadSelection) {
                                                select(Math.min(oldSelectionStart, length),
                                                       Math.min(oldSelectionEnd, length))
                                            }
                                        }
                                        transcriptScroll.followAppendedText()
                                    }
                                        Component.onCompleted:
                                            applyDecodedText(
                                                sessionCard.displayedDecodedText)
                                        Connections {
                                            target: sessionCard
                                            function onDisplayedDecodedTextChanged() {
                                                decodedTextArea.applyDecodedText(
                                                    sessionCard.displayedDecodedText)
                                            }
                                        }
                                    }
                                }
                            }
                            Rectangle {
                                id: localModelTranscriptPanel
                                objectName: "localModelTranscriptPanel"
                                // Hidden when the optional local model is not
                                // in use. A panel reporting the state of a
                                // feature the operator has not set up is noise
                                // on every card, and it reported an error for a
                                // model that was never configured.
                                visible: sessionCard.localModelState !== "disabled"
                                         && sessionCard.localModelState !== "unconfigured"
                                Layout.fillWidth: true
                                Layout.preferredHeight: !visible ? 0
                                    : (sessionCard.localModelHasText ? 88 : 50)
                                radius: 4
                                color: "#101820"
                                border.color: sessionCard.localModelState
                                              === "error"
                                              ? "#c75b62" : "#263241"
                                border.width: 1
                                clip: true
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 7
                                    spacing: 3
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: "LOCAL MODEL"
                                            color: "#91a0b1"
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                        Item { Layout.fillWidth: true }
                                        Label {
                                            objectName: "localModelStateLabel"
                                            text: sessionCard.localModelState.toUpperCase()
                                            color: sessionCard.localModelState
                                                   === "error"
                                                   ? "#ef7d85" : "#8290a0"
                                            font.pixelSize: 9
                                        }
                                    }
                                    Label {
                                        objectName: "localModelStatusLabel"
                                        visible: !sessionCard.localModelHasText
                                        Layout.fillWidth: true
                                        text: sessionCard.localModelStatus
                                        color: "#8290a0"
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                    ScrollView {
                                        id: localModelTranscriptScroll
                                        visible: sessionCard.localModelHasText
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        property bool followTail: true
                                        function maximumContentY() {
                                            if (!contentItem)
                                                return 0
                                            return Math.max(
                                                0, contentItem.contentHeight
                                                   - contentItem.height)
                                        }
                                        function pinToTail() {
                                            if (followTail && contentItem)
                                                contentItem.contentY = maximumContentY()
                                        }
                                        function followAppendedText() {
                                            if (!followTail)
                                                return
                                            pinToTail()
                                            Qt.callLater(function() {
                                                if (localModelTranscriptScroll.followTail
                                                        && localModelTranscriptScroll.contentItem) {
                                                    localModelTranscriptScroll.contentItem.contentY =
                                                        localModelTranscriptScroll.maximumContentY()
                                                }
                                            })
                                        }
                                        ScrollBar.horizontal: ScrollBar {
                                            policy: ScrollBar.AlwaysOff
                                        }
                                        ScrollBar.vertical: ScrollBar {
                                            policy: ScrollBar.AlwaysOn
                                        }
                                        Connections {
                                            target: localModelTranscriptScroll.contentItem
                                            function onMovementStarted() {
                                                localModelTranscriptScroll.followTail = false
                                            }
                                            function onMovementEnded() {
                                                localModelTranscriptScroll.followTail =
                                                    localModelTranscriptScroll.contentItem.contentY
                                                    >= localModelTranscriptScroll.maximumContentY() - 2
                                            }
                                            function onContentHeightChanged() {
                                                localModelTranscriptScroll.pinToTail()
                                            }
                                        }
                                        TextArea {
                                            id: localModelTranscriptText
                                            objectName: "localModelTranscriptText"
                                            readOnly: true
                                            selectByMouse: true
                                            width: localModelTranscriptScroll.availableWidth
                                            height: Math.max(
                                                        localModelTranscriptScroll.availableHeight,
                                                        implicitHeight)
                                            text: ""
                                            textFormat: TextEdit.PlainText
                                            color: "#c8e6df"
                                            font.pixelSize: 14
                                            wrapMode: TextEdit.WrapAnywhere
                                            padding: 0
                                            background: null
                                            // Append-only applies within one
                                            // lane incarnation. A withdrawn
                                            // or non-prefix value marks a
                                            // lifecycle reset and must clear
                                            // stale text from a retained card.
                                            function applyStableText(nextText) {
                                                if (sessionCard.localModelState
                                                        !== "ready"
                                                        || nextText.length === 0) {
                                                    text = ""
                                                    localModelTranscriptScroll.followTail = true
                                                } else if (nextText.indexOf(text)
                                                           === 0) {
                                                    // Append only the suffix,
                                                    // for the same reason the
                                                    // decoded transcript does.
                                                    if (nextText.length > length)
                                                        insert(length,
                                                               nextText.substring(length))
                                                } else {
                                                    text = nextText
                                                    localModelTranscriptScroll.followTail = true
                                                }
                                                localModelTranscriptScroll.followAppendedText()
                                            }
                                            Component.onCompleted:
                                                applyStableText(
                                                    sessionCard.localModelStableText)
                                            Connections {
                                                target: sessionCard
                                                function onLocalModelStableTextChanged() {
                                                    localModelTranscriptText.applyStableText(
                                                        sessionCard.localModelStableText)
                                                }
                                                function onLocalModelStateChanged() {
                                                    localModelTranscriptText.applyStableText(
                                                        sessionCard.localModelStableText)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            Label {
                                objectName: "decoderMetricsLabel"
                                Layout.fillWidth: true
                                // Readable while operating. This was 10px
                                // low-contrast grey on one elided row, so the
                                // values were cut off and hard to read at all.
                                // Confidence now reports the character-averaged
                                // figure: the instantaneous one falls to zero
                                // between characters, so the line read 0%
                                // while text was arriving. The instantaneous
                                // key state is dropped entirely; it is already
                                // shown by the keyed marker, and as a number it
                                // only ever flickers.
                                text: (modelData.wpm > 0
                                       ? modelData.wpm.toFixed(0) + " WPM"
                                       : "WPM —")
                                      + "   •   " + modelData.snrDb.toFixed(0)
                                      + " dB   •   "
                                      + modelData.filterWidthHz.toFixed(0)
                                      + " Hz   •   "
                                      + (modelData.meanCharacterConfidence * 100).toFixed(0)
                                      + "% confidence"
                                color: "#c8d4e0"
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                            RowLayout {
                                objectName: "decoderSessionActionRow"
                                Layout.fillWidth: true
                                spacing: 8
                                Button {
                                    objectName: "decoderSessionTxButton"
                                    Layout.preferredWidth: 124
                                    Layout.preferredHeight: 38
                                    text: modelData.callsign.length > 0
                                          ? "TX " + modelData.callsign : "TX"
                                    enabled: transmitController.armed
                                             && modelData.callsign.length > 0
                                    onClicked: {
                                        transmitController.selectTarget(
                                            modelData.id, modelData.callsign,
                                            modelData.frequencyKind === "RF"
                                            ? modelData.displayFrequencyHz : 0,
                                            modelData.currentSenderWpm > 0
                                            ? modelData.currentSenderWpm
                                            : modelData.wpm)
                                        txDrawer.open()
                                    }
                                    ToolTip.visible: hovered
                                    ToolTip.text: modelData.callsign.length === 0
                                        ? "TX waits for an exactly decoded callsign"
                                        : (transmitController.armed
                                           ? "Select this exactly decoded station for guarded TX"
                                           : "Open QSO and arm TX first")
                                }
                                Item { Layout.fillWidth: true }
                                Button {
                                    objectName: "decoderSessionMonitorButton"
                                    Layout.preferredWidth: 124
                                    Layout.preferredHeight: 38
                                    text: sessionCard.streamMonitored
                                          ? "🔊  Monitor" : "🔈  Monitor"
                                    highlighted: sessionCard.streamMonitored
                                    Accessible.name: sessionCard.streamMonitored
                                        ? "Stop monitoring this CW stream"
                                        : "Monitor this CW stream"
                                    Accessible.description:
                                        "Several decoder-card streams can be monitored together"
                                    // Decoder delegates refresh while text is
                                    // arriving. Commit on press so a model
                                    // refresh between press and release cannot
                                    // swallow the operator's monitor action.
                                    onPressed: replayController.toggleMonitorChannel(
                                                   modelData.id)
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 350
                                    ToolTip.text: sessionCard.streamMonitored
                                        ? "Stop listening to this stream"
                                        : "Listen to this filtered stream; other enabled stream speakers remain active"
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Drawer {
        id: txDrawer
        edge: Qt.LeftEdge
        width: Math.min(window.width * 0.46, 620)
        height: window.height
        modal: true
        background: Rectangle { color: "#111720" }
        ScrollView {
            anchors.fill: parent
            contentWidth: availableWidth
            ColumnLayout {
                width: Math.max(0, parent.width - 40)
                x: 20
                spacing: 12
                Label {
                    text: "Guarded TX / QSO"
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#91a0b1"
                    text: "Decoder output may suggest an action, but it cannot key the transmitter. Arm TX, exactly confirm the station, then confirm every normalized message preview."
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "txArmButton"
                        text: transmitController.armed ? "Disarm TX" : "Arm TX"
                        onClicked: transmitController.armed
                                   ? transmitController.disarm()
                                   : transmitController.arm()
                        ToolTip.visible: hovered
                        ToolTip.text: transmitController.armed
                            ? "Disarm transmission and clear pending actions"
                            : "Enter the guarded TX workflow; this does not key hardware"
                    }
                    CheckBox {
                        objectName: "autoQsoModeCheck"
                        text: "Auto-QSO suggestions"
                        checked: transmitController.autoQsoEnabled
                        enabled: transmitController.armed
                        onToggled: transmitController.autoQsoEnabled = checked
                        ToolTip.visible: hovered
                        ToolTip.text: enabled
                            ? "Suggest context-matched replies; every message still requires exact confirmation"
                            : "Arm TX before enabling reply suggestions"
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        text: "EMERGENCY RELEASE"
                        highlighted: true
                        onClicked: transmitController.emergencyRelease()
                        ToolTip.visible: hovered
                        ToolTip.text: "Immediately release KEY/PTT and latch a fault"
                    }
                    Button {
                        objectName: "txTuneButton"
                        text: transmitController.tuning ? "STOP TUNE" : "TUNE"
                        enabled: transmitController.armed
                        highlighted: transmitController.tuning
                        onClicked: transmitController.toggleTune()
                        ToolTip.visible: hovered
                        ToolTip.text: "Operator-only KEY/tone toggle; hard 15-second watchdog"
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: transmitController.state === "fault"
                           ? "#ff7b84" : "#f3bd55"
                    text: transmitController.status
                }
                ColumnLayout {
                    objectName: "txProgressPanel"
                    Layout.fillWidth: true
                    spacing: 4
                    visible: transmitController.transmitting
                             || transmitController.tuning
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: transmitController.tuning ? "TUNE" : "TRANSMITTING"
                            color: "#ff7b84"
                            font.weight: Font.Bold
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            objectName: "txCountdownLabel"
                            text: transmitController.txRemainingSeconds.toFixed(1)
                                  + " s remaining  •  "
                                  + transmitController.txElapsedSeconds.toFixed(1)
                                  + " s elapsed"
                            color: "#ffffff"
                            font.family: "monospace"
                        }
                    }
                    ProgressBar {
                        objectName: "txProgressBar"
                        Layout.fillWidth: true
                        from: 0.0
                        to: 1.0
                        value: transmitController.txProgress
                        Accessible.name: transmitController.tuning
                            ? "TUNE watchdog progress" : "Transmission progress"
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: transmitController.stationReady ? "#62ffa2" : "#91a0b1"
                    text: "Hardware: " + transmitController.hardwareStatus
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: transmitController.state === "fault"
                    Button {
                        text: "Reset fault (stays disarmed)"
                        onClicked: transmitController.resetFault()
                        ToolTip.visible: hovered
                        ToolTip.text: "Clear the latched fault without arming transmission"
                    }
                }
                Label {
                    text: transmitController.targetCallsign.length > 0
                          ? "Selected station: " + transmitController.targetCallsign
                          : "Select TX on an exactly decoded receiver card"
                    color: "#62ffa2"
                    font.pixelSize: 17
                    font.weight: Font.Bold
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: transmitController.targetCallsign.length > 0
                             && !transmitController.qsoConfirmed
                    TextField {
                        id: txCallConfirmation
                        objectName: "txCallConfirmationField"
                        Layout.fillWidth: true
                        placeholderText: "Retype callsign exactly"
                        selectByMouse: true
                    }
                    Button {
                        text: "Confirm station"
                        onClicked: transmitController.confirmTarget(
                                       txCallConfirmation.text)
                        ToolTip.visible: hovered
                        ToolTip.text: "Accept only an exact retype of the selected decoded callsign"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: transmitController.qsoConfirmed
                    Button {
                        text: "Send my call"
                        onClicked: transmitController.prepareOwnCall()
                        ToolTip.visible: hovered
                        ToolTip.text: "Prepare your configured callsign for exact preview confirmation"
                    }
                    Button {
                        text: "End QSO"
                        onClicked: transmitController.endQso()
                        ToolTip.visible: hovered
                        ToolTip.text: "Clear the selected station and pending exchange"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: transmitController.qsoConfirmed
                    Label { text: "Report"; color: "#91a0b1" }
                    TextField {
                        id: txReportField
                        objectName: "txReportField"
                        Layout.fillWidth: true
                        text: transmitController.report
                        maximumLength: 32
                        selectByMouse: true
                        onEditingFinished: transmitController.report = text
                        ToolTip.visible: hovered
                        ToolTip.text: "Edit the operator-authored signal report"
                    }
                    Button {
                        text: "Prepare report"
                        onClicked: {
                            transmitController.report = txReportField.text
                            transmitController.prepareReport()
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Move this report into exact preview confirmation; it does not transmit"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: transmitController.qsoConfirmed
                    Label { text: "Exchange"; color: "#91a0b1" }
                    TextField {
                        id: txExchangeField
                        objectName: "txExchangeField"
                        Layout.fillWidth: true
                        text: transmitController.exchange
                        maximumLength: 64
                        selectByMouse: true
                        onEditingFinished: transmitController.exchange = text
                        ToolTip.visible: hovered
                        ToolTip.text: "Edit an operator-authored contest or conversational exchange"
                    }
                    Button {
                        text: "Prepare exchange"
                        onClicked: {
                            transmitController.exchange = txExchangeField.text
                            transmitController.prepareExchange()
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Move this exchange into exact preview confirmation; it does not transmit"
                    }
                }
                RowLayout {
                    objectName: "txMacroRow"
                    Layout.fillWidth: true
                    visible: transmitController.qsoConfirmed
                    Label { text: "Quick macros"; color: "#91a0b1" }
                    Button {
                        text: appSettings.txMacro1
                        visible: text.length > 0
                        onClicked: transmitController.prepareMacro(appSettings.txMacro1)
                        ToolTip.visible: hovered
                        ToolTip.text: "Prepare " + text + " for exact preview confirmation"
                    }
                    Button {
                        text: appSettings.txMacro2
                        visible: text.length > 0
                        onClicked: transmitController.prepareMacro(appSettings.txMacro2)
                        ToolTip.visible: hovered
                        ToolTip.text: "Prepare " + text + " for exact preview confirmation"
                    }
                    Button {
                        text: appSettings.txMacro3
                        visible: text.length > 0
                        onClicked: transmitController.prepareMacro(appSettings.txMacro3)
                        ToolTip.visible: hovered
                        ToolTip.text: "Prepare " + text + " for exact preview confirmation"
                    }
                    Button {
                        text: appSettings.txMacro4
                        visible: text.length > 0
                        onClicked: transmitController.prepareMacro(appSettings.txMacro4)
                        ToolTip.visible: hovered
                        ToolTip.text: "Prepare " + text + " for exact preview confirmation"
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: "Prepare only — exact preview required"
                        color: "#f3bd55"
                        font.pixelSize: 11
                    }
                }
                Button {
                    objectName: "anchorPileupRunnerButton"
                    Layout.fillWidth: true
                    visible: transmitController.targetCallsign.length > 0
                    enabled: transmitController.targetRfHz > 0
                             && appSettings.radioFrequencyWritable
                    text: "Anchor runner at "
                          + appSettings.cwGuideCenterHz.toFixed(0) + " Hz"
                    onClicked: appSettings.setControlledRxFrequency(
                                   (transmitController.targetRfHz / 1000)
                                     .toFixed(3), 1000)
                    ToolTip.visible: hovered
                    ToolTip.text: enabled
                        ? "Retune RX so this runner falls on the configured CW reference tone; TX and split remain unchanged"
                        : "Requires a checked RF marker and a writable linked radio"
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: transmitController.proposedMessage.length > 0
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: transmitController.proposedReason + ": "
                              + transmitController.proposedMessage
                        color: "#ffd54f"
                    }
                    Button {
                        text: "Prepare"
                        onClicked: transmitController.acceptProposal()
                        ToolTip.visible: hovered
                        ToolTip.text: "Move this suggestion into the exact confirmation preview"
                    }
                }
                Label { text: "Free text"; font.weight: Font.Bold }
                TextArea {
                    id: txFreeText
                    objectName: "txFreeTextArea"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 110
                    enabled: transmitController.qsoConfirmed
                    placeholderText: "Type operator-authored CW text"
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: "Prepare free text"
                        enabled: transmitController.qsoConfirmed
                        onClicked: transmitController.prepareFreeText(txFreeText.text)
                        ToolTip.visible: hovered
                        ToolTip.text: enabled
                            ? "Normalize this operator-authored text and open exact preview confirmation"
                            : "Confirm the selected station first"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: "TX speed  " + transmitController.wordsPerMinute
                              + " WPM  •  " + transmitController.speedSource
                        color: "#91a0b1"
                        ToolTip.visible: txSpeedHelp.hovered
                        ToolTip.text: "Configured in Settings → Radio; RX matching is snapshotted before message preparation"
                        HoverHandler { id: txSpeedHelp }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: previewColumn.implicitHeight + 24
                    visible: transmitController.preparedMessage.length > 0
                    radius: 6
                    color: "#07110e"
                    border.color: transmitController.messageConfirmed
                                  ? "#4dff88" : "#f3bd55"
                    ColumnLayout {
                        id: previewColumn
                        anchors.fill: parent
                        anchors.margins: 12
                        Label { text: "EXACT TX PREVIEW"; color: "#91a0b1"; font.weight: Font.Bold }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WrapAnywhere
                            text: transmitController.preparedMessage
                            color: "#ffffff"
                            font.family: "monospace"
                            font.pixelSize: 18
                        }
                        Label {
                            text: transmitController.previewDurationSeconds.toFixed(2)
                                  + " s at " + transmitController.wordsPerMinute + " WPM"
                            color: "#91a0b1"
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: txPreviewConfirmation
                                objectName: "txPreviewConfirmationField"
                                Layout.fillWidth: true
                                placeholderText: "Retype preview exactly"
                                enabled: !transmitController.messageConfirmed
                            }
                            Button {
                                text: "Confirm preview"
                                enabled: !transmitController.messageConfirmed
                                onClicked: transmitController.confirmPreview(
                                               txPreviewConfirmation.text)
                                ToolTip.visible: hovered
                                ToolTip.text: "Accept only an exact retype of the normalized message"
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "transmitPreparedButton"
                        Layout.fillWidth: true
                        text: transmitController.stationReady
                              ? "TRANSMIT PREPARED MESSAGE"
                              : "TX HARDWARE / RADIO NOT CONFIRMED"
                        enabled: transmitController.messageConfirmed
                                 && transmitController.stationReady
                                 && !transmitController.transmitting
                        onClicked: transmitController.transmitPrepared()
                        ToolTip.visible: hovered
                        ToolTip.text: transmitController.stationReady
                            ? "Transmit the exactly confirmed message through the guarded adapter"
                            : "Requires direct KEY/PTT plus confirmed TX frequency, CW mode, and split state"
                    }
                    Button {
                        objectName: "cancelTransmissionButton"
                        visible: transmitController.transmitting
                        text: "CANCEL TX"
                        highlighted: true
                        onClicked: transmitController.cancelTransmission()
                        ToolTip.visible: hovered
                        ToolTip.text: "Stop the message and synchronously release KEY before PTT"
                    }
                }
            }
        }
    }

    Drawer {
        id: settingsDrawer
        edge: Qt.RightEdge
        width: Math.min(window.width * 0.78, 1080)
        height: window.height
        SettingsPane {
            anchors.fill: parent
            onDone: settingsDrawer.close()
            onSetupRequested: setupWizard.open()
        }
    }

    ProfileChooser {
        id: profileChooser
        anchors.centerIn: Overlay.overlay
    }

    SetupWizard {
        id: setupWizard
        anchors.centerIn: Overlay.overlay
    }

    // Asked before any receive audio can leave the machine, and asked in these
    // words because the operator is agreeing to something larger than the
    // telemetry beside it: the audio carries every signal in the passband,
    // including stations this application never decoded and anything else the
    // receiver happens to be hearing.
    //
    // Permission is for this session only and is not written to the settings.
    // Every other network setting here is remembered, because an operator who
    // set up a diagnostics stream wants it back; this one coming back by
    // itself, into a session nobody has thought about yet, is the wrong
    // default for a disclosure this size.
    Dialog {
        id: audioStreamConsent
        objectName: "audioStreamConsentDialog"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, parent ? parent.width - 48 : 460)
        title: "Allow receive audio to leave this station?"
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: diagnosticsServer.audioStreamingEnabled = true
        ColumnLayout {
            width: parent.width
            spacing: 10
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#edf3f8"
                text: "A connected observer will be able to ask for the receiver's audio. It carries every signal in the passband, not only what this application decoded."
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#c8d4e0"
                text: "It is sent unencrypted, over UDP on the same port number as the diagnostics stream, only to observers that already hold the access token and are on the allowed-peer list, and only to the address their own connection came from. Nothing is sent until one asks."
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#c8d4e0"
                text: "This applies to the current session only. It is off again the next time CW Buddy starts, and the status bar shows AUDIO OUT for as long as anything is being sent."
            }
        }
    }

    // Startup notice for pending updates. It appears once per launch, only
    // after any first-run setup is out of the way, and never steals focus from
    // a decode in progress: the operator is told and can act later.
    Dialog {
        id: updateNotice
        objectName: "updateNoticeDialog"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, parent ? parent.width - 48 : 460)
        title: "Updates available"
        standardButtons: Dialog.Close
        property bool shownThisLaunch: false
        property bool appDownloadStarted: false
        property bool appPending: updateChecker.updateAvailable
        property bool listPending: callsignDatabaseUpdater.updateAvailable
        function considerShowing() {
            if (shownThisLaunch) return
            if (appSettings.profileSelectionRequired) return
            if (!appSettings.setupComplete) return
            if (!appPending && !listPending) return
            shownThisLaunch = true
            open()
        }
        ColumnLayout {
            width: parent.width
            spacing: 10
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#c8d4e0"
                text: "The following updates are ready. Installing them is optional and nothing is downloaded until you choose to."
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: updateNotice.appPending
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#edf3f8"
                    text: "Application " + updateChecker.latestVersion
                          + " (installed " + updateChecker.currentVersion + ")"
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Button {
                        objectName: "updateNoticeDownloadAppButton"
                        visible: updateChecker.downloadActionVisible
                        text: updateChecker.downloading
                              ? "Downloading… "
                                + Math.round(updateChecker.downloadProgress * 100)
                                + "%"
                              : "Download update"
                        enabled: !updateChecker.downloading
                        onClicked: {
                            updateNotice.appDownloadStarted = true
                            updateChecker.downloadUpdate()
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: "Download this platform package and verify its SHA-256 checksum"
                    }
                    Button {
                        objectName: "updateNoticeOpenAppButton"
                        visible: updateChecker.verifiedDownloadActionsVisible
                        text: "Open Installer"
                        onClicked: updateChecker.openDownloadedFile()
                        ToolTip.visible: hovered
                        ToolTip.text: "Open the verified package with the operating-system installer"
                    }
                    Button {
                        objectName: "updateNoticeRevealAppButton"
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
                    Item { Layout.fillWidth: true }
                }
            }
            Label {
                objectName: "updateNoticeAppStatusLabel"
                Layout.fillWidth: true
                visible: updateNotice.appPending
                         && (updateNotice.appDownloadStarted
                             || updateChecker.downloading
                             || updateChecker.downloadVerified)
                wrapMode: Text.WordWrap
                color: updateChecker.downloadVerified ? "#4dff88" : "#91a0b1"
                text: updateChecker.statusMessage
            }
            RowLayout {
                Layout.fillWidth: true
                visible: updateNotice.listPending
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#edf3f8"
                    text: "Offline callsign list"
                }
                Button {
                    objectName: "updateNoticeUpdateListButton"
                    text: callsignDatabaseUpdater.downloading ? "Updating" : "Update"
                    enabled: !callsignDatabaseUpdater.downloading
                    onClicked: callsignDatabaseUpdater.updateDatabase()
                    ToolTip.visible: hovered
                    ToolTip.text: "Download and atomically verify the newer offline callsign list"
                }
            }
        }
    }

    Connections {
        target: updateChecker
        function onStateChanged() { updateNotice.considerShowing() }
    }

    Connections {
        target: callsignDatabaseUpdater
        function onStateChanged() { updateNotice.considerShowing() }
    }

    FileDialog {
        id: wavDialog
        title: "Open receiver WAV recording"
        fileMode: FileDialog.OpenFile
        nameFilters: ["WAV audio (*.wav *.wave)", "All files (*)"]
        onAccepted: replayController.openFile(selectedFile)
    }

    Component.onCompleted: {
        showMaximized()
        if (appSettings.profileSelectionRequired)
            profileChooser.open()
        else if (!appSettings.setupComplete)
            setupWizard.open()
        else
            updateNotice.considerShowing()
    }

    Connections {
        target: appSettings
        function onProfileSelectionRequiredChanged() {
            if (!appSettings.profileSelectionRequired && !appSettings.setupComplete)
                setupWizard.open()
        }
        function onProfileChanged() {
            if (!appSettings.setupComplete)
                setupWizard.open()
        }
    }
}
