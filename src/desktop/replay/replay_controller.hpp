#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <cstdint>
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../visualization/spectrum_frame.hpp"
#include "cwassistant/core/cw_character_decoder.hpp"
#include "cwassistant/core/cw_spot_registry.hpp"
#include "cwassistant/core/offline_callsign_database.hpp"

class QAudioSink;
class QIODevice;

namespace cwassistant::desktop {

class DxClusterClient;

// Keeps QML decoder-card delegates alive while rapidly changing transcript,
// level, and key-state roles are updated. A QVariantList model resets every
// delegate on each assignment, which can destroy a button between pointer
// press and release.
class DecoderSessionListModel final : public QAbstractListModel {
 public:
  explicit DecoderSessionListModel(QObject* parent = nullptr);
  [[nodiscard]] int rowCount(
      const QModelIndex& parent = QModelIndex{}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index,
                              int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
  void replace(const QVariantList& sessions);

 private:
  static constexpr int kModelDataRole = Qt::UserRole + 1;
  QVariantList sessions_;
};

// Keeps an operator-opened decoder card attached to the retained visual
// stream when the low-level tracker reacquires that same color/frequency with
// a new internal ID.
[[nodiscard]] QList<qulonglong> reconcileDecoderSessionOrder(
    const QList<qulonglong>& requested_order,
    const QVariantList& previous_sessions,
    const QVariantList& current_channels);

// Returns only a callsign-bearing span whose stable consensus grew in the
// current inference update. Older append-only text cannot be reused as fresh
// verification evidence when unrelated later characters arrive.
[[nodiscard]] std::optional<std::string> freshCharacterRefinementCallEvidence(
    std::string_view stable_text, std::size_t previous_stable_size);

struct AdvisoryCallsignPresentation {
  QString callsign;
  QString raw_span;
  bool database_match{false};
  // The presented callsign is the nearest directory entry to the acoustic
  // winner rather than the winner itself.
  bool database_corrected{false};
  int agreeing_alternatives{0};
  double acoustic_support{0.0};
  double relative_cost{0.0};
};

// Selects only the acoustically strongest callsign present in at least two
// current bounded alternatives and within two wildcard-aware edits of the
// latest completed '?' span. The database annotates that winner, and where the
// winner is not listed it may substitute the single directory entry that is
// strictly closest to it within two edits -- a near miss on a verified stream
// is usually the listed station misread. It can do neither where the
// neighbourhood is ambiguous, and in no case may it promote an acoustically
// weaker candidate, mutate the transcript, or affect verification: the result
// is advisory presentation only.
//
// Substitution is off unless the operator enables it. Two listed stations can
// differ by one character, so a correction can name a station that was never
// heard; without it the acoustic winner is always what is shown, which is the
// behaviour this application has always had.
[[nodiscard]] std::optional<AdvisoryCallsignPresentation>
advisoryCallsignPresentation(const QVariantMap& channel,
                             const cwassistant::core::OfflineCallsignDatabase&
                                 database,
                             QString* diagnostic_reason = nullptr,
                             bool allow_database_correction = false);

// The two expensive parts of a published decoder channel, kept against the
// exact evidence they were derived from.
//
// rebuildDecoderModels() runs on the GUI thread, for every visible stream, on
// every publish: measured on a busy band at 23 publishes a second across 24
// streams, so roughly 550 derivations a second on the thread that draws. Two
// of the derived values cost real time there.
//
// The first is whether the operator's own callsign appears in the transcript.
// The transcript is append-only and unbounded, so asking that question by
// splitting the whole of it grew more expensive for as long as the application
// stayed open. The second is advisoryCallsignPresentation(), which walks a
// wildcard span, runs an edit-distance program against every acoustic
// alternative and may search the whole offline directory.
//
// Measured in an optimized build over 24 streams at 23 publishes a second,
// with the 32,000-character transcripts an hour on a busy band produces: those
// two cost between 1.85 and 2.09 seconds of GUI thread for every second of
// publishes, against a reported freeze of up to 2.1 seconds. The event loop
// cannot catch up with that, which is why an empty band was fine and a full
// one was not.
//
// Both are pure functions of the evidence below, and nobody reads a callsign
// suggestion 23 times a second. When none of the evidence moved, the previous
// answer is still the right answer, so it is returned rather than recomputed.
// The same publishes then cost 2 to 7 ms when no stream changed and 63 ms with
// every stream changing on every publish, which is a rate no operator can
// produce: it needs 23 committed characters a second on each of 24 streams.
// This is the memo the decoder already keeps for word-gap reconstruction
// (word_gap_cache_input_ in cw_decoder.cpp), applied one layer up.
class DecoderChannelPresentationCache {
 public:
  struct Derived {
    bool calling_own_station{false};
    bool callsign_in_database{false};
    QString suggestion;
    QString suggestion_raw_span;
    QString suggestion_source;
    int suggestion_agreeing_alternatives{0};
    double suggestion_support{0.0};
    double suggestion_relative_cost{0.0};
    QString suggestion_diagnostic;
  };

