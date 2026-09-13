#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonObject>
#include <QMetaObject>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <numbers>
#include <string_view>
#include <vector>

#include "replay/live_audio_worker.hpp"

namespace {

// Codes from 20 up, so a failure in this phase is never confused with one in
// the audio phase below.
constexpr int kNoDiagnosticsRecord = 20;
constexpr int kDecoderWindowNotInForce = 21;
constexpr int kGuiBlockMissing = 22;
constexpr int kGuiLatenessNotReported = 23;
constexpr int kGuiPeakNotWindowScoped = 24;

// Region audio, from 30 up.
constexpr int kRegionAudioWithoutConsumer = 30;
constexpr int kRegionMonitorSilent = 31;
constexpr int kRegionMonitorWrongRate = 32;
constexpr int kRegionMonitorWrongPitch = 33;
constexpr int kRegionMonitorMirrored = 34;
constexpr int kRemoteAudioSilent = 35;
constexpr int kRemoteAudioWrongRate = 36;
constexpr int kCaptureIqMissing = 37;
constexpr int kCaptureRegionAudioMissing = 38;
constexpr int kRegionAudioNotReported = 39;
constexpr int kRegionMonitorUnbounded = 40;
// The record's own account of whether the region is running. A station
// reported `active:false, wanted:false` in the same record as a non-zero
// sample count while a remote observer was listening, which is a record that
// contradicts itself: a reader cannot tell from it whether region audio is off
// or whether the flags are simply wrong, and those call for opposite remedies.
// Only `samples` was ever asserted here, so nothing checked that the two
// booleans agree with the work actually being done.
constexpr int kRegionAudioFlagsWrong = 41;

// Two properties of the live DSP worker that only a complex-IQ session can
// show, driven entirely through the block pipe so no receiver is required:
//
//  1. A decoder window published BEFORE the first IQ block -- which is what
//     the controller does at SDR start -- is in force by the time IQ is
//     processed. It regressed into a window that was remembered but never
//     configured: the channelizer kept its defaults and refused every block,
//     so the overview spectrum painted normally while nothing decoded, until
//     the operator dragged the window and changed the requested values. The
//     assertion is that spectrum frames reach the detector at all, which can
//     only happen once the channelizer admits a block.
//
//  2. The GUI-thread lateness block is reported, and is scoped to one record
//     window: a peak is a maximum, not a difference of totals, so a peak that
//     is never cleared would mark every later record as stalled.
// Spectrum frames must be dropped, not queued, when the display is behind.
//
// Frames were emitted with no backpressure at all. A wide IQ transform is 8193
// bins and each frame carries two float vectors of them, so one queued frame is
// about 64 kB and thirty a second is two megabytes a second. An operator
// watched memory climb from 244 MB to 668 MB -- a little over three minutes of
// exactly that -- and then had to kill the application, because the growing
// backlog made the thread that draws fall further behind, which grew the
// backlog. A display frame nobody drew is worth nothing, so refusing it costs
// only the backlog it would have added.
int runSpectrumBackpressureChecks() {
  auto pipe = std::make_shared<cwassistant::desktop::LiveAudioPipe>();
  QThread dsp_thread;
  auto* worker = new cwassistant::desktop::LiveAudioDspWorker(pipe);
  worker->moveToThread(&dsp_thread);
  QObject::connect(&dsp_thread, &QThread::finished, worker,
                   &QObject::deleteLater);

  // Deliberately never acknowledged, which is what a stalled display is.
  int delivered = 0;
  QObject::connect(
      worker, &cwassistant::desktop::LiveAudioDspWorker::frameProduced,
      &dsp_thread, [&delivered](const cwassistant::desktop::SpectrumFrame&) {
        ++delivered;
      },
      Qt::DirectConnection);

  dsp_thread.start();
  QMetaObject::invokeMethod(worker, "start", Qt::BlockingQueuedConnection);

  // Far more audio than the bound, delivered in one go.
  constexpr int kBlocks = 400;
  for (int index = 0; index < kBlocks; ++index) {
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = 48'000.0;
    block.stream.kind = cwassistant::core::StreamKind::Audio;
    block.timestamp_ns =
        static_cast<std::uint64_t>(index) * 4'000'000ULL;
    block.sample_count = 192;
    for (std::size_t sample = 0; sample < block.sample_count; ++sample) {
      block.samples[sample] = {0.2F, 0.0F};
    }
    static_cast<void>(pipe->blocks.try_push(block));
    QMetaObject::invokeMethod(worker, "drain", Qt::BlockingQueuedConnection);
  }

  QMetaObject::invokeMethod(worker, "stop", Qt::BlockingQueuedConnection);
  dsp_thread.quit();
  dsp_thread.wait();

  // The bound is what matters: an unacknowledged display can never accumulate
  // more than a couple of frames, however long reception runs. Without it this
  // count rises with the block count and the queue with it.
  if (delivered >
      cwassistant::desktop::LiveAudioDspWorker::kMaximumSpectrumFramesInFlight) {
    std::fprintf(stderr,
                 "spectrum frames were not bounded: %d delivered to a display "
                 "that acknowledged none\n",
                 delivered);
    return 20;
  }
  return 0;
}

// One demodulation of the decode region, three consumers, and nothing at all
// when none of them wants it.
//
// The three are the local monitor (listen mode 3), a remote observer being sent
// receive audio, and a running debug capture. An SDR capture used to contain no
// listenable audio whatever -- the capture wrote EITHER audio.wav OR the SigMF
// IQ, and an SDR session wrote the IQ -- so an operator recording a signal that
// would not decode had nothing to play back beside it.
int runRegionAudioChecks() {
  auto pipe = std::make_shared<cwassistant::desktop::LiveAudioPipe>();
  QThread dsp_thread;
  auto* worker = new cwassistant::desktop::LiveAudioDspWorker(pipe);
  worker->moveToThread(&dsp_thread);
  QObject::connect(&dsp_thread, &QThread::finished, worker,
                   &QObject::deleteLater);

  std::vector<float> monitor_audio;
  double monitor_rate_hz = 0.0;
  int monitor_buffers = 0;
  bool acknowledge_monitor = true;
  std::vector<float> remote_audio;
  double remote_rate_hz = 0.0;
  const auto append = [](std::vector<float>& destination,
                         const QByteArray& bytes) {
    const auto count =
        static_cast<std::size_t>(bytes.size() / static_cast<qsizetype>(sizeof(float)));
    const std::size_t old_size = destination.size();
    destination.resize(old_size + count);
    std::memcpy(destination.data() + old_size, bytes.constData(),
                count * sizeof(float));
  };
  // Direct, so every buffer is observed on the worker's own thread inside the
  // blocking invocation that produced it. The monitor is acknowledged as it is
  // taken, exactly as the controller acknowledges it, because the region path
  // is bounded and an unacknowledged consumer would be cut off after eight
  // buffers -- which is the bound working, not a fault.
  QObject::connect(
      worker, &cwassistant::desktop::LiveAudioDspWorker::monitorAudioProduced,
      worker,
      [&](const QByteArray& bytes, const double rate_hz) {
        monitor_rate_hz = rate_hz;
        ++monitor_buffers;
        append(monitor_audio, bytes);
        if (acknowledge_monitor) worker->noteMonitorAudioConsumed();
      },
      Qt::DirectConnection);
  QObject::connect(
      worker, &cwassistant::desktop::LiveAudioDspWorker::receiveAudioProduced,
      worker,
      [&](const QByteArray& bytes, const double rate_hz) {
        remote_rate_hz = rate_hz;
        append(remote_audio, bytes);
      },
      Qt::DirectConnection);
  QString capture_base_path;
  QObject::connect(
      worker,
      &cwassistant::desktop::LiveAudioDspWorker::debugCaptureStateChanged,
      worker,
      [&capture_base_path](const bool, const QString& path, const double,
                           const QString&) {
        if (!path.isEmpty()) capture_base_path = path;
      },
      Qt::DirectConnection);
  QJsonObject record;
  QObject::connect(
      worker,
      &cwassistant::desktop::LiveAudioDspWorker::diagnosticsRecordProduced,
      worker, [&record](const QJsonObject& published) { record = published; },
      Qt::DirectConnection);
  // The demodulator's own sample counter, which is what makes "no work at all"
  // an assertion rather than a claim. An emitted-buffer count cannot see the
  // difference between a producer that did not run and one that ran and threw
  // the result away, and the second is the fault worth catching: doing work at
  // the rate data arrives rather than the rate a consumer needs is what this
  // codebase keeps getting wrong.
  const auto region_audio_samples = [&]() -> qint64 {
    QMetaObject::invokeMethod(worker, "publishLiveDiagnosticsRecord",
                              Qt::BlockingQueuedConnection);
    return record.value(QStringLiteral("regionAudio"))
        .toObject()
        .value(QStringLiteral("samples"))
        .toInteger(-1);
  };
  // Reads the flag out of the record last published by region_audio_samples(),
  // so one blocking round trip answers for all three fields and they can only
  // describe the same instant.
  const auto region_audio_flag = [&record](const char* name) {
    return record.value(QStringLiteral("regionAudio"))
        .toObject()
        .value(QString::fromLatin1(name))
        .toBool();
  };
  // The two booleans against the work: `wanted` is whether a consumer exists
  // and `active` is whether the demodulator is configured and running for it,
  // so both must agree with whether samples are being produced at all.
  const auto region_flags_agree = [&](const bool expected) {
    const bool wanted = region_audio_flag("wanted");
    const bool active = region_audio_flag("active");
    if (wanted == expected && active == expected) return true;
    qCritical().noquote()
        << "regionAudio flags disagree with the work being done: expected"
        << expected << "wanted=" << wanted << "active=" << active
        << "samples=" << record.value(QStringLiteral("regionAudio"))
                              .toObject()
                              .value(QStringLiteral("samples"))
                              .toInteger(-1);
    return false;
  };

  constexpr double kIqSampleRateHz = 240'000.0;
  constexpr double kIqCenterHz = 14'050'000.0;
  constexpr double kRegionBandwidthHz = 24'000.0;
  // A carrier 4 kHz above the region centre. Region audio shifts the region up
  // by half its width, so this station has to be heard at 12 + 4 = 16 kHz and
  // at nothing else -- in particular not at 12 - 4 = 8 kHz, which is where it
  // would land if the shift went the other way or the analytic property were
  // lost on the way.
  constexpr double kCarrierOffsetHz = 4'000.0;
  constexpr double kExpectedAudioHz =
      kRegionBandwidthHz * 0.5 + kCarrierOffsetHz;
  constexpr double kMirrorAudioHz =
      kRegionBandwidthHz * 0.5 - kCarrierOffsetHz;
  constexpr std::size_t kIqBlockSamples = 2'048;
  constexpr std::size_t kIqBlockCount = 160;
  std::vector<cwassistant::core::RealtimeSampleBlock> iq_blocks(kIqBlockCount);
  double phase = 0.0;
  for (std::size_t index = 0; index < iq_blocks.size(); ++index) {
    auto& block = iq_blocks[index];
    block.stream.kind = cwassistant::core::StreamKind::ComplexIq;
    block.stream.sample_rate_hz = kIqSampleRateHz;
    block.stream.center_frequency_hz = kIqCenterHz;
    block.stream.channel_count = 1;
    block.sequence = index;
    block.timestamp_ns = static_cast<std::uint64_t>(
        static_cast<long double>(index * kIqBlockSamples) * 1'000'000'000.0L /
        kIqSampleRateHz);
    block.sample_count = kIqBlockSamples;
    for (std::size_t sample = 0; sample < block.sample_count; ++sample) {
      block.samples[sample] = {0.5F * static_cast<float>(std::cos(phase)),
                               0.5F * static_cast<float>(std::sin(phase))};
      phase += 2.0 * std::numbers::pi * kCarrierOffsetHz / kIqSampleRateHz;
    }
  }
  std::size_t next_block = 0;
  const auto feed = [&](const std::size_t blocks) {
    for (std::size_t fed = 0; fed < blocks && next_block < iq_blocks.size();) {
      std::size_t pushed = 0;
      while (fed < blocks && next_block < iq_blocks.size() && pushed < 8 &&
             pipe->blocks.try_push(iq_blocks[next_block])) {
        ++next_block;
        ++fed;
        ++pushed;
      }
      QMetaObject::invokeMethod(worker, "drain", Qt::BlockingQueuedConnection);
    }
  };
  const auto magnitude_at = [](const std::vector<float>& audio,
                               const double rate_hz, const double frequency_hz,
                               const std::size_t skip) {
    if (audio.size() <= skip) return 0.0;
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t index = skip; index < audio.size(); ++index) {
      const double angle = 2.0 * std::numbers::pi * frequency_hz *
                           static_cast<double>(index) / rate_hz;
      real += static_cast<double>(audio[index]) * std::cos(angle);
      imaginary -= static_cast<double>(audio[index]) * std::sin(angle);
    }
    return std::hypot(real, imaginary) /
           static_cast<double>(audio.size() - skip);
  };

