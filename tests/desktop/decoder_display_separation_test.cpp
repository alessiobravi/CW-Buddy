// Source contract: display settings must not reach the decoder.
//
// Two operator-visible defects motivated this check. The receive workers used
// to hand CwChannelBank the display-averaged spectrum, so the Avg slider
// changed candidate discovery and the decoded text. They also reset the
// decoder unconditionally on every configure() call, so moving any display
// control discarded every track, transcript and confirmed callsign while the
// user-interface model kept showing the previous contents.
//
// This is a text-level contract because the wiring lives in Qt worker objects
// that the dependency-free test suite cannot instantiate. The behavioural
// guarantee itself is covered by the core suite's display-invariance case.

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

namespace {

bool contains(const std::string& value, const std::string& expected) {
  return value.find(expected) != std::string::npos;
}

void normalizeLineEndings(std::string& value) {
  value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
}

bool readSource(const char* path, std::string& contents) {
  std::ifstream source(path, std::ios::binary);
  if (!source) return false;
  contents.assign(std::istreambuf_iterator<char>{source},
                  std::istreambuf_iterator<char>{});
  normalizeLineEndings(contents);
  return true;
}

// Detection must consume the unaveraged bins so that the analyzer's display
// averaging cannot change which signals are found. The detector applies its
// own fixed-time smoothing internally.
bool feedsDetectorUnaveragedBins(const std::string& source) {
  return contains(source, "snapshot.instantaneous_bins_dbfs") &&
         !contains(source, "snapshot.upper_frequency_hz, snapshot.bins_dbfs))");
}

// Only a change to the audio actually presented to the detector may discard
// decoder state. Averaging and the display line rate must not.
bool resetsOnlyOnSignalPathChange(const std::string& source) {
  return contains(source, "const bool signal_path_changed =") &&
         contains(source,
                  "config.audio_lower_frequency_hz != "
                  "previous.audio_lower_frequency_hz") &&
         contains(source, "if (signal_path_changed)") &&
         !contains(source,
                   "static_cast<void>(analyzer_.configure(config));\n"
                   "  decoder_.reset();") &&
         !contains(source,
                   "static_cast<void>(analyzer_.configure(config));\n"
                   "    decoder_.reset();");
}

// Opening a decoder card is a visual/session action. Audio monitoring is an
// independent operator choice made by the toolbar or the card's speaker.
bool openingDecoderDoesNotStartMonitoring(const std::string& source) {
  const auto begin = source.find(
      "void ReplayController::openDecoderSession(const qulonglong channel_id)");
  const auto end =
      source.find("void ReplayController::openManualDecoderSession", begin);
  if (begin == std::string::npos || end == std::string::npos || end <= begin) {
    return false;
  }
  const std::string method = source.substr(begin, end - begin);
  return contains(method, "decoder_session_order_.push_back(channel_id)") &&
         !contains(method, "monitor_mode_") &&
         !contains(method, "monitored_channel_ids_") &&
         !contains(method, "publishMonitorConfiguration");
}

bool defersManualSelectionUntilSdrSpectrumIsReady(const std::string& source) {
  return contains(source,
                  "pending_manual_frequency_hz_ = audio_frequency_hz;") &&
         contains(source, "pending_manual_frequency_hz_.has_value() &&") &&
         contains(source, "!decoder_snapshots.empty()") &&
         contains(source,
                  "decoder_.selectFrequency(*pending_manual_frequency_hz_)") &&
         contains(
             source,
             "emit manualDecoderSelected(static_cast<qulonglong>(channel_id))");
}


// A failure that stopped reception is published as its own property, whole.
//
// statusText is a single elided line and always has been; that is right for a
// running commentary and wrong for a failure. An operator was shown
//
//     Live audio error: The selected audio input is un...
//
// and the half that got cut -- "unavailable. Reconnect it or select another
// input." -- is the half that says what to do. A presentation layer cannot be
// asked to recognise a failure by looking for the word "error" inside prose
// written for a human, because that guess breaks the first time a message is
// reworded, so the controller says which failures are blocking.
//
// Text-level, because these four sites are lambdas attached to worker signals
// that only fire when real hardware fails. The two blocking sites a test can
// reach without a device are asserted behaviourally in
// spectrum_waterfall_startup_test.cpp.
bool publishesBlockingFailuresWhole(const std::string& source) {
  return contains(source, "const QString& ReplayController::blockingError()") &&
         contains(source, "void ReplayController::setBlockingError(QString message)") &&
         contains(source, "void ReplayController::dismissBlockingError()") &&
         contains(source,
                  "setBlockingError(QStringLiteral(\"WAV replay error: %1\")"
                  ".arg(message));") &&
         contains(source,
                  "setBlockingError(\n                QStringLiteral("
                  "\"Live audio error: %1\").arg(message));") &&
         contains(source,
                  "setBlockingError(\n                QStringLiteral("
                  "\"Live SDR error: %1\").arg(message));") &&
         contains(source,
                  "setBlockingError(QStringLiteral(\n          \"Audio-input "
                  "permission was denied.") &&
         contains(source,
                  "setBlockingError(QStringLiteral(\n        \"Select a "
                  "discovered SDR device in Settings before starting RX.\"));");
}

// A refused retune reports something that failed without stopping anything:
// the receiver is still running on the frequency it was already on. A dialog
// the operator has to dismiss is itself an interruption, and an error that
// stopped nothing has not earned one, so this stays on the status line.
bool leavesNonBlockingFailuresOnTheStatusLine(const std::string& source) {
  return contains(source,
                  "setStatus(QStringLiteral(\"Live SDR retune error: %1\")"
                  ".arg(message));") &&
         !contains(source,
                   "setBlockingError(QStringLiteral(\"Live SDR retune error");
}

// A dismissed failure must not come back on the next unrelated state change,
// so reception starting successfully clears it -- for a recording that opened,
// for live audio that started, and for an SDR that started.
bool clearsBlockingFailureWhenReceptionStarts(const std::string& source) {
  return contains(source,
                  "source_loaded_ = true;\n            playing_ = false;\n"
                  "            blocking_error_.clear();") &&
         contains(source,
                  "blocking_error_.clear();\n            status_text_ =\n"
                  "                QStringLiteral(\"Live RX: %1") &&
         contains(source,
                  "blocking_error_.clear();\n            status_text_ = "
                  "QStringLiteral(\n                \"Live SDR: %1") &&
         contains(source,
                  "clearBlockingError();\n  setStatus(QStringLiteral("
                  "\"Starting live audio from %1") &&
         contains(source,
                  "clearBlockingError();\n  setStatus(QStringLiteral("
                  "\"Starting live SDR from %1");
}

// The per-channel presentation work must not be redone for a stream whose
// evidence did not move.
//
// rebuildDecoderModels() runs on the GUI thread for every stream on every
// publish -- 23 publishes a second across 24 streams on the reporting station.
// It used to scan the whole cumulative transcript for the operator's callsign
// by splitting it on a freshly compiled pattern, and to rerun the advisory
// callsign search, on every one of those. The transcript grows for the life of
// the stream, so the cost grew with it: measured at 2.8 seconds of GUI thread
// for every second of publishes at 32,000 characters, against a reported
// freeze of 2.1 seconds. The behavioural guarantees are asserted in
// spectrum_waterfall_startup_test.cpp; this keeps the two shapes that caused
// it from being written back.
bool derivesChannelPresentationThroughTheCache(const std::string& source) {
  return contains(source, "channel_presentation_.derive(") &&
         contains(source, "channel_presentation_.setContext(") &&
         contains(source, "channel_presentation_.endPublish();") &&
         contains(source, "channel_presentation_.invalidate();") &&
         !contains(source, "QRegularExpression(QStringLiteral(\"[^A-Z0-9/]+\"))") &&
         !contains(source,
                   "if (const auto suggestion = advisoryCallsignPresentation(\n"
                   "              item,");
}

}  // namespace

