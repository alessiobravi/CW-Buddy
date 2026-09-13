#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "../decoder/local_character_decoder.hpp"
#include "../visualization/spectrum_frame.hpp"
#include "cwassistant/core/cw_channel_bank.hpp"
#include "cwassistant/core/iq_receive.hpp"
#include "cwassistant/core/iq_writer.hpp"
#include "cwassistant/core/sample_block.hpp"
#include "cwassistant/core/spectrum_analyzer.hpp"
#include "cwassistant/core/wav_writer.hpp"
#include "live_audio_pipe.hpp"

#include <fstream>

class QAudioSource;
class QIODevice;

namespace cwassistant::desktop {

// One audio input as resolution sees it. Deliberately not a QAudioDevice: the
// outcome is decided entirely from an identifier and a name, and two strings
// can be written down in a test on a machine that has no sound hardware at
// all, which is the only way this logic can be checked at all.
struct AudioInputCandidate {
  QString id;
  QString name;
};

// Why an audio input needs more than its saved identifier to be found again.
//
// QAudioDevice::id() is an opaque handle the operating system hands out; it is
// not a property of the hardware. A reboot, a driver reload or a different USB
// port can all change it while the same interface stays plugged into the same
// radio. A station reported exactly that: the PC restarted and reception
// refused to start, because the one fact remembered about the chosen input was
// the one fact that does not survive a restart.
//
// The name does survive, but it is not unique -- that same station had two
// interfaces the operating system describes with identical words. So the name
// can rescue the common case and must not be allowed to decide the ambiguous
// one: silently opening the wrong sound card would decode a different radio
// with nothing on screen to say so, which is worse than refusing to start.
enum class AudioInputOutcome {
  // Nothing was ever chosen. Whatever the system currently calls its default.
  SystemDefault,
  // The saved identifier is still present. The overwhelmingly common path,
  // and the only one that consults nothing else.
  Matched,
  // The identifier is gone and exactly one input carries the saved name.
  // Nothing else could have been meant, so adopt it -- and say so.
  Recovered,
  // The identifier is gone and several inputs carry the saved name. Refuse,
  // name the candidates, and make the operator choose.
  Ambiguous,
  // Neither the identifier nor the name is present.
  Missing,
};

struct AudioInputResolution {
  AudioInputOutcome outcome{AudioInputOutcome::Missing};
  // Index into the candidate list; -1 for SystemDefault, Ambiguous and Missing.
  int index{-1};
  // The identifier to write back to settings, so the next start is a Matched.
  // Non-empty only for Recovered.
  QString adopted_id;
  // Operator-facing sentence. Empty for SystemDefault and Matched, because
  // nothing happened that an operator needs to be told about.
  QString message;
};

// Pure over the device list, so every outcome is reachable from a test.
[[nodiscard]] AudioInputResolution resolveAudioInputSelection(
    const std::vector<AudioInputCandidate>& devices, const QString& requested_id,
    const QString& requested_name);

class LiveAudioCaptureWorker final : public QObject {
  Q_OBJECT

 public:
  explicit LiveAudioCaptureWorker(std::shared_ptr<LiveAudioPipe> pipe,
                                  QObject* parent = nullptr);
  ~LiveAudioCaptureWorker() override;

 public slots:
  // The name travels with the identifier because the identifier alone cannot
  // survive a restart. See AudioInputOutcome above.
  void start(const QString& encoded_device_id, const QString& device_name);
  void stop();

 signals:
  void started(const QString& device_name, double sample_rate,
               int channel_count);
  void stopped();
  void failed(const QString& message);
  // An input was found by name after its saved identifier vanished. Carries
  // the identifier to persist and the sentence the operator must read: an
  // operator who has quietly swapped one interface for another of the same
  // model deserves to know the application followed the name rather than the
  // hardware.
  void inputRecovered(const QString& adopted_id, const QString& message);
  void overrunCountChanged(qulonglong count);

 private slots:
  void consumeAvailableBytes();
  void handleStateChanged();