  // Evidence that does not live in any one channel. Changing the operator's
  // callsign or the correction preference changes every stream's answer at
  // once, so every retained answer is dropped.
  void setContext(const QString& own_callsign, bool allow_database_correction);
  // The directory itself was reloaded. Its contents are not compared, so the
  // caller says when they moved.
  void invalidate() noexcept;

  [[nodiscard]] const Derived& derive(
      qulonglong channel_id, const QVariantMap& channel,
      const cwassistant::core::OfflineCallsignDatabase& database);

  // Called once a publish has derived every live stream. A stream that has
  // gone keeps no entry: its channel id is never published again.
  void endPublish() noexcept;

  // How many times derive() has had to compute an answer rather than return a
  // retained one. A publish that changed nothing must not move this, which is
  // the whole claim this class makes and the only way to assert it without
  // timing anything.
  [[nodiscard]] qulonglong computations() const noexcept {
    return computations_;
  }
  [[nodiscard]] std::size_t retainedStreams() const noexcept {
    return entries_.size();
  }

 private:
  struct Entry {
    // The evidence. Held as the published QStrings and QVariantList, which are
    // implicitly shared, so retaining them costs a reference rather than a
    // copy of the transcript.
    QString text;
    QString refined_text;
    QString callsign;
    QVariantList acoustic_alternatives;
    bool verified_cw{false};
    bool valid{false};
    qulonglong publish{0};
    Derived derived;
  };

