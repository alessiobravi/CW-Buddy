#include "live_audio_worker.hpp"

#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/cw_morse_alphabet.hpp"
#include "cwassistant/core/cw_vocabulary.hpp"
#include "decoder_channel_model.hpp"

#include <QAudioDevice>
#include <QAudioSource>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace cwassistant::desktop {
namespace {

QString encoded_device_id(const QAudioDevice& device) {
  return QString::fromLatin1(device.id().toBase64(
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QAudioDevice resolve_input(const QString& requested_id) {
  if (requested_id.isEmpty()) {
    return QMediaDevices::defaultAudioInput();
  }
  for (const auto& device : QMediaDevices::audioInputs()) {
    if (encoded_device_id(device) == requested_id) {
      return device;
    }
  }
  return {};
}

QAudioFormat capture_format(const QAudioDevice& device) {
  QAudioFormat requested;
  requested.setSampleRate(48'000);
  requested.setChannelCount(1);
  requested.setSampleFormat(QAudioFormat::Float);
  if (device.isFormatSupported(requested)) {
    return requested;
  }
  return device.preferredFormat();
}

QByteArray monitor_bytes(const std::vector<float>& samples) {
  if (samples.empty()) return {};
  return QByteArray(reinterpret_cast<const char*>(samples.data()),
                    static_cast<qsizetype>(samples.size() * sizeof(float)));
}

}  // namespace

LiveAudioCaptureWorker::LiveAudioCaptureWorker(
    std::shared_ptr<LiveAudioPipe> pipe, QObject* parent)
    : QObject(parent), pipe_(std::move(pipe)) {}

LiveAudioCaptureWorker::~LiveAudioCaptureWorker() { stop(); }

void LiveAudioCaptureWorker::start(const QString& encoded_device_id_value) {
  stop();
  stopping_ = false;
  const QAudioDevice device = resolve_input(encoded_device_id_value);
  if (device.isNull()) {
    emit failed(
        QStringLiteral("The selected audio input is unavailable. "
                       "Reconnect it or select another input."));
    return;
  }

  format_ = capture_format(device);
  if (!format_.isValid() || format_.channelCount() < 1 ||
      format_.bytesPerFrame() < 1 ||
      format_.sampleFormat() == QAudioFormat::Unknown) {
    emit failed(QStringLiteral(
        "The selected audio input has no supported PCM format."));
    return;
  }

  pipe_->overruns.store(0, std::memory_order_release);
  block_ = {};
  block_.stream = {
      .kind = cwassistant::core::StreamKind::Audio,
      .sample_rate_hz = static_cast<double>(format_.sampleRate()),
      .center_frequency_hz = 0.0,
      .channel_count = 1,
  };
  sequence_ = 0;
  captured_samples_ = 0;
  pending_bytes_ = 0;

  source_ = new QAudioSource(device, format_, this);
  source_->setBufferSize(
      std::max(format_.bytesPerFrame() * format_.sampleRate() / 10, 4'096));
  connect(source_, &QAudioSource::stateChanged, this,
          &LiveAudioCaptureWorker::handleStateChanged);
  input_ = source_->start();
  if (input_ == nullptr) {
    const QString message =
        QStringLiteral("The operating system could not start the audio input.");
    delete source_;
    source_ = nullptr;
    emit failed(message);
    return;
  }
  connect(input_, &QIODevice::readyRead, this,
          &LiveAudioCaptureWorker::consumeAvailableBytes);
  emit started(device.description(), static_cast<double>(format_.sampleRate()),
               format_.channelCount());
}

void LiveAudioCaptureWorker::stop() {
  stopping_ = true;
  input_ = nullptr;
  if (source_ != nullptr) {
    source_->stop();
    delete source_;
    source_ = nullptr;
    emit stopped();
  }
  pending_bytes_ = 0;
  block_ = {};
}

float LiveAudioCaptureWorker::readSample(const char* data) const noexcept {
  switch (format_.sampleFormat()) {
    case QAudioFormat::UInt8:
      return (static_cast<float>(
                  *reinterpret_cast<const unsigned char*>(data)) -
              128.0F) /
             128.0F;
    case QAudioFormat::Int16: {
      std::int16_t value{};
      std::memcpy(&value, data, sizeof(value));
      return static_cast<float>(value) / 32'768.0F;
    }
    case QAudioFormat::Int32: {
      std::int32_t value{};
      std::memcpy(&value, data, sizeof(value));
      return static_cast<float>(static_cast<double>(value) / 2'147'483'648.0);
    }
    case QAudioFormat::Float: {
      float value{};
      std::memcpy(&value, data, sizeof(value));
      return std::clamp(value, -1.0F, 1.0F);
    }
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats: break;
  }
  return 0.0F;
}

void LiveAudioCaptureWorker::appendFrame(const char* frame) {
  const int bytes_per_sample = format_.bytesPerSample();
  float mono = 0.0F;
  for (int channel = 0; channel < format_.channelCount(); ++channel) {
    mono += readSample(frame + channel * bytes_per_sample);
  }
  mono /= static_cast<float>(format_.channelCount());

  if (block_.sample_count == 0) {
    block_.timestamp_ns = static_cast<std::uint64_t>(
        static_cast<long double>(captured_samples_) * 1'000'000'000.0L /
        static_cast<long double>(format_.sampleRate()));
  }
  block_.samples[block_.sample_count++] = {mono, 0.0F};
  ++captured_samples_;
  if (block_.sample_count == kPublishedBlockSamples) {
    publishBlock();
  }
}

void LiveAudioCaptureWorker::publishBlock() {
  block_.sequence = sequence_++;
  if (!pipe_->blocks.try_push(block_)) {
    const auto count =
        pipe_->overruns.fetch_add(1, std::memory_order_acq_rel) + 1;
    emit overrunCountChanged(count);
  }
  block_.sample_count = 0;
}

void LiveAudioCaptureWorker::consumeAvailableBytes() {
  if (input_ == nullptr) {
    return;
  }
  const qsizetype capacity = kRawBufferBytes - pending_bytes_;
  const qsizetype bytes_read =
      input_->read(raw_buffer_.data() + pending_bytes_, capacity);
  if (bytes_read <= 0) {
    return;
  }

  const qsizetype total = pending_bytes_ + bytes_read;
  const qsizetype frame_bytes = format_.bytesPerFrame();
  const qsizetype complete_bytes = total - total % frame_bytes;
  for (qsizetype offset = 0; offset < complete_bytes; offset += frame_bytes) {
    appendFrame(raw_buffer_.data() + offset);
  }
  pending_bytes_ = total - complete_bytes;
  if (pending_bytes_ > 0) {
    std::memmove(raw_buffer_.data(), raw_buffer_.data() + complete_bytes,
                 static_cast<std::size_t>(pending_bytes_));
  }
}

void LiveAudioCaptureWorker::handleStateChanged() {
  if (!stopping_ && source_ != nullptr &&
      source_->state() == QtAudio::StoppedState &&
      source_->error() != QtAudio::NoError) {
    emit failed(QStringLiteral(
        "Live audio stopped because the input device reported an error."));
  }
}

LiveAudioDspWorker::LiveAudioDspWorker(std::shared_ptr<LiveAudioPipe> pipe,
                                       QObject* parent)
    : QObject(parent),
      pipe_(std::move(pipe)),
      timer_(this),
      live_diagnostics_timer_(this) {
  timer_.setInterval(5);
  connect(&timer_, &QTimer::timeout, this, &LiveAudioDspWorker::drain);
  // Its own timer rather than a check inside drain(): a stalled input is the
  // single most useful thing this stream can report, and a record produced
  // only when a block arrived would go silent exactly when it matters.
  live_diagnostics_timer_.setInterval(kLiveDiagnosticsIntervalMs);
  connect(&live_diagnostics_timer_, &QTimer::timeout, this,
          &LiveAudioDspWorker::publishLiveDiagnosticsRecord);
}

void LiveAudioDspWorker::start() {
  processing_complex_iq_ = false;
  decoder_model_dirty_ = false;
  drain_calls_ = 0;
  drain_capped_ = 0;
  blocks_drained_ = 0;
  model_publishes_ = 0;
  drain_micros_total_ = 0;
  gui_lateness_ms_ = 0.0;
  gui_stalls_ = 0;
  gui_heartbeat_ns_ = 0;
  capture_diagnostics_window_ = {};
  live_diagnostics_window_ = {};
  model_publish_clock_.invalidate();
  diagnostics_publish_clock_.invalidate();
  analyzer_.reset();
  decoder_analyzer_.reset();
  sdr_decoder_channelizer_.reset();
  sdr_decoder_pending_ = {};
  sdr_decoder_pending_sequence_ = 0;
  // The channelizer has just been reset along with the analyzer, the pending
  // buffer and the decoder, so whatever window was in force is no longer
  // standing on any of the state it was applied with. Clearing the applied
  // record is what guarantees the next session's first complex block puts the
  // whole path into force together, rather than inheriting a half-torn-down
  // one on the strength of a stale record.
  sdr_decoder_window_applied_ = false;
  applied_sdr_decoder_center_frequency_hz_ = 0.0;
  applied_sdr_decoder_bandwidth_hz_ = 0.0;
  pending_manual_frequency_hz_.reset();
  decoder_.reset();
  character_frontends_.reset();
  monitor_resample_phase_ = 0.0;
  monitor_resample_input_rate_hz_ = 0.0;
  monitor_resample_sum_ = 0.0F;
  monitor_resample_count_ = 0;
  resetRegionAudio();
  monitor_buffers_in_flight_.store(0, std::memory_order_release);
  region_audio_samples_ = 0;
  dropped_region_audio_buffers_ = 0;
  cwassistant::core::RealtimeSampleBlock stale;
  while (pipe_->blocks.try_pop(stale)) {
  }
  live_clock_.start();
  live_diagnostics_timer_.start();
  timer_.start();
}

void LiveAudioDspWorker::stop() {
  processing_complex_iq_ = false;
  decoder_model_dirty_ = false;
  drain_calls_ = 0;
  drain_capped_ = 0;
  blocks_drained_ = 0;
  model_publishes_ = 0;
  drain_micros_total_ = 0;
  gui_lateness_ms_ = 0.0;
  gui_stalls_ = 0;
  gui_heartbeat_ns_ = 0;
  capture_diagnostics_window_ = {};
  live_diagnostics_window_ = {};
  model_publish_clock_.invalidate();
  diagnostics_publish_clock_.invalidate();
  live_diagnostics_timer_.stop();
  live_clock_.invalidate();
  timer_.stop();
  analyzer_.reset();
  decoder_analyzer_.reset();
  sdr_decoder_channelizer_.reset();
  sdr_decoder_pending_ = {};
  sdr_decoder_pending_sequence_ = 0;
  // The channelizer has just been reset along with the analyzer, the pending
  // buffer and the decoder, so whatever window was in force is no longer
  // standing on any of the state it was applied with. Clearing the applied
  // record is what guarantees the next session's first complex block puts the
  // whole path into force together, rather than inheriting a half-torn-down
  // one on the strength of a stale record.
  sdr_decoder_window_applied_ = false;
  applied_sdr_decoder_center_frequency_hz_ = 0.0;
  applied_sdr_decoder_bandwidth_hz_ = 0.0;
  pending_manual_frequency_hz_.reset();
  decoder_.reset();
  character_frontends_.reset();
  monitor_resample_phase_ = 0.0;
  monitor_resample_input_rate_hz_ = 0.0;
  monitor_resample_sum_ = 0.0F;
  monitor_resample_count_ = 0;
  resetRegionAudio();
  monitor_buffers_in_flight_.store(0, std::memory_order_release);
  region_audio_samples_ = 0;
  dropped_region_audio_buffers_ = 0;
  emit diagnosticsProduced(
      verificationDiagnosticsModel(decoder_.verificationDiagnostics()));
  if (capture_active_) {
    finishDebugCapture(QStringLiteral("Live RX stopped"));
  }
}

void LiveAudioDspWorker::selectDecoderFrequency(
    const double audio_frequency_hz) {
  const std::uint64_t channel_id = decoder_.selectFrequency(audio_frequency_hz);
  if (channel_id == 0) {
    // Changing an SDR decoder window resets the detector's spectrum bounds.
    // A click delivered immediately after that change must survive until the
    // first spectrum from the newly configured IQ slice establishes them.
    pending_manual_frequency_hz_ = audio_frequency_hz;
    return;
  }
  pending_manual_frequency_hz_.reset();
  emit decoderProduced(decoderChannelModel(decoder_.channels()));
  emit manualDecoderSelected(static_cast<qulonglong>(channel_id));
}

void LiveAudioDspWorker::startDebugCapture(const QString& directory_path) {
  if (capture_active_) {
    finishDebugCapture(QStringLiteral("Restarted"));
  }
  const QString folder_name = QStringLiteral("cwa-debug-capture-%1")
                                  .arg(QDateTime::currentDateTimeUtc().toString(
                                      QStringLiteral("yyyyMMdd-HHmmss")));
  QDir base_dir(directory_path);
  if (!base_dir.exists()) {
    base_dir.mkpath(QStringLiteral("."));
  }
  const QString capture_dir = base_dir.filePath(folder_name);
  if (!QDir().mkpath(capture_dir)) {
    emit debugCaptureStateChanged(
        false, QString(), 0.0,
        QStringLiteral("Could not create capture folder"));
    return;
  }
  const QString log_path =
      QDir(capture_dir).filePath(QStringLiteral("diagnostics.jsonl"));
  capture_diagnostics_log_.open(log_path.toStdString(),
                                std::ios::out | std::ios::trunc);
  if (!capture_diagnostics_log_) {
    emit debugCaptureStateChanged(
        false, QString(), 0.0,
        QStringLiteral("Could not open capture diagnostics file"));
    return;
  }
  // The recording file is opened lazily on the first block in drain(), once
  // the input's actual kind and sample rate are known; opening it here with a
  // guessed rate could write a file that plays back at the wrong pitch/speed,
  // and the source mode alone does not say what the samples really are.
  capture_base_path_ = capture_dir;
  capture_wav_path_ = QDir(capture_dir).filePath(QStringLiteral("audio.wav"));
  // The same file name for both sources, because it holds the same thing in
  // both: the audio the decoder was reading. From a sound card that is the
  // card's own samples; from an SDR it is the decode region demodulated, and
  // it is written BESIDE the IQ rather than instead of it. An SDR capture used
  // to contain no listenable audio at all -- an operator reviewing a recording
  // of a signal that would not decode had a SigMF file and nothing to play.
  capture_region_audio_path_ = capture_wav_path_;
  capture_region_audio_failed_ = false;
  capture_iq_path_ =
      QDir(capture_dir).filePath(QStringLiteral("iq.sigmf-data"));
  capture_iq_sample_rate_hz_ = 0.0;
  capture_iq_center_frequency_hz_ = 0.0;
  capture_writer_pending_ = true;
  capture_start_ns_ = 0;
  capture_last_snapshot_ns_ = 0;
  capture_existing_track_count_ = decoder_.allTrackDiagnostics().size();
  capture_existing_published_count_ = decoder_.channels().size();
  capture_have_start_ = false;
  capture_active_ = true;
  emit debugCaptureStateChanged(true, capture_base_path_, 0.0,
                                QStringLiteral("Recording"));
}

void LiveAudioDspWorker::stopDebugCapture() {
  if (!capture_active_) return;
  finishDebugCapture(QStringLiteral("Stopped by operator"));
}

void LiveAudioDspWorker::setPresentationDiagnostics(
    const QVariantMap& diagnostics) {
  presentation_diagnostics_ = diagnostics;
}

void LiveAudioDspWorker::acceptGuiHeartbeat(const double lateness_ms) {
  const double lateness = std::max(0.0, lateness_ms);
  gui_lateness_ms_ = lateness;
  // The arrival time, on this worker's clock, so the record can report how
  // long it has been since the GUI thread last checked in. A heartbeat is
  // queued from the GUI thread, so a blocked GUI thread cannot deliver one --
  // which makes the growing gap a live indicator rather than a stale one.
  gui_heartbeat_ns_ =
      live_clock_.isValid() ? static_cast<std::uint64_t>(live_clock_.nsecsElapsed())
                            : 0;
  if (lateness >= kGuiStallLatenessMs) ++gui_stalls_;
  // Both windows, exactly as a slow drain is recorded in both: whichever
  // reader publishes next must see this peak, and neither may consume it on
  // the other's behalf.
  capture_diagnostics_window_.gui_peak_lateness_ms = std::max(
      capture_diagnostics_window_.gui_peak_lateness_ms, lateness);
  live_diagnostics_window_.gui_peak_lateness_ms =
      std::max(live_diagnostics_window_.gui_peak_lateness_ms, lateness);
}

void LiveAudioDspWorker::finishDebugCapture(const QString& note) {
  // A SigMF recording is two files, so an operator told only "capture
  // stopped" cannot tell what was produced or whether it is worth keeping.
  // Name the payload and its size while the writer still reports them.
  QString detail = note;
  if (capture_iq_writer_.isOpen()) {
    detail = QStringLiteral("%1 • %2 (%3 MB, %4 s IQ)")
                 .arg(note, QFileInfo(capture_iq_path_).fileName(),
                      QString::number(
                          static_cast<double>(capture_iq_writer_.bytesWritten()) /
                              (1024.0 * 1024.0),
                          'f', 1),
                      QString::number(capture_iq_writer_.secondsWritten(), 'f',
                                      1));
    // Named separately, because the question an operator asks of an SDR
    // capture is now "can I listen to it", and a line that mentions only the
    // SigMF payload answers it with silence.
    const double region_audio_rate_hz =
        region_demodulator_.outputSampleRateHz();
    if (capture_writer_.isOpen() && capture_writer_.framesWritten() > 0 &&
        region_audio_rate_hz > 0.0) {
      detail += QStringLiteral(" • %1 (%2 s audio)")
                    .arg(QFileInfo(capture_region_audio_path_).fileName(),
                         QString::number(
                             static_cast<double>(capture_writer_.framesWritten()) /
                                 region_audio_rate_hz,
                             'f', 1));
    }
  }
  capture_writer_.close();
  capture_iq_writer_.close();
  if (capture_diagnostics_log_.is_open()) {
    capture_diagnostics_log_.flush();
    capture_diagnostics_log_.close();
  }
  const double elapsed_seconds =
      capture_have_start_
          ? static_cast<double>(capture_last_snapshot_ns_ - capture_start_ns_) /
                1'000'000'000.0
          : 0.0;
  capture_active_ = false;
  capture_writer_pending_ = false;
  capture_have_start_ = false;
  // Cleared only after the elapsed time above has been read. The same record
  // now goes out on the live stream between captures, and a stream record
  // still reporting the track counts a finished capture inherited would read
  // as a capture that is still running.
  capture_existing_track_count_ = 0;
  capture_existing_published_count_ = 0;
  emit debugCaptureStateChanged(false, capture_base_path_, elapsed_seconds,
                                detail);
}

void LiveAudioDspWorker::writeDebugCaptureSnapshot() {
  // The capture file reads its own measurement window, so the rates it
  // records are the same whether or not anyone is watching the live stream.
  capture_diagnostics_log_
      << QJsonDocument(
             buildDiagnosticsRecord(
                 capture_diagnostics_window_, capture_last_snapshot_ns_,
                 static_cast<double>(capture_last_snapshot_ns_ -
                                     capture_start_ns_) /
                     1'000'000'000.0))
             .toJson(QJsonDocument::Compact)
             .toStdString()
      << '\n';
}

void LiveAudioDspWorker::publishLiveDiagnosticsRecord() {
  // The timer is only running while live reception is, but a stop() that
  // arrives between a timeout and its delivery would leave the clock invalid.
  if (!live_clock_.isValid()) return;
  const auto now_ns = static_cast<std::uint64_t>(live_clock_.nsecsElapsed());
  emit diagnosticsRecordProduced(buildDiagnosticsRecord(
      live_diagnostics_window_, now_ns,
      static_cast<double>(now_ns) / 1'000'000'000.0));
}

QJsonObject LiveAudioDspWorker::buildDiagnosticsRecord(
    DiagnosticsWindow& window, const std::uint64_t now_ns,
    const double elapsed_seconds) {
  QJsonObject root;
  root.insert(QStringLiteral("elapsedSeconds"), elapsed_seconds);

  // A diagnostic capture deliberately does not reset the live decoder: doing
  // so would interrupt open sessions merely because the operator requested a
  // recording. Make that provenance explicit so inherited tracks/callsigns in
  // the first JSON snapshot are not mistaken for evidence found in audio.wav.
  // Between captures these are zero, which is how a stream reader tells a
  // record that accompanies a recording from one that does not.
  QJsonObject capture_context;
  capture_context.insert(QStringLiteral("startedWithExistingDecoderState"),
                         capture_existing_track_count_ != 0U);
  capture_context.insert(QStringLiteral("existingTracksAtStart"),
                         static_cast<qint64>(capture_existing_track_count_));
  capture_context.insert(
      QStringLiteral("existingPublishedChannelsAtStart"),
      static_cast<qint64>(capture_existing_published_count_));
  root.insert(QStringLiteral("captureContext"), capture_context);
  root.insert(QStringLiteral("presentation"),
              QJsonObject::fromVariantMap(presentation_diagnostics_));
  const auto diagnostics = decoder_.verificationDiagnostics();
  QJsonObject summary;
  summary.insert(QStringLiteral("candidateTracks"),
                 static_cast<qint64>(diagnostics.candidate_tracks));
  summary.insert(QStringLiteral("morseLikelyTracks"),
                 static_cast<qint64>(diagnostics.morse_likely_tracks));
  summary.insert(QStringLiteral("verifiedTracks"),
                 static_cast<qint64>(diagnostics.verified_tracks));
  root.insert(QStringLiteral("summary"), summary);

  // What the detector was actually given. A field report of "decodes nothing"
  // could not be told apart from a configuration that excluded the signal,
  // because none of this was recorded: the analyzer settings, whether any
  // spectrum frame reached the detector at all, and whether the Morse
  // alphabet in force came from a file or from the copy inside the
  // application. Without them the only way to narrow it is to guess.
  // Whether the application is keeping up, as opposed to what it found.
  //
  // Rates are measured between this reader's own successive records rather
  // than over the whole session, so a stall shows as a change instead of
  // being averaged away. The two that matter most:
  // modelPublishesPerSecond, which reached the hundreds when the
  // model was published once per drained block and buried the thread that
  // draws; and drainsCappedPerSecond, which is non-zero only when blocks were
  // still waiting after a drain gave up.
  const double window_seconds =
      window.opened_ns != 0 && now_ns > window.opened_ns
          ? static_cast<double>(now_ns - window.opened_ns) / 1'000'000'000.0
          : 0.0;
  const auto per_second = [window_seconds](const std::uint64_t delta) {
    return window_seconds > 0.0 ? static_cast<double>(delta) / window_seconds : 0.0;
  };
  QJsonObject throughput;
  throughput.insert(QStringLiteral("windowSeconds"), window_seconds);
  throughput.insert(QStringLiteral("drainsPerSecond"),
                    per_second(drain_calls_ - window.drain_calls));
  throughput.insert(QStringLiteral("blocksPerSecond"),
                    per_second(blocks_drained_ - window.blocks));
  throughput.insert(QStringLiteral("modelPublishesPerSecond"),
                    per_second(model_publishes_ - window.publishes));
  throughput.insert(QStringLiteral("drainsCappedPerSecond"),
                    per_second(drain_capped_ - window.capped));
  throughput.insert(
      QStringLiteral("averageDrainMicroseconds"),
      drain_calls_ > window.drain_calls
          ? static_cast<double>(drain_micros_total_ - window.micros) /
                static_cast<double>(drain_calls_ - window.drain_calls)
          : 0.0);
  throughput.insert(QStringLiteral("peakDrainMicroseconds"),
                    static_cast<double>(window.peak_micros));
  root.insert(QStringLiteral("throughput"), throughput);

  // The region-audio producer, stated as facts rather than as intent: whether
  // it is running, at what rate, how much it has produced, and how much was
  // dropped instead of queued when the thread that plays fell behind. A
  // station with the monitor off, no observer and no capture must show zero
  // samples however long it has been receiving.
  QJsonObject region_audio;
  region_audio.insert(QStringLiteral("active"), region_audio_active_);
  region_audio.insert(QStringLiteral("wanted"), regionAudioWanted());
  region_audio.insert(QStringLiteral("sampleRateHz"),
                      region_demodulator_.outputSampleRateHz());
  region_audio.insert(QStringLiteral("samples"),
                      static_cast<qint64>(region_audio_samples_));
  region_audio.insert(QStringLiteral("droppedBuffers"),
                      static_cast<qint64>(dropped_region_audio_buffers_));
  root.insert(QStringLiteral("regionAudio"), region_audio);

  // Whether the thread that DRAWS is keeping up, which nothing above measures.
  //
  // Everything in `throughput` describes this worker. The fault this stream
  // was built for is the opposite one: an application that fatigues and a
  // spectrum that turns snappy while the processor is mostly idle, which is a
  // saturated GUI thread and leaves every decoder counter looking perfect. So
  // the GUI thread runs a fixed-interval heartbeat and reports how late each
  // fire was -- elapsed time since the previous fire, minus the interval it
  // asked for. That remainder is precisely how long the thread could not get
  // back to its event loop, which is how long it could not repaint either.
  //
  // Lateness rather than a frame rate, because lateness survives the stall it
  // is measuring. If the GUI thread blocks for three seconds, it delivers no
  // heartbeat for three seconds and the first one afterwards reports three
  // seconds of lateness: the gap in the numbers and the spike that ends it are
  // the same event seen twice. A frame-rate counter would merely have stopped,
  // which is indistinguishable from an idle screen with nothing to draw.
  //
  // `sinceLastHeartbeatMs` is the half of that picture that is readable while
  // the stall is still in progress -- it grows on this worker's clock for as
  // long as the GUI thread stays blocked, and it is deliberately not folded
  // into the peak, which reports only lateness the GUI thread actually
  // measured and reported. A value near the interval is a healthy thread; a
  // value in the seconds means the window is stuck right now.
  QJsonObject gui;
  gui.insert(QStringLiteral("heartbeatIntervalMs"),
             static_cast<double>(kGuiHeartbeatIntervalMs));
  gui.insert(QStringLiteral("latenessMs"), gui_lateness_ms_);
  gui.insert(QStringLiteral("peakLatenessMs"), window.gui_peak_lateness_ms);
  gui.insert(QStringLiteral("stallCount"),
             static_cast<qint64>(gui_stalls_ - window.gui_stalls));
  gui.insert(QStringLiteral("stallThresholdMs"), kGuiStallLatenessMs);
  gui.insert(QStringLiteral("sinceLastHeartbeatMs"),
             live_clock_.isValid()
                 ? static_cast<double>(
                       static_cast<std::uint64_t>(live_clock_.nsecsElapsed()) -
                       gui_heartbeat_ns_) /
                       1'000'000.0
                 : 0.0);
  root.insert(QStringLiteral("gui"), gui);

  // Re-open this reader's window, and only this reader's. The other reader's
  // baselines are untouched, so its next record still measures from the last
  // time it was read rather than from whenever this one happened to run.
  window.opened_ns = now_ns;
  window.drain_calls = drain_calls_;
  window.blocks = blocks_drained_;
  window.publishes = model_publishes_;
  window.capped = drain_capped_;
  window.micros = drain_micros_total_;
  window.gui_stalls = gui_stalls_;
  // Peak is per window, so one slow drain does not mark every later record
  // as slow.
  window.peak_micros = 0;
  // Same rule for the GUI peak: a maximum that is never cleared reports one
  // early hitch forever, and a stream reader watching a recovered application
  // would see it flagged as stalled for the rest of the session. Cleared here,
  // in the reader's own window, so clearing it for the live stream cannot
  // change what diagnostics.jsonl records for a capture nobody is watching.
  window.gui_peak_lateness_ms = 0.0;

  const auto analyzer_config = analyzer_.config();
  QJsonObject detector;
  detector.insert(QStringLiteral("fftSize"),
                  static_cast<double>(analyzer_config.fft_size));
  detector.insert(QStringLiteral("frameRateHz"),
                  static_cast<double>(analyzer_config.frame_rate_hz));
  detector.insert(QStringLiteral("averagingFrames"),
                  static_cast<double>(analyzer_config.averaging_frames));
  detector.insert(QStringLiteral("audioLowerHz"),
                  analyzer_config.audio_lower_frequency_hz);
  detector.insert(QStringLiteral("audioUpperHz"),
                  analyzer_config.audio_upper_frequency_hz);
  detector.insert(QStringLiteral("automaticBandwidth"),
                  analyzer_config.audio_automatic_bandwidth);
  detector.insert(QStringLiteral("automaticGain"),
                  analyzer_config.audio_automatic_gain);
  detector.insert(QStringLiteral("gainDb"),
                  static_cast<double>(analyzer_config.audio_gain_db));
  detector.insert(QStringLiteral("dcRejection"),
                  analyzer_config.audio_dc_rejection);
  detector.insert(QStringLiteral("spectrumFramesToDetector"),
                  static_cast<double>(detector_frames_));
  detector.insert(QStringLiteral("decoderResets"),
                  static_cast<double>(decoder_resets_));
  detector.insert(QStringLiteral("morseAlphabetSymbols"),
                  static_cast<double>(
                      cwassistant::core::cwSharedMorseAlphabet().size()));
  detector.insert(QStringLiteral("morseAlphabetFromBuiltin"),
                  cwassistant::core::cwMorseAlphabetLoadedFromBuiltin());
  detector.insert(QStringLiteral("exchangeVocabularyTokens"),
                  static_cast<double>(
                      cwassistant::core::cwSharedVocabulary()
                          .exchangeWordCount()));
  root.insert(QStringLiteral("detector"), detector);

  // Recorded every snapshot (not just once) specifically so a reviewer can
  // tell, after the fact, whether/when the operator's RX VFO moved during
  // the capture window -- a VFO move is a common, easily overlooked
  // explanation for a signal that stops decoding partway through a capture.
  QJsonObject radio;
  radio.insert(QStringLiteral("available"), radio_frequency_available_);
  radio.insert(QStringLiteral("rxFrequencyHz"),
               static_cast<double>(radio_rx_rf_hz_));
  radio.insert(QStringLiteral("txFrequencyHz"),
               static_cast<double>(radio_tx_rf_hz_));
  radio.insert(QStringLiteral("splitActive"), radio_split_active_);
  root.insert(QStringLiteral("radio"), radio);

  // Receiver level telemetry rides with every snapshot so a reviewer can see
  // when the front end started clipping, not merely that it did somewhere in
  // the recording. Gain state is repeated here because the sidecar records
  // only the value at the moment the capture began.
  if (capture_iq_writer_.isOpen()) {
    const auto block_levels = capture_iq_writer_.lastBlockLevels();
    const auto capture_totals = capture_iq_writer_.captureLevels();
    QJsonObject iq;
    iq.insert(QStringLiteral("dataFile"),
              QFileInfo(capture_iq_path_).fileName());
    iq.insert(QStringLiteral("metadataFile"),
              QFileInfo(QString::fromStdString(
                            capture_iq_writer_.metadataPath()))
                  .fileName());
    const auto datatype =
        cwassistant::core::IqWriter::datatypeName(capture_iq_format_);
    iq.insert(QStringLiteral("datatype"),
              QString::fromLatin1(datatype.data(),
                                  static_cast<qsizetype>(datatype.size())));
    iq.insert(QStringLiteral("sampleRateHz"), capture_iq_sample_rate_hz_);
    iq.insert(QStringLiteral("centerFrequencyHz"),
              capture_iq_center_frequency_hz_);
    iq.insert(QStringLiteral("sampleCount"),
              static_cast<qint64>(capture_iq_writer_.samplesWritten()));
    iq.insert(QStringLiteral("dataBytes"),
              static_cast<qint64>(capture_iq_writer_.bytesWritten()));
    iq.insert(QStringLiteral("recordedSeconds"),
              capture_iq_writer_.secondsWritten());
    iq.insert(QStringLiteral("automaticGainKnown"), sdr_gain_state_known_);
    iq.insert(QStringLiteral("automaticGain"), sdr_automatic_gain_);
    iq.insert(QStringLiteral("gainDb"), sdr_gain_db_);
    iq.insert(QStringLiteral("nearFullScaleThreshold"),
              cwassistant::core::IqWriter::kNearFullScale);
    iq.insert(QStringLiteral("blockPeakMagnitude"),
              block_levels.peak_magnitude);
    iq.insert(QStringLiteral("blockNearFullScaleSamples"),
              static_cast<qint64>(block_levels.near_full_scale_samples));
    iq.insert(QStringLiteral("blockDcReal"), block_levels.mean_real);
    iq.insert(QStringLiteral("blockDcImaginary"), block_levels.mean_imaginary);
    iq.insert(QStringLiteral("capturePeakMagnitude"),
              capture_totals.peak_magnitude);
    iq.insert(QStringLiteral("captureNearFullScaleSamples"),
              static_cast<qint64>(capture_totals.near_full_scale_samples));
    iq.insert(QStringLiteral("captureDcReal"), capture_totals.mean_real);
    iq.insert(QStringLiteral("captureDcImaginary"),
              capture_totals.mean_imaginary);
    root.insert(QStringLiteral("iq"), iq);
  }

  QJsonArray tracks;
  const auto published_channels = decoder_.channels();
  for (const auto& track : decoder_.allTrackDiagnostics()) {
    QJsonObject item;
    item.insert(QStringLiteral("id"), static_cast<qint64>(track.id));
    item.insert(QStringLiteral("frequencyHz"), track.frequency_hz);
    item.insert(QStringLiteral("identityOriginFrequencyHz"),
                track.identity_origin_frequency_hz);
    item.insert(QStringLiteral("presentationFrequencyHz"),
                track.presentation_frequency_hz);
    item.insert(QStringLiteral("driftHzPerSecond"), track.drift_hz_per_second);
    item.insert(QStringLiteral("snrDb"), track.snr_db);
    item.insert(QStringLiteral("narrowbandCoherence"),
                track.narrowband_coherence);
    item.insert(QStringLiteral("filterWidthHz"), track.filter_width_hz);
    item.insert(QStringLiteral("state"),
                QString::fromLatin1(cwassistant::core::cwTrackStateName(
                    track.verification_state)));
    item.insert(QStringLiteral("reason"),
                QString::fromLatin1(cwassistant::core::cwVerificationReasonName(
                    track.verification_reason)));
    item.insert(QStringLiteral("spectralObservations"),
                track.spectral_observations);
    item.insert(QStringLiteral("keyTransitions"),
                static_cast<qint64>(track.key_transitions));
    item.insert(QStringLiteral("decodedSymbols"),
                static_cast<qint64>(track.decoded_symbols));
    item.insert(QStringLiteral("unknownSymbols"),
                static_cast<qint64>(track.unknown_symbols));
    item.insert(QStringLiteral("timingQuality"), track.timing_quality);
    item.insert(QStringLiteral("cadenceQuality"), track.cadence_quality);
    item.insert(QStringLiteral("meanCharacterConfidence"),
                track.mean_character_confidence);
    item.insert(QStringLiteral("wpm"), track.wpm);
    item.insert(QStringLiteral("acousticWpm"), track.acoustic_wpm);
    item.insert(QStringLiteral("acousticCadenceConfidence"),
                track.acoustic_cadence_confidence);
    item.insert(QStringLiteral("keyingLevelSeparationDb"),
                track.keying_level_separation_db);
    item.insert(QStringLiteral("keyingLevelExplainedVariation"),
                track.keying_level_explained_variation);
    item.insert(QStringLiteral("robustKeyingLevelAnchorActive"),
                track.robust_keying_level_anchor_active);
    item.insert(QStringLiteral("text"), QString::fromStdString(track.text));
    item.insert(QStringLiteral("refinedText"),
                QString::fromStdString(track.refined_text));
    const auto published = std::find_if(
        published_channels.begin(), published_channels.end(),
        [&track](const cwassistant::core::CwChannelSnapshot& channel) {
          return channel.id == track.id;
        });
    const std::string presented_callsign = published == published_channels.end()
                                               ? std::string{}
                                               : published->callsign;
    QString callsign_source = QStringLiteral("none");
    if (!presented_callsign.empty()) {
      if (cwassistant::core::CallsignPolicy::best_complete_in_text(
              track.refined_text) == presented_callsign) {
        callsign_source = QStringLiteral("phase-consensus");
      } else if (cwassistant::core::CallsignPolicy::best_complete_in_text(
                     track.text) == presented_callsign) {
        callsign_source = QStringLiteral("literal-decoder");
      } else {
        callsign_source = QStringLiteral("retained");
      }
    }
    item.insert(QStringLiteral("presentedCallsign"),
                QString::fromStdString(presented_callsign));
    item.insert(QStringLiteral("presentedCallsignSource"), callsign_source);
    QJsonArray transmissions;
    if (published != published_channels.end()) {
      item.insert(QStringLiteral("currentSenderCallsign"),
                  QString::fromStdString(published->current_sender_callsign));
      item.insert(QStringLiteral("currentSenderWpm"),
                  published->current_sender_wpm);
      for (const auto& turn : published->transmissions) {
        QJsonObject value;
        value.insert(QStringLiteral("sequence"),
                     static_cast<qint64>(turn.sequence));
        value.insert(QStringLiteral("text"), QString::fromStdString(turn.text));
        value.insert(QStringLiteral("sender"),
                     QString::fromStdString(turn.sender_callsign));
        value.insert(QStringLiteral("wpm"), turn.wpm);
        value.insert(QStringLiteral("cadenceConfidence"),
                     turn.cadence_confidence);
        if (turn.timing_fingerprint) {
          const auto& timing = *turn.timing_fingerprint;
          QJsonObject fingerprint;
          fingerprint.insert(QStringLiteral("firstObservationId"),
                             static_cast<qint64>(timing.first_observation_id));
          fingerprint.insert(QStringLiteral("lastObservationId"),
                             static_cast<qint64>(timing.last_observation_id));
          fingerprint.insert(QStringLiteral("evidenceStartedNs"),
                             static_cast<qint64>(timing.evidence_started_ns));
          fingerprint.insert(QStringLiteral("evidenceEndedNs"),
                             static_cast<qint64>(timing.evidence_ended_ns));
          fingerprint.insert(QStringLiteral("markCount"),
                             static_cast<qint64>(timing.mark_count));
          fingerprint.insert(QStringLiteral("gapCount"),
                             static_cast<qint64>(timing.gap_count));
          fingerprint.insert(QStringLiteral("ditCount"),
                             static_cast<qint64>(timing.dit_count));
          fingerprint.insert(QStringLiteral("dahCount"),
                             static_cast<qint64>(timing.dah_count));
          fingerprint.insert(QStringLiteral("elementGapCount"),
                             static_cast<qint64>(timing.element_gap_count));
          fingerprint.insert(QStringLiteral("characterGapCount"),
                             static_cast<qint64>(timing.character_gap_count));
          fingerprint.insert(QStringLiteral("wordGapCount"),
                             static_cast<qint64>(timing.word_gap_count));
          fingerprint.insert(QStringLiteral("ditMedianMs"),
                             timing.dit_median_ms);
          fingerprint.insert(QStringLiteral("dahMedianMs"),
                             timing.dah_median_ms);
          fingerprint.insert(QStringLiteral("elementGapMedianMs"),
                             timing.element_gap_median_ms);
          fingerprint.insert(QStringLiteral("characterGapMedianMs"),
                             timing.character_gap_median_ms);
          fingerprint.insert(QStringLiteral("wordGapMedianMs"),
                             timing.word_gap_median_ms);
          fingerprint.insert(QStringLiteral("keyingWeight"),
                             timing.keying_weight);
          fingerprint.insert(QStringLiteral("normalizedMarkResidual"),
                             timing.normalized_mark_residual);
          fingerprint.insert(QStringLiteral("evidenceConfidence"),
                             timing.evidence_confidence);
          value.insert(QStringLiteral("timingFingerprint"), fingerprint);
        } else {
          value.insert(QStringLiteral("timingFingerprint"), QJsonValue::Null);
        }
        transmissions.push_back(value);
      }
    }
    item.insert(QStringLiteral("transmissions"), transmissions);
    QJsonArray alternatives;
    for (const auto& alternative : track.acoustic_alternatives) {
      QJsonObject candidate;
      candidate.insert(QStringLiteral("text"),
                       QString::fromStdString(alternative.text));
      candidate.insert(
          QStringLiteral("provisionalElements"),
          QString::fromStdString(alternative.provisional_elements));
      candidate.insert(QStringLiteral("wpm"), alternative.wpm);
      candidate.insert(QStringLiteral("acousticCost"),
                       alternative.acoustic_cost);
      candidate.insert(QStringLiteral("evidenceConfidence"),
                       alternative.evidence_confidence);
      candidate.insert(QStringLiteral("firstObservationId"),
                       static_cast<qint64>(alternative.first_observation_id));
      candidate.insert(QStringLiteral("lastObservationId"),
                       static_cast<qint64>(alternative.last_observation_id));
      alternatives.push_back(candidate);
    }
    item.insert(QStringLiteral("acousticAlternatives"), alternatives);
    item.insert(QStringLiteral("provisionalText"),
                QString::fromStdString(track.provisional_text));
    item.insert(QStringLiteral("matchAgeSeconds"), track.match_age_seconds);
    item.insert(QStringLiteral("colorIndex"),
                static_cast<int>(track.color_index));
    item.insert(QStringLiteral("matched"), track.matched);
    item.insert(QStringLiteral("active"), track.active);
    item.insert(QStringLiteral("keyDown"), track.key_down);
    item.insert(QStringLiteral("operatorSelected"), track.operator_selected);
    tracks.push_back(item);
  }
  root.insert(QStringLiteral("tracks"), tracks);
  return root;
}

void LiveAudioDspWorker::configure(
    const int averaging_frames, const int frame_rate_hz,
    const bool dc_rejection, const bool automatic_gain, const double gain_db,
    const double automatic_gain_target_dbfs, const bool automatic_bandwidth,
    const double lower_frequency_hz, const double upper_frequency_hz) {
  const auto previous = analyzer_.config();
  auto config = previous;
  config.averaging_frames =
      static_cast<std::uint8_t>(std::clamp(averaging_frames, 1, 32));
  config.frame_rate_hz =
      static_cast<std::uint16_t>(std::clamp(frame_rate_hz, 1, 120));
  config.audio_dc_rejection = dc_rejection;
  config.audio_automatic_gain = automatic_gain;
  config.audio_gain_db = static_cast<float>(std::clamp(gain_db, -40.0, 40.0));
  config.audio_automatic_gain_target_dbfs =
      static_cast<float>(std::clamp(automatic_gain_target_dbfs, -40.0, -1.0));
  config.audio_automatic_bandwidth = automatic_bandwidth;
  config.audio_lower_frequency_hz =
      std::clamp(lower_frequency_hz, 0.0, 95'999.0);
  config.audio_upper_frequency_hz = std::clamp(
      upper_frequency_hz, config.audio_lower_frequency_hz + 1.0, 96'000.0);
  // Only a change that alters the audio actually presented to the detector may
  // discard decoder state. Spectrum averaging and the display frame rate are
  // presentation settings: resetting on them destroyed every track, transcript
  // and callsign whenever the operator moved a display slider.
  const bool signal_path_changed =
      config.audio_dc_rejection != previous.audio_dc_rejection ||
      config.audio_automatic_gain != previous.audio_automatic_gain ||
      config.audio_gain_db != previous.audio_gain_db ||
      config.audio_automatic_gain_target_dbfs !=
          previous.audio_automatic_gain_target_dbfs ||
      config.audio_automatic_bandwidth != previous.audio_automatic_bandwidth ||
      config.audio_lower_frequency_hz != previous.audio_lower_frequency_hz ||
      config.audio_upper_frequency_hz != previous.audio_upper_frequency_hz;
  static_cast<void>(analyzer_.configure(config));
  if (signal_path_changed) {
    ++decoder_resets_;
    decoder_.reset();
  }
}

void LiveAudioDspWorker::setDebugCaptureMaximumSeconds(const double seconds) {
  maximum_capture_seconds_ = std::clamp(seconds, 30.0, 1'800.0);
}

void LiveAudioDspWorker::setOperatorRole(const QString& role) {
  decoder_.setOperatorRole(
      cwassistant::core::cwOperatorRoleFromName(role.toStdString()));
}

void LiveAudioDspWorker::setKeyingModel(const QString& model) {
  decoder_.setKeyingModel(
      cwassistant::core::cwKeyingModelFromName(model.toStdString()));
}

void LiveAudioDspWorker::setOwnCallsign(const QString& callsign) {
  decoder_.setOwnCallsign(callsign.trimmed().toStdString());
}

void LiveAudioDspWorker::setDecodedSignalTimeoutSeconds(const int seconds) {
  decoder_.configure({.decoded_track_retention_seconds =
                          static_cast<double>(std::clamp(seconds, 5, 120))});
}

void LiveAudioDspWorker::setWeakSignalDecoding(
    const bool enabled, const double minimum_decode_snr_db) {
  // No reset here, deliberately. Only a change to the audio presented to the
  // detector may discard decoder state; this one changes which of the already
  // tracked signals are worth decoding, and the bank applies it on its next
  // update without losing a single track, transcript or confirmed callsign.
  decoder_.setWeakSignalDecoding(enabled,
                                 static_cast<float>(minimum_decode_snr_db));
}

void LiveAudioDspWorker::setLocalCharacterFrontendEnabled(const bool enabled) {
  character_frontends_.setEnabled(enabled);
}

void LiveAudioDspWorker::setMonitor(const int mode,
                                    const QVariantList& channel_ids,
                                    const double reference_tone_hz) {
  // 3 is region listening: the whole decode region demodulated to audio, as
  // opposed to 1 (the raw receiver passband, meaningless for complex IQ) and 2
  // (one tracked signal, narrow-filtered and re-pitched to the sidetone).
  monitor_mode_ = std::clamp(mode, 0, 3);
  monitor_resample_phase_ = 0.0;
  monitor_resample_input_rate_hz_ = 0.0;
  monitor_resample_sum_ = 0.0F;
  monitor_resample_count_ = 0;
  // Leaving region mode ends the demand this worker was producing audio for,
  // and entering it starts a new one. Either way the next region audio must
  // begin from a clean oscillator and an empty delay line: the samples in
  // between were never demodulated, so carrying state across them would play a
  // fragment of an older region into the first buffer the operator hears.
  resetRegionAudio();
  monitor_buffers_in_flight_.store(0, std::memory_order_release);
  // The sample and drop counters are deliberately NOT cleared here. They are
  // session totals, and a reader asking "has this station been demodulating
  // the region" must not be answered differently because the operator touched
  // the listen control a moment ago.
  const auto selected_mode =
      mode == 1   ? cwassistant::core::CwMonitorMode::FullReceiver
      : mode == 2 ? cwassistant::core::CwMonitorMode::SelectedTrack
                  : cwassistant::core::CwMonitorMode::Off;
  std::vector<std::uint64_t> ids;
  ids.reserve(static_cast<std::size_t>(channel_ids.size()));
  for (const QVariant& value : channel_ids)
    ids.push_back(static_cast<std::uint64_t>(value.toULongLong()));
  decoder_.setMonitorTracks(selected_mode, ids, reference_tone_hz);
}

bool LiveAudioDspWorker::sdrDecoderWindowNeedsApply() const noexcept {
  return !sdr_decoder_window_applied_ ||
         applied_sdr_decoder_center_frequency_hz_ !=
             sdr_decoder_center_frequency_hz_ ||
         applied_sdr_decoder_bandwidth_hz_ != sdr_decoder_bandwidth_hz_;
}

void LiveAudioDspWorker::applySdrDecoderWindow() {
  const double output_rate_hz =
      std::clamp(sdr_decoder_bandwidth_hz_ * 2.5, 48'000.0, 192'000.0);
  if (!sdr_decoder_channelizer_.configure(
          {.center_frequency_hz = sdr_decoder_center_frequency_hz_,
           .bandwidth_hz = sdr_decoder_bandwidth_hz_,
           .maximum_output_sample_rate_hz = output_rate_hz})) {
    // Left unapplied on purpose. A window the channelizer will not accept is
    // not in force, and recording it as applied would be the same mistake this
    // separation exists to prevent -- the next request, or the next block,
    // must be free to try again.
    return;
  }
  sdr_decoder_window_applied_ = true;
  applied_sdr_decoder_center_frequency_hz_ = sdr_decoder_center_frequency_hz_;
  applied_sdr_decoder_bandwidth_hz_ = sdr_decoder_bandwidth_hz_;
  sdr_decoder_pending_ = {};
  sdr_decoder_pending_sequence_ = 0;
  decoder_analyzer_.reset();
  // Not reset(). The samples jumped; the stations did not. A track's frequency
  // is absolute RF, so a station stays where it was when the receiver moves,
  // and wiping the bank here destroyed every track, transcript and identity on
  // each retune -- what an operator sees as losing the tracks whenever the RF
  // spectrum is moved. Tracks the new window no longer covers are parked by
  // the bank's own out-of-band rule and come back with the receiver.
  decoder_.noteInputDiscontinuity();
  character_frontends_.reset();
  // The region itself has moved, which changes both the demodulator's shift
  // frequency and the meaning of every sample still in its delay line.
  resetRegionAudio();
}

void LiveAudioDspWorker::noteSpectrumFrameConsumed() noexcept {
  const int previous =
      spectrum_frames_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
  // Never let it go negative. A frame consumed after a stop, or a stop that
  // cleared the count while a frame was still queued, would otherwise leave a
  // permanent credit that defeats the bound for the rest of the session.
  if (previous <= 0) {
    spectrum_frames_in_flight_.store(0, std::memory_order_release);
  }
}

bool LiveAudioDspWorker::publishSpectrumFrame(SpectrumFrame&& frame) {
  // Latest-wins. The display cannot show a frame that was superseded before it
  // was drawn, so a frame refused here costs nothing but the backlog it would
  // have added -- and that backlog is what took the application down.
  if (spectrum_frames_in_flight_.load(std::memory_order_acquire) >=
      kMaximumSpectrumFramesInFlight) {
    ++dropped_spectrum_frames_;
    ++dropped_since_delivered_;
    return false;
  }
  spectrum_frames_in_flight_.fetch_add(1, std::memory_order_acq_rel);
  // Carried with the frame so the display can tell a break in reception from
  // its own inability to keep up. Without it the waterfall padded every
  // dropped interval with a blank row and filled with black stripes.
  frame.dropped_before = dropped_since_delivered_;
  dropped_since_delivered_ = 0;
  emit frameProduced(frame);
  return true;
}

void LiveAudioDspWorker::setSdrDecoderWindow(const double center_frequency_hz,
                                             const double bandwidth_hz) {
  // Always recorded, and never an early return on the requested values. These
  // settings are republished whenever anything on the SDR page changes, and
  // that happens while an audio card is the running source, so what is stored
  // here is a request and nothing more.
  //
  // The previous version returned early when the request matched what was
  // stored, which made the stored pair mean both "asked for" and "in force".
  // At SDR start the window is published before the first IQ block arrives:
  // the request was stored with nothing applied, every later republication
  // asked for the same values and returned early, and the channelizer kept its
  // defaults and refused every block. The overview spectrum paints before the
  // channelizer is consulted, so the operator saw a live waterfall with the
  // decoder window drawn on it and nothing decoding -- until a click moved the
  // window, which changed the values and finally got past the early return.
  sdr_decoder_center_frequency_hz_ = center_frequency_hz;
  sdr_decoder_bandwidth_hz_ = bandwidth_hz;
  // Acting on it is another matter, and both conditions are load-bearing.
  //
  // Not while an audio card is the source: the decoder window then has no
  // bearing on what is being decoded, and resetting the shared decoder for it
  // destroyed a working audio decode for a receiver that was not even running
  // -- which is why switching to SDR and back left audio decoding nothing
  // until the application was restarted. `drain()` applies it instead, on the
  // first complex block, which is the moment it starts to mean something.
  //
  // Not when it is already applied: most republications ask for exactly what
  // is in force, and tearing the signal path down for an unchanged value would
  // interrupt a perfectly good decode for nothing.
  if (!processing_complex_iq_ || !sdrDecoderWindowNeedsApply()) return;
  applySdrDecoderWindow();
}

void LiveAudioDspWorker::acceptCharacterRefinement(
    const qulonglong channel_id, const QString& stable_text,
    const qulonglong evidence_timestamp_ns) {
  if (decoder_.acceptCharacterRefinement(
          static_cast<std::uint64_t>(channel_id), stable_text.toStdString(),
          static_cast<std::uint64_t>(evidence_timestamp_ns))) {
    emit decoderProduced(decoderChannelModel(decoder_.channels()));
  }
}

void LiveAudioDspWorker::shiftTrackedFrequencies(const double audio_hz_delta) {
  decoder_.shiftTrackedFrequencies(audio_hz_delta);
}

void LiveAudioDspWorker::setRadioFrequencyContext(const bool available,
                                                  const qulonglong rx_rf_hz,
                                                  const qulonglong tx_rf_hz,
                                                  const bool split_active) {
  radio_frequency_available_ = available;
  radio_rx_rf_hz_ = rx_rf_hz;
  radio_tx_rf_hz_ = tx_rf_hz;
  radio_split_active_ = split_active;
}

void LiveAudioDspWorker::setSdrCaptureContext(const QString& receiver_label,
                                              const QString& antenna,
                                              const bool automatic_gain,
                                              const double gain_db) {
  sdr_receiver_label_ = receiver_label;
  sdr_antenna_ = antenna;
  sdr_automatic_gain_ = automatic_gain;
  sdr_gain_db_ = gain_db;
  // Distinguish "no SDR has reported its gain" from "gain is 0 dB": a
  // recording that silently claims 0 dB when nothing was known would send a
  // later analysis after a front-end fault that never existed.
  sdr_gain_state_known_ = true;
}

bool LiveAudioDspWorker::openIqCapture(
    const cwassistant::core::RealtimeSampleBlock& block) {
  // Everything the recording needs to be interpretable later comes from the
  // descriptor that produced these very samples, not from UI state that may
  // have moved on since the operator pressed the button.
  cwassistant::core::IqCaptureMetadata metadata;
  metadata.format = capture_iq_format_;
  metadata.sample_rate_hz = block.stream.sample_rate_hz;
  metadata.center_frequency_hz = block.stream.center_frequency_hz;
  metadata.hardware =
      (sdr_receiver_label_.isEmpty()
           ? QStringLiteral("Direct SDR receiver")
           : (sdr_antenna_.isEmpty()
                  ? sdr_receiver_label_
                  : QStringLiteral("%1 (input %2)")
                        .arg(sdr_receiver_label_, sdr_antenna_)))
          .toStdString();
  metadata.description =
      QStringLiteral(
          "CW Buddy operator debug capture; decoder window %1 Hz wide at %2 Hz")
          .arg(sdr_decoder_bandwidth_hz_, 0, 'f', 0)
          .arg(sdr_decoder_center_frequency_hz_, 0, 'f', 0)
          .toStdString();
  metadata.automatic_gain_known = sdr_gain_state_known_;
  metadata.automatic_gain = sdr_automatic_gain_;
  metadata.gain_db = sdr_gain_db_;
  if (!capture_iq_writer_.open(
          capture_iq_path_.toStdString(), metadata,
          {.maximum_data_bytes = kMaximumIqCaptureBytes,
           .maximum_seconds = maximum_capture_seconds_})) {
    finishDebugCapture(
        QStringLiteral("Could not open IQ capture file: %1")
            .arg(QString::fromStdString(capture_iq_writer_.lastError())));
    return false;
  }
  capture_iq_sample_rate_hz_ = block.stream.sample_rate_hz;
  capture_iq_center_frequency_hz_ = block.stream.center_frequency_hz;
  capture_writer_pending_ = false;
  return true;
}

void LiveAudioDspWorker::captureBlock(
    const cwassistant::core::RealtimeSampleBlock& block) {
  if (!capture_active_) return;
  // The recorder is chosen from the block descriptor rather than from the
  // configured source mode, because the descriptor is what the samples
  // actually are. Complex IQ goes to SigMF with both components intact;
  // audio keeps the existing, unchanged WAV path.
  const bool complex_iq =
      block.stream.kind == cwassistant::core::StreamKind::ComplexIq;
  if (capture_writer_pending_) {
    if (complex_iq) {
      if (!openIqCapture(block)) return;
    } else if (capture_writer_.open(capture_wav_path_.toStdString(),
                                    block.stream.sample_rate_hz)) {
      capture_writer_pending_ = false;
    } else {
      finishDebugCapture(QStringLiteral("Could not open capture audio file"));
      return;
    }
  }
  // Switching receivers mid-capture would append samples of one kind to a file
  // describing the other. End the recording instead of corrupting it.
  if (complex_iq != capture_iq_writer_.isOpen()) {
    finishDebugCapture(
        QStringLiteral("The receiver source changed during capture"));
    return;
  }
  if (!capture_have_start_) {
    capture_start_ns_ = block.timestamp_ns;
    capture_last_snapshot_ns_ = block.timestamp_ns;
    capture_have_start_ = true;
  }
  if (complex_iq) {
    capture_iq_center_frequency_hz_ = block.stream.center_frequency_hz;
    if (!capture_iq_writer_.writeBlock(block)) {
      // The writer stops for a configured bound as readily as for a fault, so
      // report its own reason rather than assuming the capture filled up.
      const auto reason = capture_iq_writer_.stopReasonText();
      finishDebugCapture(QString::fromUtf8(
          reason.data(), static_cast<qsizetype>(reason.size())));
      return;
    }
  } else if (!capture_writer_.writeBlock(block)) {
    finishDebugCapture(QStringLiteral("Capture reached its maximum size"));
    return;
  }
  const double elapsed_seconds =
      static_cast<double>(block.timestamp_ns - capture_start_ns_) /
      1'000'000'000.0;
  if (elapsed_seconds >= maximum_capture_seconds_) {
    finishDebugCapture(QStringLiteral("Reached the %1-minute capture limit")
                           .arg(maximum_capture_seconds_ / 60.0, 0, 'g', 2));
  } else if (static_cast<double>(block.timestamp_ns -
                                 capture_last_snapshot_ns_) /
                 1'000'000'000.0 >=
             kSnapshotIntervalSeconds) {
    capture_last_snapshot_ns_ = block.timestamp_ns;
    writeDebugCaptureSnapshot();
    emit debugCaptureStateChanged(true, capture_base_path_, elapsed_seconds,
                                  QStringLiteral("Recording"));
  }
}

void LiveAudioDspWorker::setRemoteAudioSubscribed(const bool subscribed) {
  if (remote_audio_subscribed_ == subscribed) return;
  remote_audio_subscribed_ = subscribed;
  // A subscriber arriving is a resumption, not a continuation: nothing was
  // demodulated while nobody was listening, so the first thing they receive
  // must not be built on a delay line holding samples from minutes ago.
  resetRegionAudio();
}

void LiveAudioDspWorker::noteMonitorAudioConsumed() noexcept {
  const int previous =
      monitor_buffers_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
  // Never negative. A buffer acknowledged after a monitor-mode change, or one
  // still queued when the count was cleared, would otherwise leave a permanent
  // credit that defeats the bound for the rest of the session.
  if (previous <= 0) {
    monitor_buffers_in_flight_.store(0, std::memory_order_release);
  }
}

bool LiveAudioDspWorker::regionAudioWanted() const noexcept {
  // Three consumers, one producer, and no fourth reason to run. Every term
  // here is a fact about somebody actually wanting the audio right now rather
  // than about a permission or a capability: an allowed-but-unused remote
  // stream, or a monitor pointed at one track, must cost nothing.
  return monitor_mode_ == 3 || remote_audio_subscribed_ || capture_active_;
}

void LiveAudioDspWorker::resetRegionAudio() noexcept {
  region_demodulator_.reset();
  // Forces produceRegionAudio() to reconfigure before it emits again, which is
  // what re-establishes the shift frequency after a bandwidth change.
  region_audio_active_ = false;
  region_audio_.clear();
}

void LiveAudioDspWorker::emitMonitorAudio(const QByteArray& audio,
                                          const double sample_rate_hz,
                                          const bool bounded) {
  if (audio.isEmpty()) return;
  if (bounded) {
    if (monitor_buffers_in_flight_.load(std::memory_order_acquire) >=
        kMaximumMonitorBuffersInFlight) {
      ++dropped_region_audio_buffers_;
      return;
    }
    monitor_buffers_in_flight_.fetch_add(1, std::memory_order_acq_rel);
  }
  emit monitorAudioProduced(audio, sample_rate_hz);
}

void LiveAudioDspWorker::produceRegionAudio(
    const cwassistant::core::RealtimeSampleBlock& block) {
  if (!regionAudioWanted()) {
    // Not "compute and discard". Nothing below this line runs, the demodulator
    // keeps no state across the silence, and the next consumer to appear
    // starts it cleanly.
    if (region_audio_active_) resetRegionAudio();
    return;
  }
  if (block.sample_count == 0 ||
      block.stream.kind != cwassistant::core::StreamKind::ComplexIq) {
    return;
  }
  const double bandwidth_hz = sdr_decoder_bandwidth_hz_;
  const double input_rate_hz = block.stream.sample_rate_hz;
  if (!region_audio_active_ ||
      region_demodulator_.config().bandwidth_hz != bandwidth_hz ||
      region_demodulator_.config().input_sample_rate_hz != input_rate_hz) {
    if (!region_demodulator_.configure(
            {.bandwidth_hz = bandwidth_hz,
             .input_sample_rate_hz = input_rate_hz})) {
      // A region no supported audio rate can carry, or a stream too narrow to
      // hold the window. Silent rather than fatal: the decoder is unaffected,
      // and the operator's remedy is to narrow the window.
      region_audio_active_ = false;
      return;
    }
    region_audio_active_ = true;
  }
  region_audio_.clear();
  region_audio_.reserve(
      region_demodulator_.maximumOutputSamples(block.sample_count));
  region_demodulator_.process(block.samples.data(), block.sample_count,
                              region_audio_);
  region_audio_samples_ += region_audio_.size();
  if (region_audio_.empty()) return;
  const double rate_hz = region_demodulator_.outputSampleRateHz();
  // Built once for both byte consumers. The monitor and the remote stream want
  // identical bytes, and QByteArray is copy-on-write, so the second consumer
  // costs a reference count rather than a second copy of the audio.
  const QByteArray audio = monitor_bytes(region_audio_);
  if (monitor_mode_ == 3) emitMonitorAudio(audio, rate_hz, true);
  // Emitted on this thread through a direct relay, straight into a sender that
  // bounds itself. Nothing is queued upstream of that bound, which is the
  // whole reason the relay is direct.
  if (remote_audio_subscribed_) emit receiveAudioProduced(audio, rate_hz);
  captureRegionAudio(region_audio_, rate_hz);
}

void LiveAudioDspWorker::captureRegionAudio(const std::vector<float>& audio,
                                            const double sample_rate_hz) {
  if (!capture_active_ || capture_region_audio_failed_ || audio.empty()) return;
  if (!capture_writer_.isOpen()) {
    // Opened here rather than in captureBlock(), because the audio does not
    // exist until the demodulator has been configured for this stream and it
    // is the DEMODULATED rate, not the receiver's, that the file has to
    // declare. A WAV header naming the IQ rate would play back at the wrong
    // speed and pitch, which is worse than no file.
    if (!capture_writer_.open(capture_region_audio_path_.toStdString(),
                              sample_rate_hz)) {
      capture_region_audio_failed_ = true;
      return;
    }
  }
  cwassistant::core::RealtimeSampleBlock chunk;
  chunk.stream = {.kind = cwassistant::core::StreamKind::Audio,
                  .sample_rate_hz = sample_rate_hz,
                  .center_frequency_hz = 0.0,
                  .channel_count = 1};
  for (std::size_t offset = 0; offset < audio.size();
       offset += chunk.samples.size()) {
    chunk.sample_count = std::min(chunk.samples.size(), audio.size() - offset);
    for (std::size_t index = 0; index < chunk.sample_count; ++index) {
      chunk.samples[index] = {audio[offset + index], 0.0F};
    }
    if (!capture_writer_.writeBlock(chunk)) {
      // Only the audio stops. The IQ recording is the primary payload and its
      // own bound is the one that decides how long a capture lasts.
      capture_region_audio_failed_ = true;
      return;
    }
  }
}

void LiveAudioDspWorker::drain() {
  QElapsedTimer drain_clock;
  drain_clock.start();
  ++drain_calls_;
  cwassistant::core::RealtimeSampleBlock block;
  int drained = 0;
  while (drained < 32 && pipe_->blocks.try_pop(block)) {
    ++drained;
    processing_complex_iq_ =
        block.stream.kind == cwassistant::core::StreamKind::ComplexIq;
    const std::size_t wanted_fft_size =
        block.stream.kind == cwassistant::core::StreamKind::ComplexIq ? 16'384U
                                                                      : 2'048U;
    if (analyzer_.config().fft_size != wanted_fft_size) {
      auto config = analyzer_.config();
      config.fft_size = wanted_fft_size;
      static_cast<void>(analyzer_.configure(config));
    }
    auto snapshots = analyzer_.process(block);
    captureBlock(block);
    cwassistant::core::RealtimeSampleBlock decoder_block;
    cwassistant::core::RealtimeSampleBlock decoder_carry;
    const cwassistant::core::RealtimeSampleBlock* processing_block = &block;
    std::vector<cwassistant::core::SpectrumSnapshot> decoder_snapshots;
    if (block.stream.kind == cwassistant::core::StreamKind::ComplexIq) {
      // The arrival of a complex block is what makes a requested decoder
      // window applicable, so this is where a window requested before there
      // was any IQ to apply it to takes effect. Without it the first window of
      // a session -- published by the controller at SDR start, before the
      // receiver has delivered anything -- would stay a request forever, the
      // channelizer would keep its defaults, and process() below would refuse
      // every block while the overview spectrum painted normally.
      if (sdrDecoderWindowNeedsApply()) applySdrDecoderWindow();
      const auto status =
          sdr_decoder_channelizer_.process(block, decoder_block);
      if (status != cwassistant::core::IqBlockStatus::Accepted &&
          status !=
              cwassistant::core::IqBlockStatus::AcceptedAfterDiscontinuity) {
        // The wide overview remains live even if the configured decoder slice
        // is temporarily outside the acquired hardware passband.
        for (auto& snapshot : snapshots) {
          QVector<float> bins(
              static_cast<qsizetype>(snapshot.bins_dbfs.size()));
          std::copy(snapshot.bins_dbfs.cbegin(), snapshot.bins_dbfs.cend(),
                    bins.begin());
          QVector<float> instantaneous_bins(
              static_cast<qsizetype>(snapshot.instantaneous_bins_dbfs.size()));
          std::copy(snapshot.instantaneous_bins_dbfs.cbegin(),
                    snapshot.instantaneous_bins_dbfs.cend(),
                    instantaneous_bins.begin());
          static_cast<void>(publishSpectrumFrame(SpectrumFrame{
              .bins_dbfs = std::move(bins),
              .sequence = snapshot.sequence,
              .timestamp_ns = snapshot.timestamp_ns,
              .lower_frequency_hz = snapshot.lower_frequency_hz,
              .upper_frequency_hz = snapshot.upper_frequency_hz,
              .instantaneous_bins_dbfs = std::move(instantaneous_bins),
          }));
        }
        continue;
      }
      if (status ==
          cwassistant::core::IqBlockStatus::AcceptedAfterDiscontinuity) {
        sdr_decoder_pending_ = {};
        // The samples after a discontinuity do not continue the ones before
        // it, and the demodulator's delay line is half a millisecond of
        // exactly those.
        resetRegionAudio();
      }
      if (decoder_block.sample_count > 0) {
        if (sdr_decoder_pending_.sample_count > 0 &&
            (sdr_decoder_pending_.stream.sample_rate_hz !=
                 decoder_block.stream.sample_rate_hz ||
             sdr_decoder_pending_.stream.center_frequency_hz !=
                 decoder_block.stream.center_frequency_hz)) {
          sdr_decoder_pending_ = {};
        }
        if (sdr_decoder_pending_.sample_count == 0) {
          sdr_decoder_pending_.stream = decoder_block.stream;
          sdr_decoder_pending_.timestamp_ns = decoder_block.timestamp_ns;
          sdr_decoder_pending_.sequence = sdr_decoder_pending_sequence_++;
        }
        const std::size_t available = sdr_decoder_pending_.samples.size() -
                                      sdr_decoder_pending_.sample_count;
        const std::size_t copied =
            std::min(available, decoder_block.sample_count);
        std::copy_n(
            decoder_block.samples.cbegin(), copied,
            sdr_decoder_pending_.samples.begin() +
                static_cast<std::ptrdiff_t>(sdr_decoder_pending_.sample_count));
        sdr_decoder_pending_.sample_count += copied;
        if (copied < decoder_block.sample_count) {
          decoder_carry.stream = decoder_block.stream;
          decoder_carry.timestamp_ns =
              decoder_block.timestamp_ns +
              static_cast<std::uint64_t>(static_cast<long double>(copied) *
                                         1'000'000'000.0L /
                                         decoder_block.stream.sample_rate_hz);
          decoder_carry.sequence = sdr_decoder_pending_sequence_++;
          decoder_carry.sample_count = decoder_block.sample_count - copied;
          std::copy_n(decoder_block.samples.cbegin() +
                          static_cast<std::ptrdiff_t>(copied),
                      decoder_carry.sample_count,
                      decoder_carry.samples.begin());
        }
      }
      // Multi-MHz devices can yield only a few dozen decimated samples per
      // hardware block. Batch them so the decoder, card model, and monitor do
      // not receive thousands of tiny queued updates per second.
      if (sdr_decoder_pending_.sample_count < 512) {
        for (auto& snapshot : snapshots) {
          QVector<float> bins(
              static_cast<qsizetype>(snapshot.bins_dbfs.size()));
          std::copy(snapshot.bins_dbfs.cbegin(), snapshot.bins_dbfs.cend(),
                    bins.begin());
          QVector<float> instantaneous_bins(
              static_cast<qsizetype>(snapshot.instantaneous_bins_dbfs.size()));
          std::copy(snapshot.instantaneous_bins_dbfs.cbegin(),
                    snapshot.instantaneous_bins_dbfs.cend(),
                    instantaneous_bins.begin());
          static_cast<void>(publishSpectrumFrame(SpectrumFrame{
              .bins_dbfs = std::move(bins),
              .sequence = snapshot.sequence,
              .timestamp_ns = snapshot.timestamp_ns,
              .lower_frequency_hz = snapshot.lower_frequency_hz,
              .upper_frequency_hz = snapshot.upper_frequency_hz,
              .instantaneous_bins_dbfs = std::move(instantaneous_bins),
          }));
        }
        continue;
      }
      processing_block = &sdr_decoder_pending_;
      decoder_snapshots = decoder_analyzer_.process(*processing_block);
    } else {
      decoder_snapshots = snapshots;
    }
    for (const auto& snapshot : decoder_snapshots) {
      // Detection consumes the unaveraged bins and applies its own fixed-time
      // smoothing, so the operator's display averaging cannot change which
      // signals are discovered or how quickly they qualify.
      if (block.stream.kind != cwassistant::core::StreamKind::ComplexIq) {
        ++detector_frames_;
        static_cast<void>(decoder_.updateSpectrum(
            snapshot.timestamp_ns, snapshot.lower_frequency_hz,
            snapshot.upper_frequency_hz, snapshot.instantaneous_bins_dbfs,
            false));
        continue;
      }
      const double bin_width_hz = snapshot.bin_width_hz;
      const double requested_lower_hz =
          sdr_decoder_center_frequency_hz_ - sdr_decoder_bandwidth_hz_ * 0.5;
      const double requested_upper_hz =
          sdr_decoder_center_frequency_hz_ + sdr_decoder_bandwidth_hz_ * 0.5;
      const auto first_bin = static_cast<std::size_t>(std::clamp(
          std::ceil((requested_lower_hz - snapshot.lower_frequency_hz) /
                    bin_width_hz),
          0.0,
          static_cast<double>(snapshot.instantaneous_bins_dbfs.size() - 1U)));
      const auto last_bin = static_cast<std::size_t>(std::clamp(
          std::floor((requested_upper_hz - snapshot.lower_frequency_hz) /
                     bin_width_hz),
          static_cast<double>(first_bin),
          static_cast<double>(snapshot.instantaneous_bins_dbfs.size() - 1U)));
      const double detector_lower_hz =
          snapshot.lower_frequency_hz +
          static_cast<double>(first_bin) * bin_width_hz;
      const auto detector_bins = std::span<const float>(
          snapshot.instantaneous_bins_dbfs.data() + first_bin,
          last_bin - first_bin + 1U);
      ++detector_frames_;
      static_cast<void>(decoder_.updateSpectrum(
          snapshot.timestamp_ns, detector_lower_hz,
          detector_lower_hz +
              static_cast<double>(detector_bins.size()) * bin_width_hz,
          detector_bins, false));
    }
    if (pending_manual_frequency_hz_.has_value() &&
        !decoder_snapshots.empty()) {
      const std::uint64_t channel_id =
          decoder_.selectFrequency(*pending_manual_frequency_hz_);
      pending_manual_frequency_hz_.reset();
      if (channel_id != 0) {
        emit decoderProduced(decoderChannelModel(decoder_.channels()));
        emit manualDecoderSelected(static_cast<qulonglong>(channel_id));
      }
    }
    // Called for its effect on the bank, not for its return: the model is
    // published from the bank's own channels after the loop now, so binding
    // the result here only earned an unused-variable warning.
    static_cast<void>(decoder_.processSamples(*processing_block));
    // The one demodulation of the decode region, feeding the local monitor,
    // the remote stream and the debug capture from the same samples. Placed
    // here because `processing_block` IS the decode region: the channelizer
    // has already centred and bounded it, so what an operator hears is exactly
    // what the decoder is reading.
    produceRegionAudio(*processing_block);
    const auto& raw_monitor_audio = decoder_.monitorAudio();
    if (!raw_monitor_audio.empty() &&
        processing_block->stream.kind ==
            cwassistant::core::StreamKind::ComplexIq) {
      // A raw IQ passband is not meaningful loudspeaker audio. Selected-track
      // monitoring is already narrow-filtered and re-pitched by the channel
      // bank; downsample only that result to a widely supported audio rate.
      if (monitor_mode_ == 2) {
        const double monitor_output_rate_hz =
            std::min(48'000.0, processing_block->stream.sample_rate_hz);
        if (monitor_resample_input_rate_hz_ !=
            processing_block->stream.sample_rate_hz) {
          monitor_resample_phase_ = 0.0;
          monitor_resample_sum_ = 0.0F;
          monitor_resample_count_ = 0;
          monitor_resample_input_rate_hz_ =
              processing_block->stream.sample_rate_hz;
        }
        std::vector<float> resampled;
        resampled.reserve(static_cast<std::size_t>(std::ceil(
            static_cast<double>(raw_monitor_audio.size()) *
            monitor_output_rate_hz / processing_block->stream.sample_rate_hz)));
        for (const float sample : raw_monitor_audio) {
          monitor_resample_sum_ += sample;
          ++monitor_resample_count_;
          monitor_resample_phase_ += monitor_output_rate_hz;
          if (monitor_resample_phase_ >=
              processing_block->stream.sample_rate_hz) {
            monitor_resample_phase_ -= processing_block->stream.sample_rate_hz;
            resampled.push_back(monitor_resample_sum_ /
                                static_cast<float>(monitor_resample_count_));
            monitor_resample_sum_ = 0.0F;
            monitor_resample_count_ = 0;
          }
        }
        emitMonitorAudio(monitor_bytes(resampled), monitor_output_rate_hz,
                         false);
      }
    } else {
      emitMonitorAudio(monitor_bytes(raw_monitor_audio),
                       processing_block->stream.sample_rate_hz, false);
    }
    const auto& character_tracks = decoder_.characterRefinementTracks();
    for (auto& window :
         character_frontends_.process(*processing_block, character_tracks))
      emit characterWindowProduced(0, std::move(window));
    for (auto& snapshot : snapshots) {
      QVector<float> bins(static_cast<qsizetype>(snapshot.bins_dbfs.size()));
      std::copy(snapshot.bins_dbfs.cbegin(), snapshot.bins_dbfs.cend(),
                bins.begin());
      QVector<float> instantaneous_bins(
          static_cast<qsizetype>(snapshot.instantaneous_bins_dbfs.size()));
      std::copy(snapshot.instantaneous_bins_dbfs.cbegin(),
                snapshot.instantaneous_bins_dbfs.cend(),
                instantaneous_bins.begin());
      static_cast<void>(publishSpectrumFrame(SpectrumFrame{
          .bins_dbfs = std::move(bins),
          .sequence = snapshot.sequence,
          .timestamp_ns = snapshot.timestamp_ns,
          .lower_frequency_hz = snapshot.lower_frequency_hz,
          .upper_frequency_hz = snapshot.upper_frequency_hz,
          .instantaneous_bins_dbfs = std::move(instantaneous_bins),
      }));
    }
    if (block.stream.kind != cwassistant::core::StreamKind::ComplexIq ||
        !decoder_snapshots.empty())
      decoder_model_dirty_ = true;
    if (block.stream.kind == cwassistant::core::StreamKind::ComplexIq)
      sdr_decoder_pending_ = std::move(decoder_carry);
  }
  // Published after the loop, not inside it, and no faster than an operator
  // can see. The model is a snapshot of the current channels, so the latest
  // one says everything the intermediate ones would have.
  if (decoder_model_dirty_ &&
      (!model_publish_clock_.isValid() ||
       model_publish_clock_.elapsed() >= kModelPublishIntervalMs)) {
    model_publish_clock_.restart();
    decoder_model_dirty_ = false;
    ++model_publishes_;
    emit decoderProduced(decoderChannelModel(decoder_.channels()));
  }
  if (drained > 0 &&
      (!diagnostics_publish_clock_.isValid() ||
       diagnostics_publish_clock_.elapsed() >= kDiagnosticsPublishIntervalMs)) {
    diagnostics_publish_clock_.restart();
    emit diagnosticsProduced(
        verificationDiagnosticsModel(decoder_.verificationDiagnostics()));
  }
  blocks_drained_ += static_cast<std::uint64_t>(drained);
  // Hitting the cap means blocks were still waiting when the drain gave up,
  // which is the one honest sign that samples are arriving faster than they
  // are being consumed.
  if (drained >= 32) ++drain_capped_;
  const auto elapsed_us =
      static_cast<std::uint64_t>(std::max<qint64>(0, drain_clock.nsecsElapsed() / 1'000));
  drain_micros_total_ += elapsed_us;
  // Each reader keeps its own maximum. A peak is not a difference of totals,
  // so it cannot be recovered from a shared counter once the other reader has
  // cleared it -- and a capture whose worst drain had been consumed by a
  // stream record would under-report exactly the number it exists to show.
  capture_diagnostics_window_.peak_micros =
      std::max(capture_diagnostics_window_.peak_micros, elapsed_us);
  live_diagnostics_window_.peak_micros =
      std::max(live_diagnostics_window_.peak_micros, elapsed_us);
}

}  // namespace cwassistant::desktop
