// Source contract for the operator's side of the audio stream.
//
// The C++ tests prove that a peer cannot start receive audio on its own. This
// one guards the other half of the same rule, which lives in QML and which no
// socket test can see: that there is exactly one place in the main window
// where the permission is granted, that it is behind a confirmation which says
// what is being disclosed, and that an indicator stays on the bar for as long
// as audio is leaving the machine.
//
// Read as text rather than by driving the window, in the style of the other
// QML contracts here, because what matters is that no second grant path
// appears. A running window can only show what the one path does; the source
// can be asked whether there is another.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// How many times one string appears. The grant is counted rather than merely
// found: one assignment behind a confirmation is the contract, and a second
// one anywhere would be a way to turn audio on without the operator reading
// what it means.
std::size_t occurrences(const std::string& haystack,
                        const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

}  // namespace

int main() {
  std::ifstream stream(CWA_MAIN_QML_PATH);
  if (!stream) return 1;
  std::stringstream buffer;
  buffer << stream.rdbuf();
  const std::string qml = buffer.str();
  if (qml.empty()) return 2;

  // An indicator on the status bar, built from the count of observers actually
  // being sent audio rather than from the permission, so it says "leaving this
  // machine" and not "allowed to".
  if (!contains(qml, "objectName: \"audioStreamStatusChip\"") ||
      !contains(qml, "diagnosticsServer.audioSubscriberCount") ||
      !contains(qml, "diagnosticsServer.audioStreamingEnabled") ||
      !contains(qml, "\"AUDIO OUT \" + audioStreamChip.listeners")) {
    return 3;
  }

  // Allowed is not the same as able. The audio half shares the diagnostics
  // port over UDP, and a port it could not bind must show as its own state
  // rather than as a service that is ready and quietly sending nothing.
  if (!contains(qml, "diagnosticsServer.audioListening") ||
      !contains(qml, "\"AUDIO DOWN\"")) {
    return 9;
  }

  // Visible while the diagnostics service is up even when audio is off, so the
  // state an operator most wants to be able to confirm -- that audio is not
  // going anywhere -- is one they can actually see.
  if (!contains(qml, "visible: audioStreamChip.serviceListening")) return 4;

  // The confirmation, and what it has to say. Audio is a larger disclosure
  // than the telemetry beside it, and the operator has to be told which of the
  // two they are agreeing to.
  //
  // Anchored on the closing quote of the displayed strings, not on the phrases
  // alone. A comment in this file explaining why the disclosure is worded that
  // way would otherwise satisfy an assertion about the wording, and a source
  // contract that a comment can satisfy is not a contract.
  if (!contains(qml, "objectName: \"audioStreamConsentDialog\"") ||
      !contains(qml, "standardButtons: Dialog.Cancel | Dialog.Ok") ||
      !contains(qml,
                "every signal in the passband, not only what this "
                "application decoded.\"") ||
      !contains(qml,
                "It is sent unencrypted, over UDP on the same port number as "
                "the diagnostics stream,")) {
    return 5;
  }

  // EXACTLY ONE GRANT, AND IT IS THE DIALOG'S. A second assignment anywhere --
  // a settings row wired straight through, a keyboard shortcut, a restored
  // value at startup -- would be a way for audio to be allowed without the
  // operator having read the sentence above.
  if (occurrences(qml, "diagnosticsServer.audioStreamingEnabled = true") != 1) {
    return 6;
  }
  if (!contains(qml,
                "onAccepted: diagnosticsServer.audioStreamingEnabled = true")) {
    return 7;
  }

  // Withdrawing is immediate and unconfirmed. An operator taking this back is
  // taking it back because of what is being sent now, and a dialog in the way
  // would be a dialog between them and stopping it.
  if (!contains(qml, "diagnosticsServer.audioStreamingEnabled = false")) {
    return 8;
  }

  // The local half of the same audio. Region listening is a monitor mode
  // beside OFF and RX, and its label has to say it is the whole window rather
  // than one stream -- an operator who reads it as "listen to the selected
  // signal" will report the pitch as wrong when it is exactly right.
  if (!contains(qml, "objectName: \"monitorRegionButton\"") ||
      !contains(qml, "replayController.setMonitorMode(3)") ||
      !contains(qml, "Listen to the whole decode region at once, not one "
                     "stream")) {
    return 9;
  }

  // THE TAP ITSELF, which is the half that had no test and no caller.
  // DiagnosticsServer::publishAudio existed, was tested, and was reached from
  // nowhere: the service could be enabled, a peer could subscribe, and the
  // stream carried silence. The wiring lives in main.cpp and cannot be
  // observed from a running window, so it is asserted as source.
  std::ifstream main_stream(CWA_DESKTOP_MAIN_CPP_PATH);
  if (!main_stream) return 10;
  std::stringstream main_buffer;
  main_buffer << main_stream.rdbuf();
  const std::string main_cpp = main_buffer.str();
  if (main_cpp.empty()) return 11;
  if (!contains(main_cpp, "ReplayController::receiveAudioProduced") ||
      !contains(main_cpp, "diagnostics_server.publishAudio(")) {
    return 12;
  }
  // And the return path. Region audio is demodulated only while somebody is
  // actually being sent it, so the subscriber count has to reach the worker;
  // without this the producer would either run for a permission nobody is
  // using or never run at all.
  if (!contains(main_cpp, "DiagnosticsServer::stateChanged") ||
      !contains(main_cpp, "setRemoteAudioSubscribed(") ||
      !contains(main_cpp, "diagnostics_server.audioSubscriberCount() > 0")) {
    return 13;
  }
  return 0;
}