  std::unordered_map<qulonglong, Entry> entries_;
  QString own_callsign_;
  bool allow_database_correction_{false};
  qulonglong publish_{1};
  qulonglong computations_{0};
};

class ReplayController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString sourceName READ sourceName NOTIFY stateChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
  // The full text of a failure that stopped reception, or left it unable to
  // start; empty when nothing is blocking.
  //
  // statusText is one elided line, which is right for a running commentary and
  // wrong for a failure: an operator was shown "Live audio error: The selected
  // audio input is un..." and lost exactly the half that says what to do about
  // it. This carries the same message uncut so a dialog can present it, and it
  // is a separate property because a presentation layer must not have to
  // recognise failures by looking for the word "error" in prose written for a
  // human and liable to be reworded.
  Q_PROPERTY(QString blockingError READ blockingError NOTIFY stateChanged)
  Q_PROPERTY(bool sourceLoaded READ sourceLoaded NOTIFY stateChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
  Q_PROPERTY(int sourceMode READ sourceMode WRITE setSourceMode NOTIFY stateChanged)
  Q_PROPERTY(bool liveCapturing READ liveCapturing NOTIFY stateChanged)
  Q_PROPERTY(bool activeSource READ activeSource NOTIFY stateChanged)
  Q_PROPERTY(qulonglong inputOverruns READ inputOverruns NOTIFY stateChanged)
  Q_PROPERTY(double sampleRate READ sampleRate NOTIFY stateChanged)
  Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY stateChanged)
  Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY stateChanged)
  Q_PROPERTY(int averagingFrames READ averagingFrames WRITE setAveragingFrames
                 NOTIFY averagingFramesChanged)
  Q_PROPERTY(QVariantList decoderChannels READ decoderChannels
                 NOTIFY decoderChanged)
  Q_PROPERTY(int decoderChannelCount READ decoderChannelCount
                 NOTIFY decoderChanged)
  Q_PROPERTY(QVariantList decoderSessions READ decoderSessions
                 NOTIFY decoderChanged)
  Q_PROPERTY(QAbstractItemModel* decoderSessionModel READ decoderSessionModel
                 CONSTANT)
  Q_PROPERTY(int decoderSessionCount READ decoderSessionCount
                 NOTIFY decoderChanged)
  Q_PROPERTY(QVariantMap verificationDiagnostics READ verificationDiagnostics
                 NOTIFY decoderChanged)
  Q_PROPERTY(QString localCharacterState READ localCharacterState
                 NOTIFY decoderChanged)
  Q_PROPERTY(QString localCharacterStatus READ localCharacterStatus
                 NOTIFY decoderChanged)
  Q_PROPERTY(bool decodeWeakSignals READ decodeWeakSignals
                 NOTIFY weakSignalDecodingChanged)
  Q_PROPERTY(double minimumDecodeSnrDb READ minimumDecodeSnrDb
                 NOTIFY weakSignalDecodingChanged)
  Q_PROPERTY(QString offlineCallsignDatabaseState READ offlineCallsignDatabaseState
                 NOTIFY decoderChanged)
  Q_PROPERTY(QString offlineCallsignDatabaseStatus READ offlineCallsignDatabaseStatus
                 NOTIFY decoderChanged)
  Q_PROPERTY(int offlineCallsignDatabaseEntries READ offlineCallsignDatabaseEntries
                 NOTIFY decoderChanged)
  Q_PROPERTY(bool debugCaptureActive READ debugCaptureActive
                 NOTIFY debugCaptureChanged)
  Q_PROPERTY(QString debugCapturePath READ debugCapturePath
                 NOTIFY debugCaptureChanged)
  Q_PROPERTY(double debugCaptureElapsedSeconds READ debugCaptureElapsedSeconds
                 NOTIFY debugCaptureChanged)
  Q_PROPERTY(QString debugCaptureNote READ debugCaptureNote
                 NOTIFY debugCaptureChanged)
  // True when the spectrum's horizontal axis can be labelled in absolute RF.
  // Direct IQ always can. An audio card can only once a radio is reporting its
  // dial, because the audio passband alone says nothing about where the
  // receiver is pointed.
  // NOTIFY radioFrequencyChanged, not stateChanged. Everything the axis
  // mapping reads -- whether a radio is readable, its receive frequency, the
  // sideband and the reference tone -- is set in setRadioFrequencyContext,
  // which emits that signal and not the other. Bound to stateChanged, the
  // labels did not re-evaluate when the operator changed band: switching from
  // 40 m to 20 m left the ruler still printing 7 MHz, because on an audio card
  // the spectrum's own bounds do not move with the dial and nothing else in
  // the binding had changed. A source change is covered by the frame bounds
  // moving, which the binding also reads.
  Q_PROPERTY(bool axisShowsRf READ axisShowsRf NOTIFY radioFrequencyChanged)
  Q_PROPERTY(bool radioFrequencyAvailable READ radioFrequencyAvailable
                 NOTIFY radioFrequencyChanged)
  Q_PROPERTY(qulonglong radioRxFrequencyHz READ radioRxFrequencyHz
                 NOTIFY radioFrequencyChanged)
  Q_PROPERTY(qulonglong radioTxFrequencyHz READ radioTxFrequencyHz
                 NOTIFY radioFrequencyChanged)
  Q_PROPERTY(bool radioSplitActive READ radioSplitActive
                 NOTIFY radioFrequencyChanged)
  Q_PROPERTY(int monitorMode READ monitorMode WRITE setMonitorMode
                 NOTIFY monitorChanged)
  Q_PROPERTY(qulonglong monitoredChannelId READ monitoredChannelId
                 NOTIFY monitorChanged)
  Q_PROPERTY(QVariantList monitoredChannelIds READ monitoredChannelIds
                 NOTIFY monitorChanged)
  Q_PROPERTY(QString monitorStatus READ monitorStatus NOTIFY monitorChanged)
  Q_PROPERTY(double monitorLevel READ monitorLevel WRITE setMonitorLevel
                 NOTIFY monitorChanged)
  // What other receivers currently report hearing, as read-only presentation
  // data. Each entry carries callsign, frequencyHz, displayFrequencyHz,
  // reverseBeacon, cluster, ageSeconds and observations.
  //
  // This is corroboration and never authority. Nothing here may replace,
  // rewrite, or auto-fill a decoded callsign, nothing here changes what the
  // decoder believes, and nothing here reaches the transmit path. It says what
  // somebody else reported; the operator decides what that is worth.
  Q_PROPERTY(QVariantList dxSpots READ dxSpots NOTIFY dxSpotsChanged)
  Q_PROPERTY(QString dxSpotsStatus READ dxSpotsStatus NOTIFY dxSpotsChanged)
  // Whether the telnet link is up right now, and what it is doing, for a
  // status indicator that has to be readable without opening settings. The
  // pair is separate from dxSpotsStatus because that line describes the spot
  // store, while these describe the connection that feeds it: a link that is
  // down while spots are still within their retention window is exactly the
  // state an operator needs to be able to see.
  Q_PROPERTY(bool dxClusterConnected READ dxClusterConnected
                 NOTIFY dxClusterStateChanged)
  Q_PROPERTY(QString dxClusterStatus READ dxClusterStatus
                 NOTIFY dxClusterStateChanged)

 public:
  explicit ReplayController(QObject* parent = nullptr);
  ~ReplayController() override;