int main() {
  std::string live_worker;
  if (!readSource(CWA_LIVE_AUDIO_WORKER_PATH, live_worker)) return 1;
  std::string replay_controller;
  if (!readSource(CWA_REPLAY_CONTROLLER_PATH, replay_controller)) return 2;

  std::string crlf_probe{"guard\r\ncheck\r\n"};
  normalizeLineEndings(crlf_probe);
  if (crlf_probe != "guard\ncheck\n") return 3;

  if (!feedsDetectorUnaveragedBins(live_worker)) return 4;
  if (!feedsDetectorUnaveragedBins(replay_controller)) return 5;
  if (!resetsOnlyOnSignalPathChange(live_worker)) return 6;
  if (!resetsOnlyOnSignalPathChange(replay_controller)) return 7;
  if (!openingDecoderDoesNotStartMonitoring(replay_controller)) return 8;
  if (!defersManualSelectionUntilSdrSpectrumIsReady(live_worker)) return 9;
  if (!publishesBlockingFailuresWhole(replay_controller)) return 10;
  if (!leavesNonBlockingFailuresOnTheStatusLine(replay_controller)) return 11;
  if (!clearsBlockingFailureWhenReceptionStarts(replay_controller)) return 12;
  if (!derivesChannelPresentationThroughTheCache(replay_controller)) return 13;

  return 0;
}
