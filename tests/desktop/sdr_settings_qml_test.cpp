#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  std::string text{std::istreambuf_iterator<char>{input},
                   std::istreambuf_iterator<char>{}};
  // Carriage returns are removed because this test asserts about the shape of
  // the source, and a line ending is not part of that shape. Git checks these
  // files out with CRLF on Windows, so a needle spanning a line break -- the
  // "\n}\n" that finds the end of a function body below -- matches on every
  // other platform and never matches there. The test then reports that a call
  // is missing from a function whose body it simply failed to find, which is a
  // false failure describing the wrong problem.
  std::erase(text, '\r');
  return text;
}

bool contains(const std::string_view source, const std::string_view text) {
  return source.find(text) != std::string_view::npos;
}

// The body of one free-standing definition: from its signature to the first
// closing brace in the first column. Checks below assert that a call sits
// inside a particular function rather than merely somewhere in the file,
// which is the difference between "the region is written when it is set" and
// "the region is written by apply(), as before".
std::string_view functionBody(const std::string_view source,
                              const std::string_view signature) {
  const std::size_t begin = source.find(signature);
  if (begin == std::string_view::npos)
    return {};
  const std::size_t end = source.find("\n}\n", begin);
  if (end == std::string_view::npos)
    return {};
  return source.substr(begin, end - begin);
}

} // namespace