  [[nodiscard]] const QString& sourceName() const noexcept;
  [[nodiscard]] const QString& statusText() const noexcept;
  [[nodiscard]] const QString& blockingError() const noexcept;
  // The operator has read and dismissed the dialog. Clearing it here rather
  // than in the presentation layer means a dismissed failure cannot be
  // resurrected by the next unrelated stateChanged().
  Q_INVOKABLE void dismissBlockingError();
  [[nodiscard]] bool sourceLoaded() const noexcept;
  [[nodiscard]] bool playing() const noexcept;
  [[nodiscard]] int sourceMode() const noexcept;
  [[nodiscard]] bool liveCapturing() const noexcept;
  [[nodiscard]] bool activeSource() const noexcept;
  [[nodiscard]] qulonglong inputOverruns() const noexcept;
  [[nodiscard]] double sampleRate() const noexcept;
  [[nodiscard]] double durationSeconds() const noexcept;
  [[nodiscard]] double positionSeconds() const noexcept;
  [[nodiscard]] int averagingFrames() const noexcept;
  [[nodiscard]] const QVariantList& decoderChannels() const noexcept;
  [[nodiscard]] int decoderChannelCount() const noexcept;
  [[nodiscard]] const QVariantList& decoderSessions() const noexcept;
  [[nodiscard]] QAbstractItemModel* decoderSessionModel() noexcept;
  [[nodiscard]] int decoderSessionCount() const noexcept;
  [[nodiscard]] const QVariantMap& verificationDiagnostics() const noexcept;
  [[nodiscard]] const QString& localCharacterState() const noexcept;
  [[nodiscard]] const QString& localCharacterStatus() const noexcept;
  Q_INVOKABLE void setCallsignDatabaseCorrectionEnabled(bool value);
  [[nodiscard]] const QString& offlineCallsignDatabaseState() const noexcept;
  [[nodiscard]] const QString& offlineCallsignDatabaseStatus() const noexcept;
  [[nodiscard]] int offlineCallsignDatabaseEntries() const noexcept;
  [[nodiscard]] bool debugCaptureActive() const noexcept;
  [[nodiscard]] const QString& debugCapturePath() const noexcept;
  [[nodiscard]] double debugCaptureElapsedSeconds() const noexcept;
  [[nodiscard]] const QString& debugCaptureNote() const noexcept;
  [[nodiscard]] bool radioFrequencyAvailable() const noexcept;
  [[nodiscard]] bool axisShowsRf() const noexcept;
  // Maps a point on the spectrum's own axis to the frequency to print there.
  //
  // The axis carries whatever the analyser produced, which for an audio card
  // is the passband -- 0 to 24 kHz -- and that is not what an operator reads a
  // band by. The decoder cards already showed absolute RF; the ruler under the
  // spectrum did not, so the same signal was named two different ways on one
  // screen. Falls back to the axis value unchanged when there is no dial to
  // map against, because an unlabelled axis is worse than an honest audio one.
  Q_INVOKABLE double axisFrequencyHz(double axis_hz) const noexcept;
  [[nodiscard]] qulonglong radioRxFrequencyHz() const noexcept;
  [[nodiscard]] qulonglong radioTxFrequencyHz() const noexcept;
  [[nodiscard]] bool radioSplitActive() const noexcept;
  [[nodiscard]] int monitorMode() const noexcept;
  [[nodiscard]] qulonglong monitoredChannelId() const noexcept;
  [[nodiscard]] QVariantList monitoredChannelIds() const;
  [[nodiscard]] const QString& monitorStatus() const noexcept;
  [[nodiscard]] double monitorLevel() const noexcept;
  [[nodiscard]] const QVariantList& dxSpots() const noexcept;
  [[nodiscard]] const QString& dxSpotsStatus() const noexcept;
  [[nodiscard]] bool dxClusterConnected() const noexcept;
  [[nodiscard]] QString dxClusterStatus() const;
  void setAveragingFrames(int value);
  void setSpectrumProcessing(bool dc_rejection, bool automatic_gain,
                             double gain_db,
                             double automatic_gain_target_dbfs,
                             bool automatic_bandwidth,
                             double lower_frequency_hz,
                             double upper_frequency_hz,
                             int frame_rate_hz);
  void setSourceMode(int value);
  void setDecodedSignalTimeoutSeconds(int seconds);
  [[nodiscard]] bool decodeWeakSignals() const noexcept;
  [[nodiscard]] double minimumDecodeSnrDb() const noexcept;
  // The two values travel together because the decoder applies them together:
  // a threshold means nothing without knowing whether it is in force, and
  // sending them separately would leave a moment where the bank is gating on
  // one operator's choice and the other's number.
  void setWeakSignalDecoding(bool enabled, double minimum_decode_snr_db);
  void setOwnCallsign(const QString& callsign);
  Q_INVOKABLE void setKeyingModel(const QString& model);
  Q_INVOKABLE void setOperatorRole(const QString& role);
  [[nodiscard]] const QString& keyingModel() const noexcept;
  void configureLocalCharacterDecoder(bool enabled, const QString& model_path,
                                      const QString& metadata_path);
  void configureOfflineCallsignDatabase(bool enabled,
                                        const QString& database_path);
  // The operator's whole DX spot preference in one call, because the pieces
  // are only meaningful together: a node with no callsign to log in as
  // contacts nobody, a callsign with no node chosen goes nowhere, and a
  // retention window means nothing without a feed to fill it. The telnet
  // cluster is the only spot source, so this is the only configure call the
  // feature has. server_index is an index into the loaded server list, or -1
  // for the operator's own host and port.
  //
  // login_callsign is the station callsign. It is the only thing this
  // application ever writes to a cluster, and an unusable one leaves the link
  // off rather than connecting anonymously, which no cluster permits.
  //
  // retention_minutes and tolerance_hz size the spot store. Presentation-only
  // preferences such as whether labels are drawn stay in the settings object
  // and never reach here.
  Q_INVOKABLE void configureDxCluster(bool enabled, int server_index,
                                      const QString& custom_host,
                                      int custom_port,
                                      const QString& login_callsign,
                                      int retention_minutes, int tolerance_hz);
  // display_name is what the operator reads; device_name is the operating
  // system's own description of the device, which is what an input can be
  // found by when its identifier has changed underneath it.
  void setAudioInputSelection(QString encoded_id, QString display_name,
                              QString device_name);
  void setSdrInputSelection(QString device_id, QString display_name,
                            qulonglong center_frequency_hz,
                            int sample_rate_hz, int bandwidth_hz,
                            QString antenna,
                            bool automatic_gain,
                            double gain_db, qulonglong decoder_center_frequency_hz,
                            int decoder_bandwidth_hz);
  void setRadioFrequencyContext(bool available, qulonglong rx_rf_hz,
                                qulonglong tx_rf_hz, bool split_active,
                                int sideband_index,
                                double reference_tone_hz);
  Q_INVOKABLE void setMonitorMode(int mode);
  Q_INVOKABLE void setMonitorLevel(double level);
  Q_INVOKABLE bool isMonitorChannelEnabled(
      qulonglong channel_id) const noexcept;
  Q_INVOKABLE void toggleMonitorChannel(qulonglong channel_id);
  Q_INVOKABLE double rfFrequencyToDisplayHz(
      qulonglong rf_frequency_hz) const noexcept;
  Q_INVOKABLE qulonglong displayFrequencyToRfHz(
      double display_frequency_hz) const noexcept;
  void setMonitorOutputSelection(QString encoded_device_id);
  // Whether receive audio is currently being sent to a remote observer. Set by
  // the application from the diagnostics service, which is the only thing that
  // knows; relayed to the DSP worker so region audio is demodulated when
  // somebody is listening and not otherwise. The controller neither owns nor
  // interprets the subscription -- it is a wire, exactly as it is for the
  // diagnostics records.
  void setRemoteAudioSubscribed(bool subscribed);

