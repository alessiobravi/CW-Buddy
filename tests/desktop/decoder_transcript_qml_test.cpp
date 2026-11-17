#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

bool contains(const std::string& value, const std::string& expected) {
  return value.find(expected) != std::string::npos;
}

std::size_t countOccurrences(const std::string& value,
                             const std::string& expected) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = value.find(expected, offset)) != std::string::npos) {
    ++count;
    offset += expected.size();
  }
  return count;
}

void normalizeLineEndings(std::string& value) {
  value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
}

}  // namespace

int main() {
  std::ifstream source(CWA_MAIN_QML_PATH, std::ios::binary);
  if (!source) return 1;
  std::string qml{std::istreambuf_iterator<char>{source},
                  std::istreambuf_iterator<char>{}};
  // Git may materialize text files with CRLF on Windows. The QML contract is
  // line-ending independent, so normalize before matching its bounded block.
  normalizeLineEndings(qml);
  const std::size_t start = qml.find("id: transcriptScroll");
  // Delimited by the metrics label's stable object name rather than by the
  // text expression inside it, which changes whenever the line is reworded.
  const std::size_t end =
      qml.find("objectName: \"decoderMetricsLabel\"", start);
  const std::size_t session_card_start = qml.find("id: sessionCard");
  const std::size_t local_panel_start =
      qml.find("id: localModelTranscriptPanel", start);
  if (start == std::string::npos || end == std::string::npos ||
      session_card_start == std::string::npos ||
      local_panel_start == std::string::npos || session_card_start >= start ||
      local_panel_start >= end) {
    return 2;
  }
  const std::string transcript = qml.substr(start, end - start);
  const std::string session_header =
      qml.substr(session_card_start, start - session_card_start);
  const std::string local_panel =
      qml.substr(local_panel_start, end - local_panel_start);
  const std::size_t callsign_start =
      qml.find("property string callsignEvidenceText:");
  const std::size_t callsign_end =
      qml.find("property string ownCallEvidenceText:", callsign_start);

  // An append must not rebuild the text document. Reassigning the whole
  // string discards the layout and resets the viewport, so the card showed a
  // stale offset for one frame on every decoded character, which reads as a
  // constant shudder. The new suffix is inserted instead, and the tail is
  // pinned in the same frame the content grows rather than a frame later.
  if (!contains(transcript, "insert(length,") ||
      !contains(transcript, "nextText.substring(length)") ||
      !contains(transcript, "function pinToTail()") ||
      !contains(transcript, "function onContentHeightChanged()") ||
      !contains(transcript, "transcriptScroll.pinToTail()")) {
    return 4;
  }

  // Appends may move only the viewport. Moving the TextEdit cursor caused
  // Qt to repeatedly ensure it was visible, disturbing selection and making
  // every live update jump through intermediate layouts.
  if (!contains(transcript, "textFormat: TextEdit.PlainText") ||
      contains(transcript, "TextEdit.RichText") ||
      contains(transcript, "cursorPosition") ||
      !contains(transcript, "contentItem.contentY") ||
      !contains(transcript, "decodedTextArea.selectionStart") ||
      !contains(transcript, "decodedTextArea.selectionEnd") ||
      !contains(transcript, "function applyDecodedText(nextText)") ||
      contains(qml, "Acoustic correction:") ||
      !contains(qml, "modelData.contextualText.length > 0") ||
      !contains(qml, "? modelData.contextualText") ||
      !contains(qml, "modelData.refinedText.length > 0") ||
      !contains(transcript, "onDisplayedDecodedTextChanged()") ||
      !contains(transcript, "select(Math.min(oldSelectionStart") ||
      !contains(transcript, "if (!followTail)") ||
      !contains(transcript, "function onMovementStarted()") ||
      !contains(transcript, "followTail = false")) {
    return 3;
  }
  if (callsign_start == std::string::npos ||
      callsign_end == std::string::npos ||
      contains(qml.substr(callsign_start, callsign_end - callsign_start),
               "localModel") ||
      contains(qml.substr(callsign_start, callsign_end - callsign_start),
               "callsignSuggestion") ||
      !contains(qml, "objectName: \"localModelTranscriptPanel\"") ||
      !contains(qml, "objectName: \"localModelStateLabel\"") ||
      !contains(qml, "objectName: \"localModelStatusLabel\"") ||
      !contains(qml, "objectName: \"localModelTranscriptText\"") ||
      !contains(qml, "id: localModelTranscriptScroll") ||
      !contains(qml, "property string localModelStableText:") ||
      !contains(qml, "property string ownCallEvidenceText:") ||
      !contains(qml, "callsignEvidenceText + \" \" + localModelStableText") ||
      !contains(qml, "property string localModelCallsign:") ||
      !contains(qml, "text: \"MODEL\"") ||
      !contains(qml, "objectName: \"advisoryCallsignSuggestionBadge\"") ||
      !contains(qml, "property string advisoryCallsignSuggestion:") ||
      !contains(qml, "property string callsignSuggestionSource:") ||
      !contains(qml, "\"≈ \" + sessionCard.advisoryCallsignSuggestion") ||
      !contains(qml, "modelData.qsoParticipants.length >= 2") ||
      !contains(qml, "modelData.qsoParticipants[0]") ||
      !contains(qml, "modelData.qsoParticipants[1]") ||
      !contains(qml, "objectName: \"currentSenderLabel\"") ||
      !contains(qml, "modelData.currentSenderCallsign") ||
      !contains(qml, "modelData.currentSenderWpm") ||
      !contains(qml, "? \"TX \" + modelData.callsign : \"TX\"") ||
      !contains(qml, "Advisory acoustic consensus") ||
      !contains(qml, "sessionCard.localModelCallsign") ||
      !contains(qml, "function applyStableText(nextText)") ||
      !contains(qml, "function onLocalModelStateChanged()") ||
      !contains(qml, "localModelTranscriptScroll.followAppendedText()") ||
      !contains(qml, "localModelTranscriptScroll.availableWidth") ||
      !contains(qml, "nextText.indexOf(text)") ||
      contains(qml, "rawDecodedText + \" \" + modelData.localModelText")) {
    return 11;
  }

  // The metrics line is read while operating. It was 10px low-contrast grey on
  // a single elided row, so values were cut off, and it reported instantaneous
  // confidence which falls to zero between characters and showed 0% while text
  // was arriving.
  const std::size_t metrics = qml.find("objectName: \"decoderMetricsLabel\"");
  if (metrics == std::string::npos) return 15;
  const std::string metrics_block = qml.substr(metrics, 1800);
  if (!contains(metrics_block, "meanCharacterConfidence") ||
      contains(metrics_block, "elide: Text.ElideRight") ||
      contains(metrics_block, "font.pixelSize: 10") ||
      !contains(metrics_block, "wrapMode: Text.WordWrap")) {
    return 16;
  }

  // The optional local model must not report an error for a model that was
  // never configured, and its panel must not occupy every card when the
  // feature is not in use. Enabling it without selecting files previously
  // attempted a load, and an empty path failed the metadata check as though
  // the file were the wrong kind or too large.
  if (!contains(qml, "sessionCard.localModelState !== \"disabled\"") ||
      !contains(qml, "sessionCard.localModelState !== \"unconfigured\"")) {
    return 17;
  }

  // A confirmed callsign must say whether the offline list corroborates it.
  // Both are legitimate outcomes -- an unlisted station is ordinary -- so the
  // badge reports corroboration and never implies the decode is wrong, and it
  // is hidden entirely when no list is loaded rather than claiming "not
  // listed" against an empty directory.
  if (!contains(qml, "objectName: \"callsignDatabaseBadge\"") ||
      !contains(qml, "modelData.callsignInDatabase") ||
      !contains(qml, "modelData.callsignDatabaseLoaded") ||
      !contains(qml, "\\u2713 LISTED") || !contains(qml, "\"DECODED\"")) {
    return 12;
  }

  // A station calling the operator must be visible on the spectrum, before
  // any card is opened: an alert that only appears inside an opened session
  // cannot draw attention to a call the operator has not found yet.
  if (!contains(qml, "modelData.callingOwnStation") ||
      !contains(qml, "\\u25CF CALLING YOU") ||
      !contains(qml, "loops: Animation.Infinite") ||
      !contains(qml, "objectName: \"channelLabelBackground\"")) {
    return 13;
  }

  // Keep wrapping width independent of scrollbar visibility, with no
  // horizontal scrollbar and a permanently reserved vertical gutter.
  if (!contains(transcript, "width: transcriptScroll.availableWidth") ||
      !contains(qml, "objectName: \"decoderTranscriptFrame\"") ||
      !contains(transcript, "anchors.margins: 1") ||
      !contains(transcript, "leftPadding: 9") ||
      !contains(transcript, "rightPadding: 9") ||
      !contains(transcript, "topPadding: 9") ||
      !contains(transcript, "bottomPadding: 9") ||
      !contains(transcript, "background: null") ||
      !contains(transcript,
                "wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere") ||
      !contains(transcript, "height: Math.max(") ||
      !contains(transcript, "transcriptScroll.availableHeight") ||
      !contains(transcript, "implicitHeight)") ||
      !contains(transcript, "ScrollBar.horizontal: ScrollBar") ||
      !contains(transcript, "policy: ScrollBar.AlwaysOff") ||
      !contains(transcript, "ScrollBar.vertical: ScrollBar") ||
      !contains(transcript, "policy: ScrollBar.AsNeeded") ||
      !contains(session_header,
                "height: Math.ceil(sessionCardLayout.implicitHeight + 20)") ||
      !contains(session_header, "id: sessionCardLayout") ||
      !contains(session_header, "clip: true") ||
      !contains(session_header, "elide: Text.ElideRight") ||
      !contains(local_panel, "sessionCard.localModelHasText ? 88 : 50") ||
      !contains(local_panel, "clip: true")) {
    return 4;
  }

  const std::size_t action_row_start =
      qml.find("objectName: \"decoderSessionActionRow\"");
  const std::size_t action_row_end = qml.find("id: txDrawer", action_row_start);
  if (action_row_start == std::string::npos ||
      action_row_end == std::string::npos) {
    return 18;
  }
  const std::string action_row =
      qml.substr(action_row_start, action_row_end - action_row_start);
  const std::size_t tx_action =
      action_row.find("objectName: \"decoderSessionTxButton\"");
  const std::size_t monitor_action =
      action_row.find("objectName: \"decoderSessionMonitorButton\"");
  const std::size_t action_spacer =
      action_row.find("Item { Layout.fillWidth: true }");
  if (tx_action == std::string::npos || monitor_action == std::string::npos ||
      action_spacer == std::string::npos || tx_action >= action_spacer ||
      action_spacer >= monitor_action ||
      countOccurrences(action_row, "Layout.preferredWidth: 124") < 2 ||
      countOccurrences(action_row, "Layout.preferredHeight: 38") < 2 ||
      !contains(action_row, "text: sessionCard.streamMonitored") ||
      !contains(action_row, "Monitor\"") ||
      !contains(action_row, "Accessible.name: sessionCard.streamMonitored")) {
    return 19;
  }

  const std::size_t manual_start = qml.find("id: manualSliceHitArea");
  const std::size_t tune_down_start = qml.find("id: tuneRxDownButton");
  const std::size_t tune_up_start = qml.find("id: tuneRxUpButton");
  const std::size_t marker_start = qml.find("id: channelMarker");
  const std::size_t marker_end = qml.find("ToolTip.text:", marker_start);
  if (manual_start == std::string::npos ||
      tune_down_start == std::string::npos ||
      tune_up_start == std::string::npos || marker_start == std::string::npos ||
      marker_end == std::string::npos || manual_start >= tune_down_start ||
      tune_down_start >= tune_up_start || tune_up_start >= marker_start) {
    return 5;
  }
  const std::string manual =
      qml.substr(manual_start, marker_start - manual_start);
  const std::string marker =
      qml.substr(marker_start, marker_end - marker_start);
  const std::string tune_down =
      qml.substr(tune_down_start, tune_up_start - tune_down_start);
  const std::string tune_up =
      qml.substr(tune_up_start, marker_start - tune_up_start);
  if (!contains(manual, "objectName: \"manualSliceHitArea\"") ||
      !contains(manual, "z: 6") ||
      !contains(manual, "enabled: replayController.activeSource") ||
      !contains(manual, "acceptedButtons: Qt.LeftButton | Qt.RightButton") ||
      !contains(manual, "function streamIdAtX(positionX)") ||
      !contains(manual, "replayController.openDecoderSession(streamId)") ||
      !contains(manual, "function hasExactModifiers(mouse, required)") ||
      !contains(manual,
                "hasExactModifiers(mouse,\n                            "
                "                         "
                "Qt.ControlModifier)") ||
      // Alt, not Shift. The region drag moved to Ctrl+Right to sit with the
      // rest of the receive-decoder family, and Alt+Left took over opening a
      // decode where the operator points.
      !contains(manual, "hasExactModifiers(mouse, Qt.AltModifier)") ||
      !contains(manual,
                "hasExactModifiers(mouse,\n                            "
                "                         "
                "Qt.NoModifier)") ||
      !contains(manual, "decoderSelectionActive") ||
      !contains(manual, "appSettings.setSdrDecoderWindow(selectedCenterHz,") ||
      !contains(manual, "replayController.openManualDecoderSession(") ||
      !contains(manual, "objectName: \"sdrDecoderDragSelection\"") ||
      !contains(manual, "appSettings.radioPointedTxFrequencyAvailable") ||
      !contains(manual, "replayController.displayFrequencyToRfHz(") ||
      !contains(manual, "appSettings.setControlledTxFrequencyHz(txRfHz)") ||
      !contains(manual, "mouse.button === Qt.LeftButton") ||
      !contains(manual, "onClicked: function(mouse)") ||
      contains(manual, "onDoubleClicked:") ||
      !contains(manual, "spectrumDisplay.lowerFrequencyHz") ||
      contains(manual, "appSettings.cwGuideCenterHz = frequencyHz") ||
      // Pointing the received spectrum and opening a manual decode were one
      // gesture, so an operator asking for either always got both. They are
      // separate intentions on separate gestures now, and Ctrl+Right returns
      // from the click handler because press and release own the region drag
      // -- acting here too would give one gesture two outcomes again.
      !contains(manual,
                "if (hasExactModifiers(mouse, Qt.ControlModifier))\n"
                "                            return") ||
      !contains(manual, "replayController.openManualDecoderSession(") ||
      !contains(manual, "objectName: \"spectrumPointerHelp\"") ||
      !contains(manual, "appSettings.showSpectrumGestureHints") ||
      !contains(manual, "id: pointerHintLifetime") ||
      !contains(manual, "interval: 10000") ||
      !contains(manual, "id: pointerHintCooldown") ||
      !contains(manual, "interval: 300000") ||
      !contains(manual, "LEFT: open") ||
      !contains(manual, "RIGHT: point RX") ||
      !contains(manual, "CTRL+RIGHT: decode region") ||
      !contains(manual, "ALT+LEFT: manual decode") ||
      // The wheel button was handled but never accepted, so panning could not
      // work; clicking it retunes a direct IQ receiver to centre on the
      // pointer.
      !contains(manual, "Qt.MiddleButton") ||
      !contains(manual, "appSettings.sdrCenterFrequencyHz = centreHz") ||
      // "decode region", not "decoder span": the gesture defines where to
      // listen and nothing else. It also opened a manual decode at the centre
      // of the selection, so choosing a region silently created a stream the
      // operator had not asked for, at a frequency that is merely the middle
      // of a drag.
      !contains(manual, "WHEEL BTN: centre") ||
      // The release handler sets the window and stops. Deciding what to decode
      // is a separate gesture on the signal itself.
      contains(manual,
               "appSettings.setSdrDecoderWindow(selectedCenterHz,\n"
               "                                                        "
               "bandwidthHz)\n                        replayController"
               ".openManualDecoderSession(") ||
      !contains(manual, "CTRL+LEFT: TX") ||
      !contains(tune_down, "objectName: \"tuneRxDownButton\"") ||
      !contains(tune_down, "appSettings.radioFrequencyWritable") ||
      !contains(tune_down, "z: 8") ||
      !contains(tune_down, "appSettings.stepControlledRxFrequency(-1)") ||
      !contains(tune_down, "Tune SDR RX down") ||
      !contains(tune_down, "Tune RX down") ||
      !contains(tune_down, "appSettings.stepSdrRxFrequency(-1)") ||
      !contains(tune_up, "objectName: \"tuneRxUpButton\"") ||
      !contains(tune_up, "appSettings.radioFrequencyWritable") ||
      !contains(tune_up, "z: 8") ||
      !contains(tune_up, "appSettings.stepControlledRxFrequency(1)") ||
      !contains(tune_up, "Tune SDR RX up") ||
      !contains(tune_up, "Tune RX up") ||
      !contains(tune_up, "appSettings.stepSdrRxFrequency(1)") ||
      !contains(marker, "z: 5") || contains(marker, "id: channelHitArea") ||
      !contains(marker, "manualSliceHitArea.hoveredStreamId")) {
    return 6;
  }
  const std::size_t toolbar_start = qml.find("id: receiverToolbar");
  const std::size_t spectrum_start = qml.find("id: spectrumPanel");
  const std::size_t display_start =
      qml.find("id: spectrumDisplay", spectrum_start);
  if (toolbar_start == std::string::npos ||
      spectrum_start == std::string::npos ||
      display_start == std::string::npos || toolbar_start >= spectrum_start ||
      spectrum_start >= display_start) {
    return 7;
  }
  const std::string toolbar =
      qml.substr(toolbar_start, spectrum_start - toolbar_start);
  const std::string spectrum_panel =
      qml.substr(spectrum_start, display_start - spectrum_start);
  const std::size_t vfo_editor_start = qml.find("id: vfoRxEditor");
  const std::size_t on_air_start = qml.find("objectName: \"onAirIndicator\"");
  const std::size_t tune_start =
      qml.find("objectName: \"radioTuneButton\"", on_air_start);
  const std::size_t rx_mode_start =
      qml.find("objectName: \"vfoRxModeBadge\"", vfo_editor_start);
  const std::size_t tx_editor_start =
      qml.find("id: vfoTxEditor", rx_mode_start);
  const std::size_t tx_mode_start =
      qml.find("objectName: \"vfoTxModeBadge\"", tx_editor_start);
  const std::size_t vfo_badge_start =
      qml.find("objectName: \"vfoSplitBadge\"", tx_mode_start);
  const std::size_t sync_start =
      qml.find("objectName: \"vfoFrequencySyncButton\"", vfo_badge_start);
  const std::size_t radio_heading_start = qml.find("text: \"Radio Control\"");
  const std::size_t decoder_heading_start =
      qml.find("text: \"CW Decoder\"", radio_heading_start);
  const std::size_t diagnostics_start =
      qml.find("objectName: \"diagnosticsToggle\"", decoder_heading_start);
  if (vfo_editor_start == std::string::npos ||
      on_air_start == std::string::npos ||
      vfo_badge_start == std::string::npos ||
      rx_mode_start == std::string::npos || sync_start == std::string::npos ||
      tx_editor_start == std::string::npos ||
      tx_mode_start == std::string::npos || tune_start == std::string::npos ||
      radio_heading_start == std::string::npos ||
      decoder_heading_start == std::string::npos ||
      diagnostics_start == std::string::npos || on_air_start >= tune_start ||
      tune_start >= vfo_editor_start || rx_mode_start >= tx_editor_start ||
      tx_editor_start >= tx_mode_start || tx_mode_start >= vfo_badge_start ||
      vfo_badge_start >= sync_start ||
      radio_heading_start >= vfo_editor_start ||
      sync_start >= decoder_heading_start ||
      decoder_heading_start >= diagnostics_start) {
    return 8;
  }
  const std::string vfo_editor =
      qml.substr(vfo_editor_start, tx_editor_start - vfo_editor_start);
  if (!contains(toolbar, "z: 20") ||
      !contains(toolbar, "objectName: \"startLiveAudioButton\"") ||
      !contains(toolbar, "z: 21") || !contains(spectrum_panel, "clip: true") ||
      !contains(spectrum_panel, "z: 0") ||
      !contains(qml, "objectName: \"emptyStateStartButton\"") ||
      !contains(vfo_editor, "objectName: \"vfoRxEditor\"") ||
      !contains(vfo_editor, "objectName: \"vfoRxEditHitArea\"") ||
      !contains(vfo_editor, "objectName: \"vfoRxFrequencyField\"") ||
      !contains(vfo_editor, "appSettings.setControlledRxFrequency(") ||
      !contains(vfo_editor, "function dismissEdit()") ||
      !contains(vfo_editor, "function cancelEdit()") ||
      !contains(vfo_editor, "function onRadioFrequencyControlChanged()") ||
      !contains(vfo_editor, "onActiveFocusChanged:") ||
      !contains(vfo_editor, "event.key === Qt.Key_Escape") ||
      !contains(vfo_editor, "event.key === Qt.Key_Return") ||
      !contains(vfo_editor, "event.accepted = true") ||
      !contains(vfo_editor, "vfoRxEditor.dismissEdit()") ||
      contains(vfo_editor, "Keys.onReturnPressed:") ||
      contains(vfo_editor, "Keys.onEnterPressed:") ||
      !contains(vfo_editor, "font.family: \"monospace\"") ||
      !contains(vfo_editor, "color: \"#050b10\"") ||
      !contains(vfo_editor, "Accessible.name: \"Edit RX frequency\"") ||
      !contains(qml, "function formatRigFrequency(hz)") ||
      !contains(qml, "objectName: \"vfoRxModeBadge\"") ||
      !contains(qml, "objectName: \"onAirIndicator\"") ||
      !contains(qml, "color: \"transparent\"") ||
      !contains(qml, "border.width: 0") ||
      !contains(qml, "objectName: \"radioTuneButton\"") ||
      !contains(qml, "color: transmitController.tuning ? \"#ff6a24\"") ||
      !contains(qml, "? \"#ff9a45\" : \"#e56b1f\"") ||
      !contains(qml, "Math.ceil(") ||
      !contains(qml, "transmitController.txRemainingSeconds") ||
      !contains(qml, "onClicked: transmitController.toggleTune()") ||
      !contains(qml, "hard 15-second watchdog") ||
      !contains(qml, "objectName: \"cancelTransmissionButton\"") ||
      !contains(qml, "transmitController.cancelTransmission()") ||
      !contains(qml, "objectName: \"txReportField\"") ||
      !contains(qml, "objectName: \"txExchangeField\"") ||
      !contains(qml, "transmitController.prepareExchange()") ||
      !contains(qml, "objectName: \"txMacroRow\"") ||
      !contains(qml, "transmitController.prepareMacro(appSettings.txMacro3)") ||
      !contains(qml, "Prepare only — exact preview required") ||
      !contains(qml, "objectName: \"txProgressPanel\"") ||
      !contains(qml, "objectName: \"txCountdownLabel\"") ||
      !contains(qml, "objectName: \"txProgressBar\"") ||
      !contains(qml, "transmitController.txRemainingSeconds") ||
      !contains(qml, "transmitController.txElapsedSeconds") ||
      !contains(qml, "value: transmitController.txProgress") ||
      !contains(qml, "objectName: \"vfoTxLabel\"") ||
      !contains(qml, "objectName: \"vfoTxModeBadge\"") ||
      !contains(qml, "objectName: \"vfoTxFrequencyField\"") ||
      !contains(qml, "appSettings.radioRxMode") ||
      !contains(qml, "appSettings.radioTxMode") ||
      !contains(qml, "appSettings.radioTxModeTarget") ||
      !contains(qml, "appSettings.radioTxModeConfirmed") ||
      !contains(qml, "? \"CONFIRMED\" : \"TARGET\"") ||
      !contains(qml, "objectName: \"vfoFrequencySyncButton\"") ||
      !contains(qml, "property int controlButtonSize: 52") ||
      !contains(qml, "anchors.rightMargin: vfoDisplay.controlButtonSize + 5") ||
      !contains(qml, "text: \"A=B\"") || !contains(qml, "width: 28") ||
      !contains(qml, "Layout.minimumWidth: 150") ||
      !contains(qml, "fontSizeMode: Text.Fit") ||
      !contains(qml, "elide: Text.ElideNone") ||
      !contains(qml, "text: \"Radio Control\"") ||
      !contains(qml, "text: \"CW Decoder\"") ||
      !contains(qml, "text: appSettings.radioDisplayName") ||
      !contains(qml, "appSettings.radioTxFrequencySyncAvailable") ||
      !contains(qml, "appSettings.syncControlledTxFrequencyToRx()") ||
      !contains(qml, "TX mode is not copied") ||
      !contains(qml, "appSettings.setControlledTxFrequency(") ||
      !contains(qml, "objectName: \"txSliceGuideOverlay\"") ||
      !contains(qml, "appSettings.radioTxVfoFrequencyHz") ||
      !contains(qml, "replayController.rfFrequencyToDisplayHz(tx)") ||
      contains(qml, "appSettings.cwToneSidebandIndex === 0") ||
      !contains(qml, "? \"SPLIT\" : \"SIMPLEX\"") ||
      !contains(qml, "Component.onCompleted: {") ||
      !contains(qml, "showMaximized()") ||
      !contains(qml, "objectName: \"vfoRxEditErrorLabel\"") ||
      // Cards are reordered by dragging. A keyboard path is kept on the same
      // control so the ordering is reachable without a pointer.
      !contains(qml, "objectName: \"decoderSessionDragHandle\"") ||
      !contains(qml, "objectName: \"decoderSessionDragHandler\"") ||
      !contains(qml, "Keys.onUpPressed") ||
      !contains(qml, "Keys.onDownPressed") ||
      !contains(qml, "replayController.moveDecoderSession(") ||
      !contains(qml, "objectName: \"decoderSessionMonitorButton\"") ||
      !contains(qml, "replayController.toggleMonitorChannel(") ||
      !contains(qml, "replayController.isMonitorChannelEnabled(") ||
      contains(qml, "objectName: \"monitorSignalButton\"") ||
      contains(qml, "text: \"STREAM\"") ||
      !contains(qml, "objectName: \"closeDecoderSessionButton\"") ||
      !contains(qml, "onPressed: replayController.closeDecoderSession(")) {
    return 8;
  }

  const auto settings_path =
      std::filesystem::path(CWA_MAIN_QML_PATH).parent_path() /
      "SettingsPane.qml";
  std::ifstream settings_source(settings_path, std::ios::binary);
  if (!settings_source) return 9;
  const std::string settings_qml{
      std::istreambuf_iterator<char>{settings_source},
      std::istreambuf_iterator<char>{}};
  if (!contains(settings_qml, "TabButton { text: \"Decoder\" }") ||
      !contains(settings_qml,
                "objectName: \"keyingModelAdaptiveThresholdRadio\"") ||
      !contains(settings_qml, "objectName: \"keyingModelSemiMarkovRadio\"") ||
      // Stored by name, so the preference survives another technique being
      // added to the list or the list being reordered.
      !contains(settings_qml,
                "appSettings.keyingModel = \"adaptive-threshold\"") ||
      !contains(settings_qml, "appSettings.keyingModel = \"semi-markov\"") ||
      !contains(settings_qml,
                "objectName: \"operatorRoleSearchAndPounceRadio\"") ||
      !contains(settings_qml, "objectName: \"operatorRoleRunnerRadio\"") ||
      !contains(settings_qml,
                "appSettings.operatorRole = \"search-and-pounce\"") ||
      !contains(settings_qml, "objectName: \"settingsDebugCaptureButton\"") ||
      !contains(settings_qml,
                "objectName: \"settingsDebugCaptureFolderButton\"") ||
      !contains(settings_qml, "replayController.openDebugCaptureFolder()") ||
      !contains(settings_qml,
                "objectName: \"debugCaptureMaximumSecondsSpin\"") ||
      !contains(settings_qml,
                "appSettings.debugCaptureMaximumSeconds = value") ||
      !contains(settings_qml, "objectName: \"localDecoderEnabledCheck\"") ||
      !contains(settings_qml, "appSettings.localDecoderBackendAvailable") ||
      !contains(settings_qml, "objectName: \"localDecoderModelDialog\"") ||
      !contains(settings_qml,
                "appSettings.selectLocalDecoderModel(selectedFile)") ||
      !contains(settings_qml, "objectName: \"localDecoderMetadataDialog\"") ||
      !contains(settings_qml,
                "appSettings.selectLocalDecoderMetadata(selectedFile)") ||
      !contains(settings_qml, "objectName: \"localDecoderStatusLabel\"") ||
      !contains(settings_qml, "appSettings.localDecoderStatus") ||
      !contains(settings_qml,
                "objectName: \"localCallsignDatabaseEnabledCheck\"") ||
      !contains(settings_qml,
                "objectName: \"localCallsignDatabasePathField\"") ||
      !contains(settings_qml,
                "objectName: \"reloadLocalCallsignDatabaseButton\"") ||
      !contains(settings_qml, "appSettings.reloadLocalCallsignDatabase()") ||
      !contains(settings_qml,
                "objectName: \"localCallsignDatabaseStatusLabel\"") ||
      !contains(settings_qml,
                "replayController.offlineCallsignDatabaseStatus") ||
      !contains(settings_qml, "objectName: \"localCallsignDatabaseDialog\"") ||
      !contains(settings_qml,
                "appSettings.selectLocalCallsignDatabase(selectedFile)") ||
      !contains(settings_qml,
                "objectName: \"managedCallsignDatabaseEnabledCheck\"") ||
      !contains(settings_qml,
                "objectName: \"managedCallsignDatabaseAutoUpdateCheck\"") ||
      !contains(settings_qml,
                "objectName: \"managedCallsignDatabaseUpdateButton\"") ||
      !contains(settings_qml,
                "objectName: \"managedCallsignDatabaseStatusLabel\"") ||
      !contains(settings_qml, "callsignDatabaseUpdater.checkForUpdates()") ||
      !contains(settings_qml, "callsignDatabaseUpdater.updateDatabase()") ||
      !contains(settings_qml, "never confirm a stream, replace decoded text") ||
      !contains(settings_qml, "replayController.localCharacterStatus") ||
      !contains(settings_qml, "replayController.localCharacterState") ||
      !contains(settings_qml, "objectName: \"radioTuningStepSlider\"") ||
      !contains(settings_qml, "value: appSettings.radioTuningStepHz / 1000") ||
      !contains(settings_qml,
                "appSettings.radioTuningStepHz = Math.round(value * 1000)")) {
    return 10;
  }
  if (!contains(settings_qml,
                "objectName: \"runDirectKeyingLoopbackButton\"") ||
      !contains(settings_qml, "appSettings.runDirectKeyingLoopback(") ||
      !contains(settings_qml, "appSettings.txMacro1") ||
      !contains(settings_qml, "appSettings.txMacro4") ||
      !contains(settings_qml, "appSettings.directKeyingValidated") ||
      contains(settings_qml, "appSettings.directKeyingValidated =")) {
    return 18;
  }

  std::ifstream main_source(CWA_DESKTOP_MAIN_CPP_PATH, std::ios::binary);
  if (!main_source) return 12;
  std::string main_cpp{std::istreambuf_iterator<char>{main_source},
                       std::istreambuf_iterator<char>{}};
  normalizeLineEndings(main_cpp);
  std::string crlf_probe{"guard\r\ncheck\r\n"};
  normalizeLineEndings(crlf_probe);
  if (crlf_probe != "guard\ncheck\n") return 14;
  if (!contains(main_cpp, "QStringLiteral(\"callsignDatabaseUpdater\")") ||
      !contains(main_cpp, "CallsignDatabaseUpdater::databaseInstalled") ||
      !contains(main_cpp, "callsign_database_updater.installedFilePath()") ||
      !contains(main_cpp, "smoke_test = parser.isSet(smoke_test_option)") ||
      !contains(main_cpp,
                "!parser.isSet(smoke_test_option) &&\n"
                "      callsign_database_updater.managedEnabled()") ||
      !contains(main_cpp, "callsign_database_updater.checkAndInstallIfDue()") ||
      !contains(main_cpp, "transmit_controller.configureHardware(") ||
      !contains(main_cpp, "settings.directKeyingValidated()") ||
      !contains(main_cpp, "transmit_controller.configureRadioSafety(")) {
    return 13;
  }
  std::ifstream app_settings_source(CWA_APP_SETTINGS_CPP_PATH,
                                    std::ios::binary);
  if (!app_settings_source) return 19;
  std::string app_settings_cpp{
      std::istreambuf_iterator<char>{app_settings_source},
      std::istreambuf_iterator<char>{}};
  normalizeLineEndings(app_settings_cpp);
  if (!contains(app_settings_cpp,
                "return requestControlledTxRfFrequency(*rx_rf_hz);") ||
      contains(app_settings_cpp,
               "setControlledTxFrequency(QString::number(*rx_rf_hz), 1U)") ||
      !contains(app_settings_cpp,
                "next_state = hamlib_client_->radioState();") ||
      !contains(app_settings_cpp,
                "hamlib_client_->setTxFrequency(dial_frequency_hz)") ||
      !contains(app_settings_cpp, "hamlib_client_->setSplit(enabled)") ||
      !contains(app_settings_cpp,
                "probe.run(configuration, radio_disconnected_confirmed)") ||
      !contains(app_settings_cpp,
                "directKeyingConfigurationSha256(configuration)") ||
      !contains(app_settings_cpp, "acceptanceConfigurationSha256") ||
      contains(
          app_settings_cpp,
          ".value(storageKey(QStringLiteral(\"keying/directValidated\"))")) {
    return 20;
  }

  // PERF-002, the presentation path. Each of these replaced work that was
  // repeated per pointer sample or per rendered frame with the same work done
  // once per change of its inputs, so each assertion names both the shape that
  // shipped and the shape that must not come back.

  // streamIdAtX() re-read replayController.decoderChannels on every iteration
  // of its own loop, converting a list of up to 64 channel maps of 42 entries
  // each -- transcripts and acoustic alternatives included -- once per channel
  // examined, driven by hoveredStreamId on every pointer sample. The list is
  // read once now, reduced to the two numbers the hit test uses, and only
  // while the pointer is inside the panel, so a decoder publish costs the
  // hover path nothing when nobody is pointing at the spectrum.
  if (!contains(manual, "function buildStreamHoverIndex(channels)") ||
      !contains(manual, "readonly property var streamHoverIndex:") ||
      !contains(manual, "candidate.presentationFrequencyHz") ||
      // The per-iteration property reads, in both of their forms.
      contains(manual, "index < replayController.decoderChannels.length") ||
      contains(manual, "replayController.decoderChannels[index]")) {
    return 21;
  }

  // exactCallCount() ran a regular expression over the concatenation of four
  // accumulated transcripts and built one string per token, per visible card,
  // on every decoded character -- the shape mentionsCallsignToken() was fixed
  // for on the C++ side. A whole-token match is an occurrence bounded by
  // non-token characters, so the search is left to indexOf() and the boundary
  // is tested. The case fold stays, because toUpperCase() maps characters
  // outside ASCII into the token alphabet and an ASCII fold would not.
  if (!contains(qml, "function isCallsignTokenCode(code)") ||
      !contains(qml, "function exactCallCount(text, callsign)") ||
      !contains(qml, "haystack.indexOf(wanted, from)") ||
      !contains(qml, "String(text).toUpperCase()") ||
      contains(qml, "match(/[A-Z0-9/]+/g)")) {
    return 22;
  }

  // The DX spot layout ran entirely inside the placedSpots binding, so it
  // re-converted the published spot list, re-normalised every callsign,
  // re-formatted every frequency, re-measured every label against the font and
  // re-sorted the lot on every zoom step and every frame of a pan. Only the
  // span filter, the pixel positions and the collision pass are functions of
  // the view; the rest is a function of the published list and is computed
  // when that list changes.
  const std::size_t claim_space_start =
      qml.find("function claimLabelSpace(spot, taken, plotWidth)");
  const std::size_t claim_space_end =
      qml.find("function normaliseSpots(source)", claim_space_start);
  if (!contains(qml, "function normaliseSpots(source)") ||
      !contains(qml, "readonly property var normalisedSpots:") ||
      !contains(qml, "? layoutSpots(normalisedSpots,") ||
      !contains(qml, "function labelWidthOf(spot)") ||
      !contains(qml, "labelWidth: labelWidthOf(spot)") ||
      claim_space_start == std::string::npos ||
      claim_space_end == std::string::npos ||
      // The collision pass runs once per marker on every frame of a pan, so
      // the font measurement must not be inside it. labelWidthOf() takes it
      // once per spot instead, and keeps it on the record.
      contains(qml.substr(claim_space_start,
                          claim_space_end - claim_space_start),
               "measuredLabelWidth") ||
      // The whole layout driven straight off the published list.
      contains(qml, "? layoutSpots(replayController.dxSpots,")) {
    return 23;
  }

  // The gesture log is the only gesture record a session log can hold.
  // --log-file installs a Qt message handler and writes what the application
  // reports; nothing else in this tree reports anything, so removing these
  // three lines as superseded by --log-file would leave an operator's log
  // silent about the gesture it was added to diagnose. They are not a cost:
  // all three sit on discrete button events, and onPositionChanged -- the
  // handler that does run once per pointer sample -- carries none of them.
  if (!contains(manual, "console.log(\"region-gesture press\"") ||
      !contains(manual, "console.log(\"region-gesture release\"") ||
      !contains(manual, "console.log(\"region-gesture repoint\"")) {
    return 24;
  }

  // The spectrum markers are driven by an incremental model, not by the
  // decoderChannels list reassigned whole.
  //
  // Assigning a QVariantList to a Repeater destroys and recreates every
  // delegate, and this delegate is three Rectangles, a rotated plated Label
  // and a SequentialAnimation. At the 40 ms publish throttle that was up to
  // twenty-five full rebuilds a second across as many as sixty-four tracked
  // signals, whether or not a marker had moved: the list differs whenever
  // any key of any channel differs, and the levels and the transcript move on
  // nearly every publish while a marker shows neither. Measured on a probe
  // Repeater of eight streams, one stream's signal-to-noise moving by a tenth
  // of a decibel destroyed and rebuilt all eight delegates; over the model
  // the same publish rebuilds none. A destroyed delegate also restarts the
  // "CALLING YOU" pulse and the hover magnification, and is how a pointer
  // press loses its release.
  //
  // The hover index deliberately still reads decoderChannels, and must keep
  // doing so. It needs JavaScript array access over the whole channel, which
  // a QAbstractItemModel does not offer, and the marker rows carry only the
  // nineteen keys a marker is drawn from. Replacing it here would break the
  // hit test without failing anything.
  if (!contains(manual, "model: replayController.decoderMarkerModel") ||
      contains(qml, "model: replayController.decoderChannels") ||
      !contains(manual, "replayController.decoderChannels)")) {
    return 25;
  }
  return 0;
}