 private:
  [[nodiscard]] float readSample(const char* data) const noexcept;
  void appendFrame(const char* frame);
  void publishBlock();

  static constexpr std::size_t kPublishedBlockSamples = 2'048;
  static constexpr qsizetype kRawBufferBytes = 65'536;

  std::shared_ptr<LiveAudioPipe> pipe_;
  QAudioSource* source_{nullptr};
  QIODevice* input_{nullptr};
  QAudioFormat format_;
  std::array<char, static_cast<std::size_t>(kRawBufferBytes)> raw_buffer_{};
  qsizetype pending_bytes_{0};
  cwassistant::core::RealtimeSampleBlock block_{};
  std::uint64_t sequence_{0};
  std::uint64_t captured_samples_{0};
  bool stopping_{false};
};

class LiveAudioDspWorker final : public QObject {
  Q_OBJECT

 public:
  explicit LiveAudioDspWorker(std::shared_ptr<LiveAudioPipe> pipe,
                              QObject* parent = nullptr);

  // The heartbeat contract, kept here because the record has to state it: a
  // lateness figure means nothing without the interval it was measured against,
  // and a stall count means nothing without the bar it counted over. The
  // controller owns the timer and reads both from here, so the numbers in the
  // record can never describe a cadence the GUI thread is not actually using.
  //
  // 100 ms is short enough that an operator-visible hitch cannot hide between
  // two fires, and 250 ms of lateness is about where a dropped repaint stops
  // looking like jitter and starts looking like the window is stuck.
  // How many spectrum frames may be in flight to the display at once.
  //
  // Frames were emitted with no backpressure whatever. A wide IQ transform is
  // 8193 bins, and each frame carries two float vectors of them, so one queued
  // frame is about 64 kB and thirty a second is two megabytes a second. If the
  // thread that draws falls even slightly behind, that queue grows without
  // bound -- and the growth makes it fall further behind, which is why an
  // operator saw memory climb from 244 MB to 668 MB and then had to kill the
  // application. 424 MB is a little over three minutes of exactly this.
  //
  // Four. A display frame nobody drew is worth nothing -- there is no history
  // to preserve in one superseded before it reached the screen -- so dropping
  // is the correct answer rather than a compromise, and any small bound ends
  // the unbounded growth equally well. Two proved tighter than the producer's
  // own burstiness: the analyser can emit several frames from one drain, so
  // frames were refused during ordinary operation and not only under load,
  // which cost the waterfall continuity for no gain. Four is 256 kB at worst.
  static constexpr int kMaximumSpectrumFramesInFlight = 4;
  // Region audio crosses to the thread that plays, and that thread can stall.
  // Eight buffers is roughly a fifth of a second at the drain cadence -- ample
  // slack for ordinary jitter, and a hard ceiling on the backlog a stalled
  // player can build. Audio refused here is dropped rather than queued,
  // because a monitor buffer delivered late is worse than one never delivered:
  // it plays the past.
  static constexpr int kMaximumMonitorBuffersInFlight = 8;
  static constexpr int kGuiHeartbeatIntervalMs = 100;
  static constexpr double kGuiStallLatenessMs = 250.0;