  Q_INVOKABLE void openFile(const QUrl& url);
  Q_INVOKABLE void play();
  Q_INVOKABLE void pause();
  Q_INVOKABLE void stop();
  Q_INVOKABLE void startLiveAudio();
  Q_INVOKABLE void startLiveSdr();
  Q_INVOKABLE void stopLiveAudio();
  Q_INVOKABLE void openDecoderSession(qulonglong channel_id);
  Q_INVOKABLE void openManualDecoderSession(double audio_frequency_hz);
  Q_INVOKABLE void closeDecoderSession(qulonglong channel_id);
  Q_INVOKABLE void moveDecoderSession(qulonglong channel_id, int new_index);
  // Operator-started, bounded diagnostic capture (OBS-003). Only available
  // while live audio is running; writes raw audio plus periodic per-track
  // private diagnostic snapshots to a timestamped folder under the app's
  // standard data location, capped at 5 minutes.
  // Opens the folder a capture was written to in the operator's file manager.
  // A capture is only useful once it has been found, reviewed and attached to a
  // report, and a path shown as text still has to be copied out by hand.
  Q_INVOKABLE void openDebugCaptureFolder();
  Q_INVOKABLE void setDebugCaptureMaximumSeconds(int seconds);
  Q_INVOKABLE void startDebugCapture();
  Q_INVOKABLE void stopDebugCapture();

 signals:
  void stateChanged();
  void sourceReset();
  void frameReady(const cwassistant::desktop::SpectrumFrame& frame);
  void averagingFramesChanged();
  void decoderChanged();
  void monitorChanged();
  void dxSpotsChanged();
  void dxClusterStateChanged();