  QTemporaryDir capture_root;
  dsp_thread.start();
  const int result = [&]() -> int {
    QMetaObject::invokeMethod(worker, "start", Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(worker, "setSdrDecoderWindow",
                              Qt::BlockingQueuedConnection,
                              Q_ARG(double, kIqCenterHz),
                              Q_ARG(double, kRegionBandwidthHz));

    // 1. Nobody is listening. The monitor is off, no observer has subscribed
    //    and no capture is running, so not a single sample must be demodulated
    //    -- this is the fault this codebase keeps repeating, where work is done
    //    at the rate data arrives rather than the rate a consumer needs.
    feed(32);
    const qint64 idle_samples = region_audio_samples();
    if (idle_samples < 0) {
      qCritical().noquote() << "no region audio block in the record:" << record;
      return kRegionAudioNotReported;
    }
    if (idle_samples != 0 || !monitor_audio.empty() || !remote_audio.empty()) {
      qCritical().noquote()
          << "region audio produced with no consumer: demodulated="
          << idle_samples << "monitor=" << monitor_audio.size()
          << "remote=" << remote_audio.size();
      return kRegionAudioWithoutConsumer;
    }
    if (!region_flags_agree(false)) return kRegionAudioFlagsWrong;

    // 2. The local monitor, in region mode.
    QMetaObject::invokeMethod(worker, "setMonitor", Qt::BlockingQueuedConnection,
                              Q_ARG(int, 3), Q_ARG(QVariantList, QVariantList{}),
                              Q_ARG(double, 700.0));
    feed(32);
    if (monitor_audio.empty() || region_audio_samples() <= 0) {
      return kRegionMonitorSilent;
    }
    // The REGION button's own state, as the record reports it. An operator who
    // cannot hear the region reads this record to find out whether the station
    // thinks it is playing, so it must say yes while it is.
    if (!region_flags_agree(true)) return kRegionAudioFlagsWrong;
    // The rate invariant, at the one place an operator can hear it break: real
    // audio sampled at Fs carries only Fs/2, so a 24 kHz region needs at least
    // 48 kHz or its top half folds onto its bottom half.
    if (monitor_rate_hz < 2.0 * kRegionBandwidthHz) {
      qCritical().noquote() << "region audio rate" << monitor_rate_hz
                            << "cannot carry a" << kRegionBandwidthHz
                            << "Hz region";
      return kRegionMonitorWrongRate;
    }
    const std::size_t skip = std::min<std::size_t>(4'096, monitor_audio.size() / 4);
    const double wanted = magnitude_at(monitor_audio, monitor_rate_hz,
                                       kExpectedAudioHz, skip);
    const double mirror =
        magnitude_at(monitor_audio, monitor_rate_hz, kMirrorAudioHz, skip);
    if (wanted < 0.05) {
      qCritical().noquote() << "region monitor carries no tone at"
                            << kExpectedAudioHz << "Hz:" << wanted;
      return kRegionMonitorWrongPitch;
    }
    if (mirror > wanted * 0.05) {
      qCritical().noquote() << "region monitor is mirrored: wanted=" << wanted
                            << "mirror=" << mirror;
      return kRegionMonitorMirrored;
    }

    // 3. The bound. Region audio crosses to the thread that plays, and that
    //    thread can stall; a producer that keeps emitting into a queue nobody
    //    drains is how this application once grew to 668 MB and had to be
    //    killed. A player that acknowledges nothing must stop receiving, not
    //    accumulate.
    acknowledge_monitor = false;
    monitor_buffers = 0;
    feed(32);
    if (monitor_buffers >
        cwassistant::desktop::LiveAudioDspWorker::
            kMaximumMonitorBuffersInFlight) {
      qCritical().noquote()
          << "region audio was not bounded:" << monitor_buffers
          << "buffers delivered to a player that acknowledged none";
      return kRegionMonitorUnbounded;
    }
    acknowledge_monitor = true;

    // 4. A remote observer. The monitor is switched off first, so what arrives
    //    can only have come from the region demodulator and not from the
    //    channel bank's own monitor path.
    QMetaObject::invokeMethod(worker, "setMonitor", Qt::BlockingQueuedConnection,
                              Q_ARG(int, 0), Q_ARG(QVariantList, QVariantList{}),
                              Q_ARG(double, 700.0));
    QMetaObject::invokeMethod(worker, "setRemoteAudioSubscribed",
                              Qt::BlockingQueuedConnection, Q_ARG(bool, true));
    remote_audio.clear();
    feed(16);
    if (remote_audio.empty()) return kRemoteAudioSilent;
    if (remote_rate_hz < 2.0 * kRegionBandwidthHz) return kRemoteAudioWrongRate;
    // The exact combination a station reported wrongly: the local monitor is
    // off and the only consumer is a remote subscriber, so the flags must
    // still say the region is wanted and running.
    static_cast<void>(region_audio_samples());
    if (!region_flags_agree(true)) return kRegionAudioFlagsWrong;

    // 5. A debug capture of an SDR session must produce BOTH files. The IQ is
    //    the forensic payload and the audio is what an operator can actually
    //    listen to; writing one instead of the other is the reported fault.
    QMetaObject::invokeMethod(worker, "setRemoteAudioSubscribed",
                              Qt::BlockingQueuedConnection, Q_ARG(bool, false));
    QMetaObject::invokeMethod(worker, "startDebugCapture",
                              Qt::BlockingQueuedConnection,
                              Q_ARG(QString, capture_root.path()));
    feed(16);
    QMetaObject::invokeMethod(worker, "stopDebugCapture",
                              Qt::BlockingQueuedConnection);
    if (capture_base_path.isEmpty()) return kCaptureIqMissing;
    const QFileInfo iq_file(capture_base_path +
                            QStringLiteral("/iq.sigmf-data"));
    if (!iq_file.exists() || iq_file.size() <= 0) return kCaptureIqMissing;
    const QFileInfo audio_file(capture_base_path +
                               QStringLiteral("/audio.wav"));
    if (!audio_file.exists() || audio_file.size() <= 44) {
      qCritical().noquote()
          << "SDR capture wrote no listenable audio beside its IQ:"
          << capture_base_path << "audio exists=" << audio_file.exists()
          << "size=" << audio_file.size();
      return kCaptureRegionAudioMissing;
    }
    return 0;
  }();
  QMetaObject::invokeMethod(worker, "stop", Qt::BlockingQueuedConnection);
  dsp_thread.quit();
  dsp_thread.wait();
  return result;
}

int runComplexIqDiagnosticsChecks() {
  auto pipe = std::make_shared<cwassistant::desktop::LiveAudioPipe>();
  QThread dsp_thread;
  auto* worker = new cwassistant::desktop::LiveAudioDspWorker(pipe);
  worker->moveToThread(&dsp_thread);
  QObject::connect(&dsp_thread, &QThread::finished, worker,
                   &QObject::deleteLater);

  QJsonObject record;
  // Direct, so the record is captured on the worker's own thread. Every emit
  // below happens inside a BlockingQueuedConnection invocation, so this thread
  // is parked for the duration of the write and resumes with it visible.
  QObject::connect(
      worker,
      &cwassistant::desktop::LiveAudioDspWorker::diagnosticsRecordProduced,
      worker, [&record](const QJsonObject& published) { record = published; },
      Qt::DirectConnection);

  constexpr double iq_sample_rate_hz = 192'000.0;
  constexpr double iq_center_frequency_hz = 14'050'000.0;
  constexpr double decoder_bandwidth_hz = 24'000.0;
  constexpr std::size_t iq_block_samples = 2'048;
  // Enough decimated samples for the decoder branch's own 8192-point analyzer
  // to produce frames; a handful of blocks would assert nothing either way.
  constexpr std::size_t iq_block_count = 128;
  std::vector<cwassistant::core::RealtimeSampleBlock> iq_blocks(iq_block_count);
  double phase = 0.0;
  for (std::size_t index = 0; index < iq_blocks.size(); ++index) {
    auto& block = iq_blocks[index];
    block.stream.kind = cwassistant::core::StreamKind::ComplexIq;
    block.stream.sample_rate_hz = iq_sample_rate_hz;
    block.stream.center_frequency_hz = iq_center_frequency_hz;
    block.stream.channel_count = 1;
    block.sequence = index;
    block.timestamp_ns = static_cast<std::uint64_t>(
        static_cast<long double>(index * iq_block_samples) * 1'000'000'000.0L /
        iq_sample_rate_hz);
    block.sample_count = iq_block_samples;
    for (std::size_t sample = 0; sample < block.sample_count; ++sample) {
      block.samples[sample] = {0.5F * static_cast<float>(std::cos(phase)),
                               0.5F * static_cast<float>(std::sin(phase))};
      // A carrier 1 kHz above the receiver's centre, so it sits inside the
      // requested window rather than on its edge.
      phase += 2.0 * std::numbers::pi * 1'000.0 / iq_sample_rate_hz;
    }
  }

  dsp_thread.start();
  const int result = [&]() -> int {
    QMetaObject::invokeMethod(worker, "start", Qt::BlockingQueuedConnection);
    // Published with nothing in the pipe, exactly as the controller publishes
    // it when the operator starts the receiver.
    QMetaObject::invokeMethod(worker, "setSdrDecoderWindow",
                              Qt::BlockingQueuedConnection,
                              Q_ARG(double, iq_center_frequency_hz),
                              Q_ARG(double, decoder_bandwidth_hz));
    for (std::size_t index = 0; index < iq_blocks.size();) {
      std::size_t pushed = 0;
      while (index < iq_blocks.size() && pushed < 8 &&
             pipe->blocks.try_push(iq_blocks[index])) {
        ++index;
        ++pushed;
      }
      QMetaObject::invokeMethod(worker, "drain", Qt::BlockingQueuedConnection);
    }
    QMetaObject::invokeMethod(worker, "publishLiveDiagnosticsRecord",
                              Qt::BlockingQueuedConnection);
    if (record.isEmpty()) return kNoDiagnosticsRecord;
    if (record.value(QStringLiteral("detector"))
            .toObject()
            .value(QStringLiteral("spectrumFramesToDetector"))
            .toDouble() <= 0.0) {
      qCritical().noquote() << "decoder window never took effect: detector="
                            << record.value(QStringLiteral("detector"));
      return kDecoderWindowNotInForce;
    }

    QMetaObject::invokeMethod(worker, "acceptGuiHeartbeat",
                              Qt::BlockingQueuedConnection,
                              Q_ARG(double, 400.0));
    QMetaObject::invokeMethod(worker, "publishLiveDiagnosticsRecord",
                              Qt::BlockingQueuedConnection);
    const QJsonObject gui = record.value(QStringLiteral("gui")).toObject();
    if (!gui.contains(QStringLiteral("latenessMs")) ||
        !gui.contains(QStringLiteral("peakLatenessMs")) ||
        !gui.contains(QStringLiteral("stallCount")) ||
        !gui.contains(QStringLiteral("sinceLastHeartbeatMs"))) {
      return kGuiBlockMissing;
    }
    // 400 ms is past the visible-stall bar, so it is both the lateness and a
    // counted stall.
    if (gui.value(QStringLiteral("latenessMs")).toDouble() != 400.0 ||
        gui.value(QStringLiteral("peakLatenessMs")).toDouble() != 400.0 ||
        gui.value(QStringLiteral("stallCount")).toInt() != 1) {
      qCritical().noquote() << "GUI lateness not reported:" << gui;
      return kGuiLatenessNotReported;
    }
    // A second record with no heartbeat in between. The peak and the stall
    // count belong to the window that has just closed; the last measured
    // lateness is the latest reading and stays until a new one arrives.
    QMetaObject::invokeMethod(worker, "publishLiveDiagnosticsRecord",
                              Qt::BlockingQueuedConnection);
    const QJsonObject next_gui = record.value(QStringLiteral("gui")).toObject();
    if (next_gui.value(QStringLiteral("peakLatenessMs")).toDouble() != 0.0 ||
        next_gui.value(QStringLiteral("stallCount")).toInt() != 0 ||
        next_gui.value(QStringLiteral("latenessMs")).toDouble() != 400.0) {
      qCritical().noquote() << "GUI peak is not window-scoped:" << next_gui;
      return kGuiPeakNotWindowScoped;
    }
    return 0;
  }();
  QMetaObject::invokeMethod(worker, "stop", Qt::BlockingQueuedConnection);
  dsp_thread.quit();
  dsp_thread.wait();
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  if (const int complex_iq_result = runComplexIqDiagnosticsChecks();
      complex_iq_result != 0) {
    return complex_iq_result;
  }
  if (const int backpressure_result = runSpectrumBackpressureChecks();
      backpressure_result != 0) {
    return backpressure_result;
  }
  if (const int region_audio_result = runRegionAudioChecks();
      region_audio_result != 0) {
    return region_audio_result;
  }
  auto pipe = std::make_shared<cwassistant::desktop::LiveAudioPipe>();
  QThread dsp_thread;
  auto* worker = new cwassistant::desktop::LiveAudioDspWorker(pipe);
  worker->moveToThread(&dsp_thread);
  QObject::connect(&dsp_thread, &QThread::finished, worker,
                   &QObject::deleteLater);

  QObject::connect(
      worker, &cwassistant::desktop::LiveAudioDspWorker::frameProduced,
      &application,
      [&application](const cwassistant::desktop::SpectrumFrame& frame) {
        application.setProperty(
            "validFrame", frame.bins_dbfs.size() == 1'025 &&
                              frame.instantaneous_bins_dbfs.size() == 1'025 &&
                              frame.lower_frequency_hz == 0.0 &&
                              frame.upper_frequency_hz == 24'000.0);
      });
  QString capture_base_path;
  QTemporaryDir capture_root;
  QVariantList last_channels;
  QVariantMap last_diagnostics;
  std::vector<float> selected_monitor_audio;
  QObject::connect(
      worker, &cwassistant::desktop::LiveAudioDspWorker::debugCaptureStateChanged,
      &application,
      [&capture_base_path](const bool, const QString& path, const double,
                           const QString&) { capture_base_path = path; });

  QObject::connect(
      worker, &cwassistant::desktop::LiveAudioDspWorker::decoderProduced,
      &application, [&application, worker, &last_channels](
                        const QVariantList& channels) {
        last_channels = channels;
        const auto channel = channels.isEmpty()
            ? QVariantMap{}
            : channels.front().toMap();
        const auto character_evidence =
            channel.value(QStringLiteral("characterEvidence")).toList();
        const bool valid_decoder = channels.size() == 1 &&
            channel.value(QStringLiteral("verifiedCw")).toBool() &&
            channel.value(QStringLiteral("verificationState")).toString() ==
                QStringLiteral("verified") &&
            channel.value(QStringLiteral("verificationReason")).toString() ==
                QStringLiteral("verified") &&
            channel.value(QStringLiteral("verificationConfidence")).toDouble() >
                0.5 &&
            !character_evidence.isEmpty() &&
            character_evidence.front().toMap()
                .value(QStringLiteral("known")).toBool() &&
            std::abs(channel.value(QStringLiteral("frequencyHz")).toDouble() -
                     1'000.0) < 30.0 &&
            channel.value(QStringLiteral("snrDb")).toDouble() > 6.0;
        if (valid_decoder) {
          application.setProperty("validDecoder", true);
          if (!application.property("monitorConfigured").toBool()) {
            application.setProperty("monitorConfigured", true);
            const QVariantList monitor_ids{
                QVariant::fromValue<qulonglong>(
                    channel.value(QStringLiteral("id")).toULongLong())};
            QMetaObject::invokeMethod(
                worker, "setMonitor", Qt::QueuedConnection, Q_ARG(int, 2),
                Q_ARG(QVariantList, monitor_ids),
                Q_ARG(double, 700.0));
          }
        }
      });
  QObject::connect(
      worker, &cwassistant::desktop::LiveAudioDspWorker::monitorAudioProduced,
      &application,
      [&application, worker, &selected_monitor_audio](
          const QByteArray& bytes, const double sample_rate_hz) {
        if (!application.property("monitorConfigured").toBool() ||
            sample_rate_hz != 48'000.0 ||
            bytes.size() % static_cast<qsizetype>(sizeof(float)) != 0) {
          return;
        }
        const qsizetype sample_count =
            bytes.size() / static_cast<qsizetype>(sizeof(float));
        const std::size_t old_size = selected_monitor_audio.size();
        selected_monitor_audio.resize(
            old_size + static_cast<std::size_t>(sample_count));
        std::memcpy(selected_monitor_audio.data() + old_size, bytes.constData(),
                    static_cast<std::size_t>(bytes.size()));
        if (selected_monitor_audio.size() < 16'384U) return;

        // Skip initial filter settling. The selected carrier is at 1000 Hz,
        // while the requested monitor pitch is 700 Hz; observing energy at the
        // latter proves this is the per-track mix/filter/re-pitch path rather
        // than a copy of the full receiver audio.
        const auto magnitude_at = [&selected_monitor_audio](
                                      const double frequency_hz) {
          double real = 0.0;
          double imaginary = 0.0;
          for (std::size_t index = 4'096U;
               index < selected_monitor_audio.size(); ++index) {
            const double phase = 2.0 * std::numbers::pi * frequency_hz *
                                 static_cast<double>(index) / 48'000.0;
            real += selected_monitor_audio[index] * std::cos(phase);
            imaginary -= selected_monitor_audio[index] * std::sin(phase);
          }
          return std::hypot(real, imaginary);
        };
        const double monitor_magnitude = magnitude_at(700.0);
        if (monitor_magnitude > 20.0 &&
            monitor_magnitude > 5.0 * magnitude_at(1'000.0)) {
          application.setProperty("validSelectedMonitor", true);
          if (application.property("validFrame").toBool() &&
              application.property("validDecoder").toBool()) {
            QMetaObject::invokeMethod(worker, "stopDebugCapture",
                                      Qt::BlockingQueuedConnection);
            application.exit(0);
          }
        }
      });
  QObject::connect(
      worker, &cwassistant::desktop::LiveAudioDspWorker::diagnosticsProduced,
      &application, [&last_diagnostics](const QVariantMap& diagnostics) {
        last_diagnostics = diagnostics;
      });

  constexpr double sample_rate_hz = 48'000.0;
  constexpr std::size_t block_samples = 2'048;
  constexpr std::size_t dit_samples = 2'880;  // 20 WPM, 60 ms.
  std::vector<bool> key_units;
  const auto append_units = [&key_units](const bool keyed,
                                          const std::size_t units) {
    key_units.insert(key_units.end(), units, keyed);
  };
  const auto append_letter = [&append_units](const std::string_view elements) {
    for (std::size_t element = 0; element < elements.size(); ++element) {
      append_units(true, elements[element] == '.' ? 1U : 3U);
      append_units(false, element + 1 == elements.size() ? 3U : 1U);
    }
  };
  // Five repetitions leave a deterministic post-symbol evidence interval for
  // the verification entry hysteresis while exercising continued hypotheses.
  for (int repetition = 0; repetition < 5; ++repetition) {
    append_letter("...");
    append_letter("---");
    append_letter("...");
    append_units(false, 4);  // Extend the last character gap to a word gap.
  }

  const std::size_t total_samples = key_units.size() * dit_samples;
  const std::size_t block_count =
      (total_samples + block_samples - 1) / block_samples;
  std::vector<cwassistant::core::RealtimeSampleBlock> blocks(block_count);
  double phase = 0.0;
  for (std::size_t block_index = 0; block_index < blocks.size();
       ++block_index) {
    auto& block = blocks[block_index];
    block.stream.sample_rate_hz = sample_rate_hz;
    block.sequence = block_index;
    block.timestamp_ns = static_cast<std::uint64_t>(
        static_cast<long double>(block_index * block_samples) *
        1'000'000'000.0L / sample_rate_hz);
    block.sample_count = std::min(
        block_samples, total_samples - block_index * block_samples);
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      const std::size_t absolute_sample = block_index * block_samples + index;
      const bool keyed = key_units[absolute_sample / dit_samples];
      block.samples[index] = {
          keyed ? 0.5F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
      phase += 2.0 * std::numbers::pi * 1'000.0 / sample_rate_hz;
    }
  }
  dsp_thread.start();
  QMetaObject::invokeMethod(
      worker, "configure", Qt::BlockingQueuedConnection, Q_ARG(int, 1),
      Q_ARG(int, 60),
      Q_ARG(bool, true), Q_ARG(bool, false), Q_ARG(double, 0.0),
      Q_ARG(double, -12.0), Q_ARG(bool, false), Q_ARG(double, 0.0),
      Q_ARG(double, 24'000.0));
  QMetaObject::invokeMethod(worker, "start", Qt::BlockingQueuedConnection);
  QMetaObject::invokeMethod(
      worker, "setRadioFrequencyContext", Qt::BlockingQueuedConnection,
      Q_ARG(bool, true), Q_ARG(qulonglong, 7'016'450ULL),
      Q_ARG(qulonglong, 0ULL), Q_ARG(bool, false));
  const QVariantMap presentation_diagnostics{
      {QStringLiteral("offlineCallsignDatabaseState"),
       QStringLiteral("ready")},
      {QStringLiteral("offlineCallsignDatabaseEntries"), 42},
      {QStringLiteral("localCharacterModelState"),
       QStringLiteral("disabled")},
      {QStringLiteral("channels"), QVariantList{}},
  };
  QMetaObject::invokeMethod(
      worker, "setPresentationDiagnostics", Qt::BlockingQueuedConnection,
      Q_ARG(QVariantMap, presentation_diagnostics));
  // Seed one detector pass before starting the recording. This models an
  // operator pressing Debug capture after reception is already underway and
  // makes the first snapshot's lineage deterministic.
  constexpr std::size_t pre_capture_blocks = 8;
  for (std::size_t index = 0; index < pre_capture_blocks; ++index) {
    if (!pipe->blocks.try_push(blocks[index])) return 11;
  }
  QMetaObject::invokeMethod(worker, "drain", Qt::BlockingQueuedConnection);
  QMetaObject::invokeMethod(worker, "startDebugCapture",
                            Qt::BlockingQueuedConnection,
                            Q_ARG(QString, capture_root.path()));
  // A decoder-window publication arriving while an AUDIO card is the source
  // must not touch the decoder: these settings are republished whenever
  // anything on the SDR page changes, and acting on one here once destroyed a
  // working audio decode for a receiver that was not even running. The audio
  // assertions below have to keep passing with this in flight.
  QMetaObject::invokeMethod(worker, "setSdrDecoderWindow",
                            Qt::BlockingQueuedConnection,
                            Q_ARG(double, 7'016'450.0), Q_ARG(double, 12'000.0));

  std::size_t next_block = pre_capture_blocks;
  QTimer feeder;
  feeder.setInterval(1);
  QObject::connect(&feeder, &QTimer::timeout, &application,
                   [&blocks, &next_block, &pipe, &feeder] {
    if (next_block < blocks.size() &&
        pipe->blocks.try_push(blocks[next_block])) {
      ++next_block;
    }
    if (next_block == blocks.size()) feeder.stop();
  });
  feeder.start();

  QTimer::singleShot(10'000, &application,
                     [&application, &last_channels, &last_diagnostics,
                      &selected_monitor_audio] {
    qCritical().noquote()
        << "live pipeline timeout: validFrame="
        << application.property("validFrame").toBool()
        << "validDecoder="
        << application.property("validDecoder").toBool()
        << "validSelectedMonitor="
        << application.property("validSelectedMonitor").toBool()
        << "monitorSamples=" << selected_monitor_audio.size()
        << "channels=" << last_channels
        << "diagnostics=" << last_diagnostics;
    application.exit(3);
  });
  const int result = application.exec();
  QMetaObject::invokeMethod(worker, "stop", Qt::BlockingQueuedConnection);
  dsp_thread.quit();
  dsp_thread.wait();
  if (result != 0) {
    return result;
  }

  if (capture_base_path.isEmpty()) {
    return 4;
  }
  QFile wav_file(capture_base_path + QStringLiteral("/audio.wav"));
  QFile log_file(capture_base_path + QStringLiteral("/diagnostics.jsonl"));
  if (!wav_file.exists() || wav_file.size() <= 44) {
    return 5;  // Missing or header-only capture audio.
  }
  if (!log_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return 6;
  }
  const QByteArray first_line = log_file.readLine();
  if (!first_line.contains("\"tracks\"") ||
      !first_line.contains("\"candidateTracks\"") ||
      !first_line.contains("\"acousticWpm\"") ||
      !first_line.contains("\"acousticCadenceConfidence\"") ||
      !first_line.contains("\"keyingLevelSeparationDb\"") ||
      !first_line.contains("\"keyingLevelExplainedVariation\"") ||
      !first_line.contains("\"robustKeyingLevelAnchorActive\"") ||
      !first_line.contains("\"presentedCallsign\"") ||
      !first_line.contains("\"presentedCallsignSource\"") ||
      !first_line.contains("\"identityOriginFrequencyHz\"") ||
      !first_line.contains("\"presentationFrequencyHz\"") ||
      !first_line.contains("\"matchAgeSeconds\"") ||
      !first_line.contains("\"colorIndex\"") ||
      !first_line.contains("\"matched\"") ||
      !first_line.contains("\"active\"") ||
      !first_line.contains("\"keyDown\"")) {
    return 7;  // Diagnostics log line missing expected structure.
  }
  if (!first_line.contains("\"radio\"") ||
      !first_line.contains("7016450") ||
      !first_line.contains("\"available\":true")) {
    return 8;  // Radio frequency context missing from the diagnostics log.
  }
  if (!first_line.contains("\"captureContext\"") ||
      !first_line.contains("\"startedWithExistingDecoderState\":true") ||
      !first_line.contains("\"existingTracksAtStart\":1") ||
      !first_line.contains("\"existingPublishedChannelsAtStart\":0")) {
    return 9;  // Capture-start decoder lineage is not explicit.
  }
  if (!first_line.contains("\"gui\"") ||
      !first_line.contains("\"peakLatenessMs\"") ||
      !first_line.contains("\"stallCount\"")) {
    // The capture file carries the GUI-thread lateness too, and reads its own
    // window: a saturated GUI thread is as much a part of a recorded
    // reproduction as it is of a watched stream.
    return 12;
  }
  if (!first_line.contains("\"presentation\"") ||
      !first_line.contains("\"offlineCallsignDatabaseState\":\"ready\"") ||
      !first_line.contains("\"offlineCallsignDatabaseEntries\":42") ||
      !first_line.contains("\"localCharacterModelState\":\"disabled\"")) {
    return 10;  // Callsign/model presentation context is not captured.
  }
  return 0;
}