 public slots:
  void start();
  void stop();
  void configure(int averaging_frames, int frame_rate_hz, bool dc_rejection,
                 bool automatic_gain, double gain_db,
                 double automatic_gain_target_dbfs, bool automatic_bandwidth,
                 double lower_frequency_hz, double upper_frequency_hz);
  void setOwnCallsign(const QString& callsign);
  void setKeyingModel(const QString& model);
  void setDebugCaptureMaximumSeconds(double seconds);
  void setOperatorRole(const QString& role);
  void setDecodedSignalTimeoutSeconds(int seconds);
  // Chooses which tracked signals are decoded, and from what level. It never
  // touches the audio the detector receives, so it deliberately does not reset
  // the decoder: doing so would discard every track, transcript and confirmed
  // callsign the operator currently has open in exchange for a preference the
  // channel bank honours from its very next update.
  void setWeakSignalDecoding(bool enabled, double minimum_decode_snr_db);
  void setLocalCharacterFrontendEnabled(bool enabled);
  void setMonitor(int mode, const QVariantList& channel_ids,
                  double reference_tone_hz);
  // Whether anybody is being sent receive audio over the network right now.
  // Not whether the operator ALLOWED it: a permission nobody is using must
  // cost this worker nothing, and demodulating the region for an audience of
  // zero is exactly the "work at the rate data arrives rather than the rate a
  // consumer needs" fault that has cost this application dearly before.
  void setRemoteAudioSubscribed(bool subscribed);
  // Called by the thread that plays, once it has taken a monitor buffer off the
  // queue. Mirrors noteSpectrumFrameConsumed(): without it the region audio's
  // in-flight count only ever rises.
  void noteMonitorAudioConsumed() noexcept;
  void setSdrDecoderWindow(double center_frequency_hz, double bandwidth_hz);
  // Called by the thread that draws, once it has taken a frame off the queue,
  // so this worker knows whether the display is keeping up.
  void noteSpectrumFrameConsumed() noexcept;
  void acceptCharacterRefinement(qulonglong channel_id,
                                 const QString& stable_text,
                                 qulonglong evidence_timestamp_ns);
  // Re-centers every currently tracked signal by a known audio-domain shift
  // (the shift implied by an operator retuning the linked radio's RX VFO),
  // so an already-identified signal's tracking follows the retune instead
  // of being lost and re-acquired from scratch.
  void shiftTrackedFrequencies(double audio_hz_delta);
  void selectDecoderFrequency(double audio_frequency_hz);
  // Mirrors the radio frequency context ReplayController tracks, purely so
  // a debug capture snapshot can record whether/how the RX (and TX, if
  // split) dial frequency moved during the capture window.
  void setRadioFrequencyContext(bool available, qulonglong rx_rf_hz,
                                qulonglong tx_rf_hz, bool split_active);
  // Receive-only description of the SDR front end, mirrored here purely so a
  // debug capture can record the gain state alongside the samples. A recording
  // without it documents the symptom and not the cause: an overloaded front
  // end and a starved one look very different in the same spectrum, and the
  // application has no other overload indicator.
  void setSdrCaptureContext(const QString& receiver_label,
                            const QString& antenna, bool automatic_gain,
                            double gain_db);
  void setPresentationDiagnostics(const QVariantMap& diagnostics);
  // One tick of the GUI thread's heartbeat, carrying how late that tick was:
  // the elapsed time since the previous tick minus the interval that was
  // requested. Measured on the GUI thread, because only a timer running there
  // can observe that thread being blocked; reported here, because this worker
  // is what builds the record.
  //
  // Every counter this worker keeps measures this worker. A saturated GUI
  // thread -- the fault this stream exists to diagnose, reported as an
  // application that fatigues and a spectrum that turns snappy while the
  // processor sits idle -- leaves all of them healthy, because the decoder
  // genuinely is. Lateness is the one number that moves when the thread that
  // draws cannot get back to its event loop.
  //
  // Lateness rather than a frame rate: a frame rate needs a renderer to
  // cooperate and tells you only that frames stopped, while lateness is
  // measured by the blocked thread itself and its magnitude *is* the length of
  // the block. If the GUI thread seizes for three seconds, no heartbeat can be
  // delivered during it and the first one afterwards reports three seconds of
  // lateness -- the silence and the spike together are the diagnosis, and
  // sinceLastHeartbeatMs in the record makes the silence itself visible while
  // it is still happening.
  void acceptGuiHeartbeat(double lateness_ms);
  // Operator-started, bounded diagnostic capture (OBS-003): records the raw
  // audio feeding the decoder plus periodic per-track private diagnostic
  // snapshots to help debug why a visible signal is not decoding. Never
  // starts implicitly; always bounded in duration.
  void startDebugCapture(const QString& directory_path);
  void stopDebugCapture();