  void openRequested(const QString& path);
  void playRequested();
  void pauseRequested();
  void stopRequested();
  void configureRequested(int averaging_frames, int frame_rate_hz,
                          bool dc_rejection,
                          bool automatic_gain, double gain_db,
                          double automatic_gain_target_dbfs,
                          bool automatic_bandwidth,
                          double lower_frequency_hz,
                          double upper_frequency_hz);
  // The device name rides with the identifier because QAudioDevice::id() does
  // not survive a restart, a driver reload or a different USB port, and the
  // name is the only other thing the application knows about the operator's
  // choice. See AudioInputOutcome in replay/live_audio_worker.hpp.
  void liveStartRequested(const QString& encoded_device_id,
                          const QString& device_name);
  // An input was found by name after its saved identifier vanished. Settings
  // listens so the recovered identifier is written back and the next start is
  // an exact-identifier match again.
  void audioInputRecovered(const QString& adopted_id);
  void liveStopRequested();
  void sdrStartRequested(const QString& device_id,
                         double center_frequency_hz,
                         double sample_rate_hz, double bandwidth_hz,
                         const QString& antenna,
                         bool automatic_gain,
                         double gain_db);
  void sdrStopRequested();
  void sdrRetuneRequested(double center_frequency_hz);
  void liveDspStartRequested();
  void liveDspStopRequested();
  void liveDspConfigureRequested(int averaging_frames, int frame_rate_hz,
                                 bool dc_rejection,
                                 bool automatic_gain, double gain_db,
                                 double automatic_gain_target_dbfs,
                                 bool automatic_bandwidth,
                                 double lower_frequency_hz,
                                 double upper_frequency_hz);
  void decodedSignalTimeoutRequested(int seconds);
  void ownCallsignRequested(const QString& callsign);
  void liveOwnCallsignRequested(const QString& callsign);
  void keyingModelRequested(const QString& model);
  void liveDebugCaptureMaximumSecondsRequested(double seconds);
  void operatorRoleRequested(const QString& role);
  void liveOperatorRoleRequested(const QString& role);
  void liveKeyingModelRequested(const QString& model);
  void keyingModelChanged();
  void liveDecodedSignalTimeoutRequested(int seconds);
  void weakSignalDecodingRequested(bool enabled,
                                   double minimum_decode_snr_db);
  void liveWeakSignalDecodingRequested(bool enabled,
                                       double minimum_decode_snr_db);
  void weakSignalDecodingChanged();
  void liveFrequencyShiftRequested(double audio_hz_delta);
  void manualDecoderFrequencyRequested(double audio_frequency_hz);
  void liveManualDecoderFrequencyRequested(double audio_frequency_hz);
  void liveRadioFrequencyContextRequested(bool available, qulonglong rx_rf_hz,
                                          qulonglong tx_rf_hz,
                                          bool split_active);
  // Receive-only front-end description forwarded to the DSP worker so a debug
  // capture records the gain state that produced its samples.
  void liveSdrCaptureContextRequested(const QString& receiver_label,
                                      const QString& antenna,
                                      bool automatic_gain, double gain_db);
  void debugCaptureChanged();
  // Relayed straight out of the live DSP worker. The controller is a wire
  // here and nothing more: it does not own the diagnostics service, does not
  // know what addresses it is bound to, and does not know whether anything is
  // listening. The application decides where these records go.
  void diagnosticsRecordProduced(const QJsonObject& record);
  // The decode region as real audio, relayed out of the live DSP worker on the
  // worker's own thread. DIRECT for the same reason the diagnostics record is:
  // a queued relay would route every audio buffer through the GUI thread, so
  // the thread that draws would gate the stream a remote listener hears, and
  // the sender at the far end of this signal already bounds itself. Adding a
  // queue in front of a bounded sender is how an unbounded one is built by
  // accident.
  void receiveAudioProduced(const QByteArray& float_mono_audio,
                            double sample_rate_hz);
  void liveRemoteAudioSubscribedRequested(bool subscribed);
  void radioFrequencyChanged();
  void liveDebugCaptureStartRequested(const QString& directory_path);
  void liveDebugCaptureStopRequested();
  void livePresentationDiagnosticsRequested(const QVariantMap& diagnostics);
  // One tick of this thread's heartbeat and how late it was, on its way to the
  // DSP worker that builds the diagnostics record. See
  // `gui_heartbeat_timer_` for why the measurement has to be taken here.
  void liveGuiHeartbeatRequested(double lateness_ms);
  void localCharacterDecoderConfigureRequested(bool enabled,
                                               const QString& model_path,
                                               const QString& metadata_path);
  void replayCharacterFrontendEnabledRequested(bool enabled);
  void liveCharacterFrontendEnabledRequested(bool enabled);
  void monitorConfigureRequested(int mode, const QVariantList& channel_ids,
                                 double reference_tone_hz);
  void liveMonitorConfigureRequested(int mode,
                                     const QVariantList& channel_ids,
                                     double reference_tone_hz);
  void liveSdrDecoderWindowRequested(double center_frequency_hz,
                                     double bandwidth_hz);
  void replayCharacterRefinementRequested(qulonglong channel_id,
                                          const QString& stable_text,
                                          qulonglong evidence_timestamp_ns);
  void liveCharacterRefinementRequested(qulonglong channel_id,
                                        const QString& stable_text,
                                        qulonglong evidence_timestamp_ns);
  void localCharacterResetRequested();