int main() {
  const std::string main_qml = readFile(CWA_MAIN_QML_PATH);
  const std::string qml = readFile(CWA_SETTINGS_QML_PATH);
  const std::string header = readFile(CWA_APP_SETTINGS_HPP_PATH);
  const std::string implementation = readFile(CWA_APP_SETTINGS_CPP_PATH);
  const std::string controller_header =
      readFile(CWA_REPLAY_CONTROLLER_HPP_PATH);
  const std::string controller_implementation =
      readFile(CWA_REPLAY_CONTROLLER_CPP_PATH);
  if (main_qml.empty() || qml.empty() || header.empty() ||
      implementation.empty() || controller_header.empty() ||
      controller_implementation.empty()) {
    return 1;
  }

  // The user can see and configure a distinct wide-passband source without
  // losing the conventional sound-card path.
  if (!contains(qml, "TabButton { text: \"SDR\" }") ||
      !contains(qml, "objectName: \"receiverInputTypeCombo\"") ||
      !contains(qml, "objectName: \"sdrDeviceCombo\"") ||
      !contains(qml, "objectName: \"sdrCenterFrequencyField\"") ||
      !contains(qml, "objectName: \"sdrSampleRateCombo\"") ||
      !contains(qml, "objectName: \"sdrWidePassbandLabel\"") ||
      !contains(qml, "No external SDR application is required")) {
    return 2;
  }

  // An unavailable backend is explicit and actionable, and every hardware
  // control remains inert while normal audio remains selected.
  if (!contains(qml, "enabled: appSettings.sdrBackendAvailable") ||
      !contains(qml, "SDR is unavailable in this build") ||
      !contains(qml, "sound-card audio remains fully operational") ||
      !contains(implementation, "receiver_input_type_index_ = 0") ||
      !contains(implementation, "This build has no SoapySDR support")) {
    return 3;
  }

  // The persisted UI contract includes only receive configuration. It must
  // not grow a transmit, PTT, or KEY entry point under an SDR name.
  for (const std::string_view forbidden :
       {"setSdrTransmit", "sdrPtt", "sdrKey", "startSdrTransmit"}) {
    if (contains(header, forbidden) || contains(qml, forbidden))
      return 4;
  }
  if (!contains(header, "Q_INVOKABLE void refreshSdrDevices()") ||
      !contains(header, "Q_INVOKABLE void selectSdrDevice(int index)")) {
    return 5;
  }

  // Wide acquisition and bounded decoding are separate operator controls.
  // Standard capability-driven settings remain provider-neutral, and the
  // radio-follow route consumes authoritative readback rather than UI intent.
  if (!contains(qml, "objectName: \"sdrBandwidthCombo\"") ||
      !contains(qml, "objectName: \"sdrAntennaCombo\"") ||
      !contains(qml, "objectName: \"sdrDecoderBandwidthCombo\"") ||
      !contains(qml, "function formatFrequencyKhz(frequencyHz)") ||
      !contains(qml, "function parseFrequencyKhz(value)") ||
      !contains(qml, "SDR center frequency (kHz)") ||
      !contains(qml, "placeholderText: \"7021.43\"") ||
      !contains(qml, "objectName: \"sdrOperatingModeCombo\"") ||
      !contains(qml, "appSettings.sdrOperatingModeNames") ||
      !contains(qml, "appSettings.selectSdrOperatingMode(currentIndex)") ||
      !contains(qml, "two synchronized RX channels") ||
      !contains(qml, "alternative 8 MHz master sample clock") ||
      !contains(main_qml, "objectName: \"sdrDecoderWindowOverlay\"") ||
      !contains(main_qml, "spectrumDisplay.zoomAt(") ||
      !contains(main_qml, "spectrumDisplay.panBy(") ||
      !contains(main_qml, "id: pointerHintLifetime") ||
      !contains(main_qml, "interval: 10000") ||
      !contains(main_qml, "id: pointerHintCooldown") ||
      !contains(main_qml, "interval: 300000") ||
      !contains(qml, "objectName: \"showSpectrumGestureHintsCheck\"") ||
      !contains(header, "showSpectrumGestureHints READ") ||
      !contains(implementation, "display/showSpectrumGestureHints") ||
      !contains(controller_header, "liveSdrDecoderWindowRequested") ||
      !contains(header, "sdrFollowRadioVfo")) {
    return 10;
  }

  // A decode region chosen on the spectrum survives a restart. It used to be
  // written only by the settings dialog's Apply, so a width set by dragging
  // held until the application closed and then snapped back to the last
  // applied one -- the main window has no Apply to press.
  //
  // Both the drag (setSdrDecoderWindow) and the settings combo
  // (setSdrDecoderBandwidthHz) must reach disk through the same one writer,
  // and the write must precede the signals so that a listener correcting the
  // region stores its correction last. The drag reports once, on release, so
  // one selection is one write; asserted here because a gesture that reported
  // continuously would turn this into a write per pointer move.
  const std::string_view persist_body = functionBody(
      implementation, "void AppSettings::persistSdrDecoderWindow()");
  const std::string_view drag_body = functionBody(
      implementation, "void AppSettings::setSdrDecoderWindow(");
  const std::string_view bandwidth_body = functionBody(
      implementation, "void AppSettings::setSdrDecoderBandwidthHz(");
  const std::string_view center_body = functionBody(
      implementation, "void AppSettings::setSdrDecoderCenterFrequencyHz(");
  const std::size_t persist_call = drag_body.find("persistSdrDecoderWindow();");
  const std::size_t drag_signal = drag_body.find("emit sdrSettingsChanged();");
  // The gesture reports from its release handler, between the pointer-move
  // handler above it and the cancel handler below it.
  const std::size_t drag_move = main_qml.find("onPositionChanged: function(");
  const std::size_t drag_release = main_qml.find("onReleased: function(");
  const std::size_t drag_report =
      main_qml.find("appSettings.setSdrDecoderWindow(");
  const std::size_t drag_cancel = main_qml.find("onCanceled: ");
  if (persist_body.empty() || drag_body.empty() || bandwidth_body.empty() ||
      center_body.empty() ||
      !contains(persist_body, "sdr/decoderCenterFrequencyHz") ||
      !contains(persist_body, "sdr/decoderBandwidthHz") ||
      persist_call == std::string_view::npos ||
      drag_signal == std::string_view::npos || persist_call > drag_signal ||
      !contains(bandwidth_body, "persistSdrDecoderWindow();") ||
      !contains(center_body, "persistSdrDecoderWindow();") ||
      drag_move == std::string::npos || drag_release == std::string::npos ||
      drag_report == std::string::npos || drag_cancel == std::string::npos ||
      !(drag_move < drag_release && drag_release < drag_report &&
        drag_report < drag_cancel)) {
    return 16;
  }

  // One stored region, not two. The width must not be shadowed by a
  // session-only copy that a restart would then have to choose between.
  for (const std::string_view forbidden :
       {"sessionDecoderBandwidth", "session_decoder_bandwidth",
        "sdr/sessionDecoder"}) {
    if (contains(header, forbidden) || contains(implementation, forbidden))
      return 17;
  }

  // A region saved by a version that allowed more than the operator can now
  // listen to is narrowed on the way in, so an old profile cannot restore a
  // width the decimator and the 48 kHz monitor path will not carry.
  const std::size_t stored_bandwidth =
      implementation.find("QStringLiteral(\"sdr/decoderBandwidthHz\")), 24'000");
  if (stored_bandwidth == std::string::npos ||
      !contains(std::string_view(implementation)
                    .substr(stored_bandwidth, 200),
                "kMaximumSdrDecoderBandwidthHz") ||
      !contains(header,
                "kMaximumSdrDecoderBandwidthHz = 24'000") ||
      !contains(header, "kMinimumSdrDecoderBandwidthHz = 2'000")) {
    return 18;
  }

  // Alternative RSPduo operating configurations sharing one serial are
  // selected separately from the physical receiver. Stable physical/mode
  // keys supplement the legacy exact variant ID for profile migration.
  if (!contains(header, "sdrOperatingModeNames READ") ||
      !contains(header, "sdrOperatingModeIndex READ") ||
      !contains(header, "selectSdrOperatingMode(int index)") ||
      !contains(implementation, "groupSdrDevices(report.devices)") ||
      !contains(implementation, "sdr/physicalDeviceId") ||
      !contains(implementation, "sdr/deviceMode") ||
      !contains(implementation, "previous_variant_id.startsWith")) {
    return 12;
  }

  // Frequently used receive controls live beside the spectrum. The compact
  // panel remains RX-only, exposes only driver capabilities, and precedes the
  // independent CAT radio/TX panel.
  const std::size_t sdr_panel = main_qml.find("id: sdrRadioDisplay");
  const std::size_t cat_panel = main_qml.find("id: vfoDisplay");
  if (sdr_panel == std::string::npos || cat_panel == std::string::npos ||
      sdr_panel >= cat_panel ||
      !contains(main_qml, "objectName: \"sdrRxFrequencyLabel\"") ||
      !contains(main_qml, "objectName: \"sdrFrequencyDownButton\"") ||
      !contains(main_qml, "objectName: \"sdrFrequencyUpButton\"") ||
      !contains(main_qml, "objectName: \"sdrOperatingModeCombo\"") ||
      !contains(main_qml, "objectName: \"sdrControlAntennaCombo\"") ||
      !contains(main_qml, "objectName: \"sdrControlSampleRateCombo\"") ||
      !contains(main_qml, "objectName: \"sdrControlBandwidthCombo\"") ||
      !contains(main_qml, "objectName: \"sdrTuningStepCombo\"") ||
      !contains(main_qml, "objectName: \"sdrCatSyncButton\"") ||
      contains(qml, "objectName: \"sdrFollowRadioVfoCheck\"") ||
      !contains(main_qml, "appSettings.requestSdrRxFrequencyHz(") ||
      !contains(main_qml, "appSettings.stepSdrRxFrequency(-1)") ||
      !contains(main_qml, "appSettings.stepSdrRxFrequency(1)")) {
    return 12;
  }

  // The same edge arrows must tune whichever receiver currently owns the
  // displayed passband; direct SDR operation must not depend on CAT support.
  if (!contains(main_qml, "replayController.sourceMode === 2") ||
      !contains(main_qml, "? appSettings.stepSdrRxFrequency(-1)") ||
      !contains(main_qml, "? appSettings.stepSdrRxFrequency(1)")) {
    return 13;
  }

  // The SDR faceplate is styled like the CAT Radio Control faceplate: Radio
  // Sync is a square hand-drawn tile whose colour carries its state, not a
  // Material pill whose caption had to bake in an ellipsis to fit. Nothing on
  // the faceplate may be sized below its own implicit width, which is what
  // truncated the combo boxes, and the DEC RATE badge is gone because it had
  // no value binding and could never populate; its explanation now belongs to
  // the effective-IQ-rate control that actually determines decimation.
  if (!contains(main_qml, "id: sdrSyncTile") ||
      !contains(main_qml,
                "Layout.preferredWidth: sdrRadioDisplay.controlButtonSize") ||
      !contains(main_qml, "text: \"SYNC\"") ||
      contains(main_qml, "SYNC OFF") ||
      contains(main_qml, "objectName: \"sdrDecimationBadge\"") ||
      contains(main_qml, "DEC RATE") ||
      !contains(main_qml,
                "SoapySDR exposes no independent decimation control") ||
      contains(main_qml, "Layout.preferredWidth: 105") ||
      contains(main_qml, "Layout.preferredWidth: 95")) {
    return 14;
  }

  // Zoom and the decode window are independent controls, so the operator must
  // still be told where decoding is happening after zooming away from it.
  // Decoded streams report their frequency the way the VFO readout does
  // instead of as raw hertz.
  if (!contains(main_qml, "objectName: \"sdrDecoderWindowEdgeIndicator\"") ||
      !contains(main_qml, "function formatStreamFrequency(channel)") ||
      contains(main_qml, "modelData.frequencyLabel")) {
    return 15;
  }

  // Radio control remains visible with direct SDR reception so an independent
  // CAT radio can own the TX endpoint in a full-duplex profile.
  const std::size_t vfo_start = main_qml.find("id: vfoDisplay");
  const std::size_t vfo_end = main_qml.find("id: onAirIndicator", vfo_start);
  if (vfo_start == std::string::npos || vfo_end == std::string::npos ||
      contains(
          std::string_view(main_qml).substr(vfo_start, vfo_end - vfo_start),
          "sourceMode === 0") ||
      !contains(
          std::string_view(main_qml).substr(vfo_start, vfo_end - vfo_start),
          "visible: replayController.radioFrequencyAvailable")) {
    return 11;
  }
  // Opening the SDR page requests discovery once, after the page can render.
  // Application/profile construction must still never probe hardware.
  if (!contains(qml, "property bool sdrDiscoveryRequested: false") ||
      !contains(qml, "function requestInitialSdrDiscovery()") ||
      !contains(qml, "if (currentIndex === 1)") ||
      !contains(
          qml,
          "Qt.callLater(function() { appSettings.refreshSdrDevices() })") ||
      contains(implementation, "  refreshSdrDevices();") ||
      !contains(implementation, "canonicalFilePath()") ||
      !contains(implementation, "Qt::CaseInsensitive") ||
      !contains(header, "Open the SDR settings page or ") ||
      !contains(header,
                "press Refresh devices; live audio remains available.")) {
    return 9;
  }

  // The configured device reaches an explicit live receiver mode and worker;
  // it is not a settings-only mock. Source selection remains operator-driven.
  if (!contains(main_qml,
                "model: [\"Live audio\", \"WAV replay\", \"Live SDR\"]") ||
      !contains(main_qml, "objectName: \"startLiveSdrButton\"") ||
      !contains(main_qml, "replayController.startLiveSdr()") ||
      !contains(controller_header, "Q_INVOKABLE void startLiveSdr()") ||
      !contains(controller_implementation, "new SdrCaptureWorker(pipe)") ||
      !contains(controller_implementation, "emit sdrStartRequested(")) {
    return 6;
  }

  // Wide IQ is never presented as full-receiver audio, and the label hover is
  // deliberately modest so dense markers remain readable.
  if (!contains(main_qml, "replayController.sourceMode !== 2") ||
      !contains(main_qml,
                "font.pixelSize: channelMarker.pointerHovered ? 22 : 18")) {
    return 7;
  }

  // LiveAudioPipe is SPSC. The mutually exclusive audio and SDR capture
  // workers must share one serialized producer thread during source changes.
  if (!contains(controller_implementation,
                "sdr_worker->moveToThread(&audio_capture_thread_)") ||
      contains(controller_header, "QThread sdr_capture_thread_") ||
      !contains(controller_implementation,
                "source_mode_ == 2 && monitor_mode_ == 1")) {
    return 8;
  }

  // The Ctrl+Right region drag is measured in PIXELS. Whether a gesture was a
  // click or a drag is a fact about the pointer, and the two thresholds this
  // replaces were in hertz -- so the same gesture meant different things at
  // different spectrum zooms. At full span on a wide capture 250 Hz is a
  // fraction of a pixel and any slip resized the region; zoomed in far enough
  // to see CW, a deliberate drag could measure under 1000 Hz, be read as a
  // click, and put the region at the release point at the floor width while
  // the rectangle the operator had just drawn was discarded.
  //
  // One threshold, and the centre follows the same decision as the width, so
  // the box that was drawn and the region that results are the same thing.
  // functionBody() finds a brace in the first column, which QML has only at
  // the end of the file, so the release handler is bounded by the handler
  // declared immediately after it instead.
  const std::size_t release_begin = main_qml.find("onReleased: function(");
  const std::size_t release_end = main_qml.find("onCanceled:", release_begin);
  const std::string_view release_body =
      release_begin == std::string::npos || release_end == std::string::npos
          ? std::string_view{}
          : std::string_view(main_qml).substr(release_begin,
                                              release_end - release_begin);
  if (release_body.empty() ||
      !contains(main_qml,
                "readonly property real decoderSelectionThresholdPx") ||
      !contains(release_body, ">= decoderSelectionThresholdPx") ||
      !contains(release_body, "dragged ? (firstHz + lastHz) / 2 : lastHz") ||
      // The old hertz thresholds, in either of their roles.
      contains(main_qml, "draggedHz >= 250") ||
      contains(main_qml, "draggedHz >= 1000")) {
    return 19;
  }
  // The rectangle on screen and the region that is applied are one
  // calculation, and the operator can read the width before releasing --
  // without which both clamps correct the gesture silently and a drag that
  // was clamped is indistinguishable from one that mis-measured.
  const std::size_t selection_fn =
      main_qml.find("function selectionBandwidthHz()");
  const std::size_t selection_readout =
      main_qml.find("manualSliceHitArea.selectionBandwidthHz()");
  if (selection_fn == std::string::npos ||
      selection_readout == std::string::npos ||
      !contains(release_body, "selectionBandwidthHz()") ||
      !contains(main_qml, "objectName: \"sdrDecoderDragReadout\"")) {
    return 19;
  }

  // The three Listen lamps must show the controller's monitor mode and
  // nothing else. A checkable button toggles its own `checked` before the
  // handler runs, and setMonitorMode() returns without emitting when it is
  // handed the mode already in force -- so pressing REGION while region
  // listening was on turned the lamp off and left the audio playing, and
  // pressing it again turned the lamp back on without changing anything. A
  // button that visibly toggles while nothing it controls moves is
  // indistinguishable from a dead one, which is how this was reported.
  const std::size_t listen_group = main_qml.find("objectName: \"monitorOffButton\"");
  const std::size_t listen_group_end =
      main_qml.find("objectName: \"monitorLevelSlider\"", listen_group);
  if (listen_group == std::string::npos ||
      listen_group_end == std::string::npos ||
      contains(std::string_view(main_qml).substr(
                   listen_group, listen_group_end - listen_group),
               "checkable") ||
      !contains(main_qml, "checked: replayController.monitorMode === 0") ||
      !contains(main_qml, "checked: replayController.monitorMode === 1") ||
      !contains(main_qml, "checked: replayController.monitorMode === 3")) {
    return 20;
  }
  // Every reason a monitor request failed already existed as a property and
  // was displayed nowhere, so a refused REGION and a monitor output that
  // cannot carry the region's 48 kHz mono float both looked exactly like a
  // button that does nothing.
  if (!contains(controller_header, "monitorStatus READ monitorStatus") ||
      !contains(main_qml, "objectName: \"monitorStatusNotice\"") ||
      !contains(main_qml, "text: replayController.monitorStatus") ||
      !contains(controller_implementation,
                "Region listening needs direct SDR reception.")) {
    return 20;
  }

  // Cluster and RBN callsigns belong inside the spot bar. They were a sibling
  // Repeater positioned from the overlay's own coordinates against a bar whose
  // depth was a hand-counted 26 px, while the plate was built from the label's
  // own line height plus 8 -- 26 or more wherever a 13 px DemiBold line box
  // runs to 18 px or taller. Nothing clipped them to the bar, so the callsign
  // was drawn below the band that exists to contain it, on the waterfall.
  //
  // Containment is structural now: the plates are children of the bar and the
  // bar clips, so no arithmetic can put one outside it. The bar is also sized
  // from the same single measurement of the font the plate is sized from, so
  // nothing has to be cut off for that to hold.
  const std::size_t spot_bar = main_qml.find("objectName: \"dxSpotBar\"");
  const std::size_t spot_repeater =
      main_qml.find("model: dxSpotOverlay.placedSpots", spot_bar);
  const std::size_t spot_plate =
      main_qml.find("objectName: \"dxSpotLabelPlate\"", spot_bar);
  const std::size_t offset_ruler = main_qml.find("id: audioOffsetRuler");
  if (spot_bar == std::string::npos || spot_repeater == std::string::npos ||
      spot_plate == std::string::npos || offset_ruler == std::string::npos ||
      !(spot_bar < spot_repeater && spot_repeater < spot_plate &&
        spot_plate < offset_ruler) ||
      !contains(std::string_view(main_qml).substr(spot_bar,
                                                  spot_repeater - spot_bar),
                "clip: true") ||
      !contains(std::string_view(main_qml).substr(spot_bar,
                                                  spot_repeater - spot_bar),
                "height: dxSpotOverlay.spotBarHeight") ||
      !contains(main_qml, "FontMetrics {") ||
      !contains(main_qml, "id: spotLabelMetrics") ||
      !contains(main_qml, "spotPlateOffsetY + spotPlateHeight + 2") ||
      !contains(main_qml, "y: dxSpotOverlay.spotPlateOffsetY") ||
      !contains(main_qml, "height: dxSpotOverlay.spotPlateHeight")) {
    return 21;
  }
  // The label layout reserves the room a callsign really needs. The estimate
  // it replaces charged 7.4 px per character -- short for DemiBold capitals,
  // so two neighbours could both be told there was room -- plus 11 px apiece
  // for two evidence marks that have not sat beside the callsign since they
  // became the stripe underneath it.
  if (contains(main_qml, "spot.callsign.length * 7.4") ||
      contains(main_qml, "function estimatedLabelWidth(") ||
      !contains(main_qml, "spotLabelMetrics.advanceWidth(") ||
      !contains(main_qml, "var labelWidth = measuredLabelWidth(spot)")) {
    return 21;
  }
  // The audio-offset ruler clears the bar by measurement rather than by the
  // count that put its ticks inside a callsign plate.
  if (!contains(main_qml, "dxSpotOverlay.spotBarBottomY + 4") ||
      contains(main_qml, "dxSpotOverlay.waterfallTopY + 26")) {
    return 21;
  }

  // An audio input has to be findable again after its identifier changes.
  //
  // A station rebooted and reception refused to start, because the only thing
  // stored about his chosen input was the base64 of QAudioDevice::id() -- an
  // opaque operating-system handle that a restart, a driver reload or a
  // different USB port can reissue while the hardware sits untouched. The
  // operating system's own description of the device survives all three, so it
  // is stored raw, undecorated by the ordinal and default markers the list
  // shows, and it is what the input is found by when the identifier is gone.
  // Inside apply(), not merely somewhere in the file: the recovery path writes
  // the same key, and an assertion that either one satisfies would let the
  // ordinary Apply stop storing the name without anything noticing.
  if (!contains(functionBody(implementation, "bool AppSettings::apply()"),
                "storageKey(QStringLiteral(\"audio/inputDeviceName\"))") ||
      !contains(header, "const QString& audioInputDeviceName() const noexcept") ||
      !contains(functionBody(implementation, "void AppSettings::selectAudioInput("),
                "audio_input_device_name_ =")) {
    return 22;
  }
  // A recovered identifier is written to storage there and then, not left for
  // the next Apply: a station that closes the application before applying
  // anything else would otherwise meet the same unresolvable selection at the
  // next restart, which is the failure being repaired.
  const std::string_view adopt_body =
      functionBody(implementation, "void AppSettings::adoptRecoveredAudioInput(");
  if (adopt_body.empty() ||
      !contains(adopt_body, "storageKey(QStringLiteral(\"audio/inputId\"))") ||
      !contains(adopt_body, "refreshAudioInputs()") ||
      !contains(controller_header, "void audioInputRecovered(const QString& adopted_id)")) {
    return 22;
  }
  // The name travels with the identifier all the way to the worker. Without
  // it, resolution has nothing to fall back on and the recovery cannot happen
  // at all.
  if (!contains(controller_header, "void liveStartRequested(const QString& encoded_device_id,") ||
      !contains(functionBody(controller_implementation,
                             "void ReplayController::beginLiveAudioCapture("),
                "emit liveStartRequested(audio_input_id_, audio_input_device_name_)")) {
    return 22;
  }

  // Identically-named inputs are numbered, in the list and in the wizard, and
  // the numbering is explained exactly when there is something to explain. Two
  // rows reading the same words are unchoosable: a refusal to guess between
  // them is only actionable if the operator can point at one of them.
  const std::string setup_qml =
      readFile(std::filesystem::path(CWA_SETTINGS_QML_PATH).parent_path() /
               "SetupWizard.qml");
  if (setup_qml.empty() ||
      !contains(functionBody(implementation, "void AppSettings::refreshAudioInputs("),
                "QStringLiteral(\"%1 #%2\")") ||
      !contains(header, "bool audioInputNamesAmbiguous() const noexcept") ||
      !contains(qml, "visible: appSettings.audioInputNamesAmbiguous") ||
      !contains(setup_qml, "visible: appSettings.audioInputNamesAmbiguous")) {
    return 23;
  }

  return 0;
}