 signals:
  void frameProduced(const cwassistant::desktop::SpectrumFrame& frame);
  void decoderProduced(const QVariantList& channels);
  void diagnosticsProduced(const QVariantMap& diagnostics);
  void manualDecoderSelected(qulonglong channel_id);
  void debugCaptureStateChanged(bool active, const QString& base_path,
                                double elapsed_seconds, const QString& note);
  void characterWindowProduced(
      int source_mode,
      cwassistant::desktop::CwCharacterFeatureWindowPtr window);
  void monitorAudioProduced(const QByteArray& float_mono_audio,
                            double sample_rate_hz);
  // The decode region as real audio, for consumers outside the loudspeaker
  // path. Exactly the same samples the local monitor plays in region mode --
  // one demodulator feeds the monitor, the remote stream and the debug capture
  // -- so what a remote listener hears and what a capture records is what the
  // operator heard, and there is never a second region audio path to keep in
  // agreement with the first.
  void receiveAudioProduced(const QByteArray& float_mono_audio,
                            double sample_rate_hz);
  // The same diagnostics record the debug capture writes to disk, offered as
  // it is produced so a running station can be watched instead of recorded,
  // stopped and sent. Emitted on its own slow timer for the whole time live
  // reception is running, whether or not a capture is recording.
  void diagnosticsRecordProduced(const QJsonObject& record);

 private slots:
  void drain();
  void publishLiveDiagnosticsRecord();

 private:
  // The one place a spectrum frame reaches the display, so the in-flight bound
  // cannot be bypassed by a new emit site. Not a slot: it takes an rvalue, and
  // it is this class's own discipline rather than anything a caller invokes.
  // Returns false when the frame was dropped because the display has not kept
  // up.
  [[nodiscard]] bool publishSpectrumFrame(SpectrumFrame&& frame);
  void captureBlock(const cwassistant::core::RealtimeSampleBlock& block);
  // Demodulates the decode region to audio and hands it to whichever of the
  // three consumers asked for it, or does nothing whatever if none did.
  void produceRegionAudio(const cwassistant::core::RealtimeSampleBlock& block);
  // Whether anything currently wants region audio. Read before a single sample
  // is touched, so a station with the monitor off, no observer and no capture
  // running pays one boolean test per drained block for this feature.
  [[nodiscard]] bool regionAudioWanted() const noexcept;
  // Drops the demodulator's oscillator phase and delay line. Called wherever
  // the region samples stop being continuous with the ones before them.
  void resetRegionAudio() noexcept;
  // Writes region audio into the capture's audio.wav BESIDE the IQ recording.
  // A SigMF capture used to contain no listenable audio at all, because the
  // capture wrote either audio or IQ and an SDR session wrote IQ -- so an
  // operator reviewing a recording of a signal that would not decode had
  // nothing to listen to.
  void captureRegionAudio(const std::vector<float>& audio,
                          double sample_rate_hz);
  // The one place a monitor buffer leaves this worker, so the in-flight bound
  // below cannot be bypassed by a new emit site.
  void emitMonitorAudio(const QByteArray& audio, double sample_rate_hz,
                        bool bounded);
  [[nodiscard]] bool openIqCapture(
      const cwassistant::core::RealtimeSampleBlock& block);
  // One throughput measurement window: the counter values at the moment the
  // rates were last read, and the slowest drain seen since.
  //
  // Two of these exist -- one for the debug capture file, one for the live
  // stream -- because a rate is only meaningful against the moment it was
  // last read. A live snapshot that advanced the capture's baseline would
  // leave the rates in diagnostics.jsonl measured over a window nobody chose,
  // so a capture's numbers would change depending on whether anyone happened
  // to be watching the stream. A diagnostic that reads differently because it
  // is being observed is worse than no diagnostic.
  //
  // The peak needs its own copy per window rather than a shared counter: a
  // maximum is not a difference of totals, so once one reader clears it the
  // other cannot recover it.
  //
  // `opened_ns` is in whichever clock its reader uses -- the sample clock for
  // the capture (block timestamps), this worker's monotonic clock for the
  // stream. The two are never compared with each other.
  struct DiagnosticsWindow {
    std::uint64_t opened_ns{0};
    std::uint64_t drain_calls{0};
    std::uint64_t blocks{0};
    std::uint64_t publishes{0};
    std::uint64_t capped{0};
    std::uint64_t micros{0};
    std::uint64_t peak_micros{0};
    // The GUI heartbeat's numbers are scoped exactly like the drain's, and for
    // the same reason: the stall count is a baseline to subtract, while the
    // peak is a maximum and therefore needs its own copy per window. Sharing
    // one peak would let whichever reader ran first clear the worst lateness
    // before the other ever saw it.
    std::uint64_t gui_stalls{0};
    double gui_peak_lateness_ms{0.0};
  };