 private:
  void setStatus(QString status);
  // Reception stopped, or could not start. The status line gets the message
  // exactly as before -- it stays a useful one-line commentary -- and the full
  // text is kept for the dialog as well.
  //
  // A refused SDR retune deliberately does not come here. It reports something
  // that failed without stopping anything: the receiver is still running on
  // the frequency it was already on, and a dialog the operator has to dismiss
  // is itself an interruption that an error which stopped nothing has not
  // earned.
  void setBlockingError(QString message);
  // Reception was asked to start, or has started. Whatever failed before is
  // no longer what the operator is looking at.
  void clearBlockingError();
  void beginLiveAudioCapture();
  void beginLiveSdrCapture();
  void publishSpectrumConfiguration();
  // Publishes the decoder window the IQ decimator can actually admit.
  //
  // The decimator refuses a block whose requested window does not lie wholly
  // inside the acquired passband, and the overview spectrum is produced before
  // that refusal -- so a stranded window shows a perfectly live waterfall with
  // nothing ever reaching the detector. The stored window is an operator
  // preference that outlives any one receiver setting, so it is re-centred on
  // the capture rather than obeyed blindly.
  void publishSdrDecoderWindow();
  void acceptDecoderChannels(const QVariantList& channels);
  void rebuildDecoderModels();
  void publishLivePresentationDiagnostics(bool force);
  void resetDecoder();
  void publishMonitorConfiguration();
  // The one description of what the monitor is currently doing. There were
  // three copies of this ternary and a fourth mode would have had to be added
  // to each of them.
  [[nodiscard]] QString monitorStatusText() const;
  void writeMonitorAudio(const QByteArray& float_mono_audio,
                         double sample_rate_hz);
  void stopMonitorOutput();
  void ensureDxClusterClient();
  void acceptDxSpots(const std::vector<cwassistant::core::CwSpot>& spots);
  void rebuildDxSpotModel();
  // The frequency the receiver is actually listening on, in hertz, or 0 when
  // that is unknown -- no radio, no SDR, or a recording whose real frequency
  // nothing here can know.
  [[nodiscard]] qulonglong receiveRfHz() const noexcept;
  // Whether a spot is worth storing at all, given where the receiver is
  // listening. See the comment on the definition: the reverse-beacon feed is a
  // firehose and this is the only place that knows the receive frequency.
  [[nodiscard]] bool isSpotWithinReceivedBand(
      double frequency_hz) const noexcept;
  // Tells the cluster which band the receiver is on, so a node that accepts
  // filter commands stops sending the rest of the planet. Called from every
  // place the receive frequency can move.
  void publishDxClusterBandFilter();
  // The published status line for the spot feed, kept in one place so that
  // every path that can change what the cluster is doing reports it the same
  // way.
  void publishDxSpotsStatus();
  // Measures how late this thread's own heartbeat was and reports it to the
  // DSP worker. Called from `gui_heartbeat_timer_` only.
  void publishGuiHeartbeat();

  QThread worker_thread_;
  QObject* worker_{nullptr};
  QThread audio_capture_thread_;
  QObject* audio_capture_worker_{nullptr};
  QObject* sdr_capture_worker_{nullptr};
  QThread audio_dsp_thread_;
  QObject* audio_dsp_worker_{nullptr};
  QThread character_inference_thread_;
  QObject* character_inference_worker_{nullptr};
  QString source_name_;
  QString status_text_{QStringLiteral("Select Start live RX to begin receiving audio")};
  QString blocking_error_;
  bool source_loaded_{false};
  bool playing_{false};
  int source_mode_{0};
  bool live_capturing_{false};
  qulonglong input_overruns_{0};
  QString audio_input_id_;
  QString audio_input_name_{QStringLiteral("System default input")};
  QString audio_input_device_name_;
  // Held so the notice survives started(), which overwrites the status line
  // with the live-RX summary the instant capture begins.
  QString audio_input_recovery_notice_;
  QString sdr_device_id_;
  QString sdr_device_name_{QStringLiteral("No SDR selected")};
  qulonglong sdr_center_frequency_hz_{14'050'000ULL};
  int sdr_sample_rate_hz_{250'000};
  int sdr_bandwidth_hz_{0};
  QString sdr_antenna_;
  qulonglong sdr_decoder_center_frequency_hz_{14'050'000ULL};
  int sdr_decoder_bandwidth_hz_{24'000};
  bool sdr_automatic_gain_{true};
  double sdr_gain_db_{30.0};
  double sample_rate_{0.0};
  double duration_seconds_{0.0};
  double position_seconds_{0.0};
  int averaging_frames_{3};
  bool audio_dc_rejection_{true};
  bool audio_automatic_gain_{false};
  double audio_gain_db_{0.0};
  double audio_automatic_gain_target_dbfs_{-12.0};
  bool audio_automatic_bandwidth_{true};
  double audio_lower_frequency_hz_{100.0};
  double audio_upper_frequency_hz_{3'000.0};
  int spectrum_frame_rate_hz_{60};
  bool spectrum_processing_configured_{false};
  QVariantList decoder_channels_;
  QVariantList raw_decoder_channels_;
  QVariantList decoder_sessions_;
  DecoderSessionListModel decoder_session_model_;
  QList<qulonglong> decoder_session_order_;
  QVariantMap verification_diagnostics_;
  std::unordered_map<std::uint64_t,
                     cwassistant::core::CwCharacterConsensusMerger>
      local_character_consensus_;
  QString local_character_state_{QStringLiteral("disabled")};
  QString local_character_status_{QStringLiteral("Local model disabled.")};
  cwassistant::core::OfflineCallsignDatabase offline_callsign_database_;
  bool callsign_database_correction_enabled_{false};
  QString own_callsign_;
  DecoderChannelPresentationCache channel_presentation_;
  QString keying_model_{QStringLiteral("adaptive-threshold")};
  // Mirrors the operator preference so the current gate can be read back, and
  // so the pair can be resent to a worker whenever the decoder configuration
  // has been replaced wholesale underneath it. The defaults match the core
  // decoder's own: weak-signal decoding off, with a measured 12.0 dB gate.
  bool decode_weak_signals_{false};
  double minimum_decode_snr_db_{12.0};
  QString offline_callsign_database_state_{QStringLiteral("disabled")};
  QString offline_callsign_database_status_{
      QStringLiteral("Offline callsign suggestions disabled.")};
  QVariantMap last_live_presentation_diagnostics_;
  bool debug_capture_active_{false};
  QString debug_capture_path_;
  double debug_capture_elapsed_seconds_{0.0};
  QString debug_capture_note_;
  bool radio_frequency_available_{false};
  qulonglong radio_rx_rf_hz_{0};
  qulonglong radio_tx_rf_hz_{0};
  bool radio_split_active_{false};
  int cw_sideband_index_{0};
  double cw_reference_tone_hz_{700.0};
  int monitor_mode_{0};
  QList<qulonglong> monitored_channel_ids_;
  double monitor_level_{0.65};
  QString monitor_output_device_id_;
  QString monitor_status_{QStringLiteral("Monitor off")};
  std::unique_ptr<QAudioSink> monitor_audio_sink_;
  QIODevice* monitor_audio_device_{nullptr};
  int monitor_audio_sample_rate_{0};
  // This controller lives on the GUI thread, so a timer it owns is delivered
  // by the GUI thread's event loop -- and a timer that cannot be delivered on
  // time is the only direct evidence that loop is blocked. Every other counter
  // in the diagnostics record is taken on the DSP worker's thread and stays
  // healthy while the window refuses to repaint, so without this the stream
  // says nothing about the fault it exists to diagnose.
  //
  // Runs for the controller's whole life rather than only during live
  // reception: a GUI thread already saturated when the operator presses start
  // is exactly the case worth catching, and the heartbeat must already be
  // running to catch it. The worker discards these values until it starts, and
  // ten fires a second cost nothing next to what they measure.
  //
  // `gui_heartbeat_clock_` is restarted on every fire, so it always holds the
  // interval between the last two deliveries; the requested interval
  // subtracted from that is the lateness.
  QTimer gui_heartbeat_timer_;
  QElapsedTimer gui_heartbeat_clock_;
  cwassistant::core::CwSpotRegistry dx_spot_registry_;
  QTimer dx_spot_expiry_timer_;
  QVariantList dx_spots_;
  QString dx_spots_status_;
  int dx_spots_retention_minutes_{15};
  int dx_spots_tolerance_hz_{250};
  // Built only once the operator first joins a cluster, so a station that
  // never uses the feature never opens a socket for it.
  DxClusterClient* dx_cluster_client_{nullptr};
  bool dx_cluster_enabled_{false};
  // Which kind of report the joined node supplies. A reverse-beacon node's
  // spots come from machines and a cluster node's from people, and the two are
  // weighed differently, so the marks a spot carries follow the node it
  // arrived from.
  bool dx_cluster_reverse_beacon_{false};
};

}  // namespace cwassistant::desktop