  // Builds the diagnostics record, and nothing else: the capture path writes
  // what this returns to diagnostics.jsonl and the live timer publishes it.
  // `window` is the caller's own measurement window, read and then re-opened
  // at `now_ns`; `elapsed_seconds` is what the record means by elapsed --
  // time since the capture began for a capture snapshot, time since live
  // reception started for a stream record, because a stream record stamped
  // with a finished capture's elapsed time would be a lie.
  [[nodiscard]] QJsonObject buildDiagnosticsRecord(DiagnosticsWindow& window,
                                                   std::uint64_t now_ns,
                                                   double elapsed_seconds);
  void writeDebugCaptureSnapshot();
  void finishDebugCapture(const QString& note);
  // Puts the requested decoder window into force: configures the channelizer
  // and restarts everything downstream of it that the old slice had state in.
  //
  // Split out from setSdrDecoderWindow because a request and an applied
  // configuration are not the same thing, and conflating them is what left a
  // freshly started SDR drawing its decoder window over a spectrum that
  // decoded nothing. The window is published before the first IQ block
  // arrives, so at that moment there is no complex stream to configure for;
  // the request has to be remembered and then applied by the block that makes
  // it meaningful. Only `drain()` and `setSdrDecoderWindow` call this, and only
  // while complex IQ is arriving.
  void applySdrDecoderWindow();
  // Whether the requested window still differs from what the channelizer was
  // last configured with. False once applied, so an unchanged republication
  // cannot tear down a working decode.
  [[nodiscard]] bool sdrDecoderWindowNeedsApply() const noexcept;

  std::shared_ptr<LiveAudioPipe> pipe_;
  QTimer timer_;
  cwassistant::core::SpectrumAnalyzer analyzer_;
  // Recorded in the diagnostics so a report of "decodes nothing" can be told
  // apart from a detector that was never given a spectrum frame.
  std::uint64_t detector_frames_{0};
  std::uint64_t decoder_resets_{0};
  cwassistant::core::SpectrumAnalyzer decoder_analyzer_{
      {.fft_size = 8'192, .averaging_frames = 3, .frame_rate_hz = 60}};
  cwassistant::core::IqSubbandDecimator sdr_decoder_channelizer_;
  cwassistant::core::RealtimeSampleBlock sdr_decoder_pending_;
  std::uint64_t sdr_decoder_pending_sequence_{0};
  // Whether the samples currently arriving are complex IQ.
  //
  // The decoder window is an SDR setting, but the settings that carry it are
  // republished whenever anything in the SDR page changes -- including while
  // the operator is receiving from an audio card. Touching the shared decoder
  // from there tore down a perfectly good audio decode for a receiver that was
  // not even running.
  // The decoded-channel model was published once per drained block, and the
  // drain timer runs every five milliseconds over as many as thirty-two
  // blocks. Each publication deep-copies the whole model across a thread
  // boundary and the receiving thread rebuilds it again, so a handful of
  // simultaneous signals buried the thread that draws -- the application
  // stopped responding while the processor sat largely idle, because the work
  // was all on one thread. The model is a snapshot: publishing the latest one
  // at a rate an operator can actually see loses nothing.
  // Throughput counters, published with every capture snapshot.
  //
  // The existing diagnostics describe what the decoder FOUND. They say nothing
  // about whether the application is keeping up, so an operator reporting that
  // it had stopped responding while several signals decoded left no number to
  // look at -- and the cause, the decoded-channel model being published once
  // per drained block on a five-millisecond timer, was invisible in them. A
  // drain that repeatedly hits its own block cap is the signal that samples
  // are arriving faster than they are being consumed.
  std::uint64_t drain_calls_{0};
  std::uint64_t drain_capped_{0};
  std::uint64_t blocks_drained_{0};
  std::uint64_t model_publishes_{0};
  std::uint64_t drain_micros_total_{0};
  // The GUI thread's lateness, as last reported by its heartbeat, plus the
  // running total of heartbeats that exceeded kGuiStallLatenessMs. Cumulative
  // like the drain counters, so each reader subtracts its own baseline.
  //
  // `gui_heartbeat_ns_` is on this worker's monotonic clock (`live_clock_`) and
  // is when the last heartbeat *arrived here*, not when it was measured. Zero
  // means none has arrived since live reception started, which is why the
  // record reports the gap against that clock: with no heartbeat ever seen the
  // gap is the whole session, which is what a GUI thread blocked from the
  // outset would actually look like.
  double gui_lateness_ms_{0.0};
  std::uint64_t gui_stalls_{0};
  std::uint64_t gui_heartbeat_ns_{0};
  DiagnosticsWindow capture_diagnostics_window_;
  DiagnosticsWindow live_diagnostics_window_;
  static constexpr qint64 kModelPublishIntervalMs = 40;
  static constexpr qint64 kDiagnosticsPublishIntervalMs = 500;
  // Once a second, because the diagnostics record is for a human reading a
  // stream. Deliberately not driven from drain(): that timer runs every five
  // milliseconds, and publishing a deep-copied model on it is precisely the
  // fault that buried the thread that draws.
  static constexpr int kLiveDiagnosticsIntervalMs = 1'000;
  QTimer live_diagnostics_timer_;
  // Stamps the live records, and measures their window. Runs for exactly as
  // long as live reception does, so an idle worker publishes nothing.
  QElapsedTimer live_clock_;
  QElapsedTimer model_publish_clock_;
  QElapsedTimer diagnostics_publish_clock_;
  bool decoder_model_dirty_{false};
  // Written by this worker's thread and by the thread that draws, so it is
  // atomic rather than guarded: the only operations are an increment, a
  // decrement and a bounds test.
  std::atomic<int> spectrum_frames_in_flight_{0};
  std::uint64_t dropped_spectrum_frames_{0};
  std::uint64_t dropped_since_delivered_{0};
  bool processing_complex_iq_{false};
  // The window the operator has asked for. Recorded unconditionally, whatever
  // is currently running: these settings are republished whenever anything on
  // the SDR page changes, including while an audio card is the source.
  double sdr_decoder_center_frequency_hz_{14'050'000.0};
  double sdr_decoder_bandwidth_hz_{24'000.0};
  // The window the channelizer is actually configured with, which is a
  // different fact. It is only ever written by applySdrDecoderWindow(), and
  // `sdr_decoder_window_applied_` starts false because at that point the
  // channelizer holds its own defaults and not these values -- comparing
  // against the requested pair alone would claim a window was in force that
  // the channelizer had never been told about, which is exactly how a started
  // SDR ended up refusing every block.
  bool sdr_decoder_window_applied_{false};
  double applied_sdr_decoder_center_frequency_hz_{0.0};
  double applied_sdr_decoder_bandwidth_hz_{0.0};
  std::optional<double> pending_manual_frequency_hz_;
  cwassistant::core::CwChannelBank decoder_;
  LocalCharacterFrontendBank character_frontends_;

  cwassistant::core::WavWriter capture_writer_;
  // Complex IQ needs its own recorder: WavWriter is hard-wired to mono PCM16
  // and keeps only the real component, which throws away the sideband
  // distinction that is the entire point of recording a complex receiver.
  cwassistant::core::IqWriter capture_iq_writer_;
  std::ofstream capture_diagnostics_log_;
  QString capture_base_path_;
  QString capture_wav_path_;
  QString capture_iq_path_;
  // ci16_le is lossless for both supported receivers (the RSPduo is 14-bit,
  // the RTL-SDR 8-bit) and halves the file against cf32_le, which at
  // megasample rates decides whether a capture is usable at all.
  cwassistant::core::IqSampleFormat capture_iq_format_{
      cwassistant::core::IqSampleFormat::Ci16Le};
  double capture_iq_sample_rate_hz_{0.0};
  double capture_iq_center_frequency_hz_{0.0};
  QString sdr_receiver_label_;
  QString sdr_antenna_;
  bool sdr_gain_state_known_{false};
  bool sdr_automatic_gain_{false};
  double sdr_gain_db_{0.0};
  std::uint64_t capture_start_ns_{0};
  std::uint64_t capture_last_snapshot_ns_{0};
  std::size_t capture_existing_track_count_{0};
  std::size_t capture_existing_published_count_{0};
  bool capture_active_{false};
  bool capture_writer_pending_{false};
  bool capture_have_start_{false};
  bool radio_frequency_available_{false};
  qulonglong radio_rx_rf_hz_{0};
  qulonglong radio_tx_rf_hz_{0};
  bool radio_split_active_{false};
  int monitor_mode_{0};
  // Region audio: the whole decode region demodulated to real audio, produced
  // once and shared by the local monitor, the remote stream and the capture.
  cwassistant::core::IqRegionDemodulator region_demodulator_;
  // Reused between drains so steady-state production allocates nothing.
  std::vector<float> region_audio_;
  bool region_audio_active_{false};
  bool remote_audio_subscribed_{false};
  std::atomic<int> monitor_buffers_in_flight_{0};
  std::uint64_t dropped_region_audio_buffers_{0};
  // How many region-audio samples this worker has demodulated, cumulative for
  // the session. In the diagnostics record because it is the one number that
  // distinguishes "nobody is listening" from "the demodulator is running and
  // the audio is being thrown away" -- and because a feature whose whole cost
  // discipline is "produce nothing when nobody wants it" needs that claim to
  // be observable rather than asserted.
  std::uint64_t region_audio_samples_{0};
  QString capture_region_audio_path_;
  // Set once when the capture's audio file cannot be opened, so a failure is
  // reported once instead of retried on every block. It never ends the
  // capture: the IQ is the primary payload and losing a forensic recording
  // because a second file would not open is a worse outcome than losing the
  // audio beside it.
  bool capture_region_audio_failed_{false};
  double monitor_resample_phase_{0.0};
  double monitor_resample_input_rate_hz_{0.0};
  float monitor_resample_sum_{0.0F};
  std::size_t monitor_resample_count_{0};
  QVariantMap presentation_diagnostics_;
  // How long a debug capture runs before stopping itself. Configurable because
  // a signal that only misbehaves occasionally cannot be caught inside a fixed
  // five minutes, while a quick reproduction should not leave the operator with
  // a needlessly large audio file to review before sharing it.
  static constexpr double kDefaultMaximumCaptureSeconds = 300.0;
  double maximum_capture_seconds_{kDefaultMaximumCaptureSeconds};
  // A wide IQ recording dwarfs an audio one: at 8 MS/s the ci16 stream is
  // 32 MB/s, so the operator's duration setting on its own is not a usable
  // bound. 4 GiB is roughly 134 seconds at that rate -- a generous forensic
  // window -- and is also the largest single file a FAT32 removable drive
  // accepts, which is where these recordings usually end up.
  static constexpr std::uint64_t kMaximumIqCaptureBytes =
      4ULL * 1024ULL * 1024ULL * 1024ULL;
  static constexpr double kSnapshotIntervalSeconds = 1.0;
};

}  // namespace cwassistant::desktop
