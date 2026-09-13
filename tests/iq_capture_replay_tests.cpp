#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cwassistant/core/cw_channel_bank.hpp"
#include "cwassistant/core/cw_transmit_encoder.hpp"
#include "cwassistant/core/iq_replay_source.hpp"
#include "cwassistant/core/iq_writer.hpp"
#include "cwassistant/core/sample_block.hpp"
#include "support/file_sha256.hpp"
#include "support/iq_replay_chain.hpp"

// Covers the path that lets a SigMF capture be scored the way a WAV clip is:
// the two-stage receive chain the replay harness drives, the window it decodes
// in, and the guards that bind an annotation sidecar to a two-file recording.
//
// The fixture is synthetic and deliberately small, but it is produced by
// IqWriter and read back by IqReplaySource, so what is under test is the real
// chain rather than a model of it.

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

constexpr double kCaptureRateHz = 62'500.0;
// Low enough that a signal's absolute RF also fits the annotation sidecar's
// audio-shaped frequency range, which is what makes the annotated path
// exercisable at all. A real HF capture does not have that luxury.
constexpr double kCaptureCenterHz = 12'000.0;
constexpr double kSignalFrequencyHz = 14'000.0;
constexpr double kWindowBandwidthHz = 6'000.0;
constexpr std::uint16_t kWordsPerMinute = 30;

std::filesystem::path scratchPath(const std::string_view name) {
  return std::filesystem::temp_directory_path() / std::string(name);
}

void removeRecording(const std::filesystem::path& data_path) {
  std::error_code error;
  std::filesystem::remove(data_path, error);
  std::filesystem::remove(
      cwassistant::core::IqWriter::metadataPathFor(data_path.string()), error);
}

// One keyed CW signal in complex baseband, written through IqWriter in
// live-sized blocks.
//
// The envelope is ramped rather than switched. A hard edge spreads a click
// across the whole passband, and the decimator's low-pass would then hand
// detection a transient at every element boundary -- a fixture that failed for
// a reason that has nothing to do with what is being tested.
bool writeSyntheticCapture(const std::filesystem::path& data_path,
                           const std::string& text) {
  using namespace cwassistant::core;
  const auto plan = CwTransmitEncoder::encode(text, kWordsPerMinute);
  if (!plan) return false;
  const double dot_seconds =
      static_cast<double>(plan->dot_duration_ns) / 1'000'000'000.0;

  std::vector<float> envelope;
  // Half a second of lead-in: the detector needs a noise floor to measure
  // before it has anything to measure a signal against.
  envelope.assign(static_cast<std::size_t>(kCaptureRateHz * 0.5), 0.0F);
  const auto ramp_samples =
      static_cast<std::size_t>(kCaptureRateHz * 0.004);
  for (const auto& span : plan->spans) {
    const auto samples = static_cast<std::size_t>(
        dot_seconds * static_cast<double>(span.duration_units) *
        kCaptureRateHz);
    for (std::size_t index = 0; index < samples; ++index) {
      float level = span.key_down ? 1.0F : 0.0F;
      if (span.key_down && ramp_samples > 0) {
        const std::size_t from_edge =
            std::min(index, samples > index ? samples - 1U - index : 0U);
        if (from_edge < ramp_samples) {
          level = static_cast<float>(
              0.5 - 0.5 * std::cos(std::numbers::pi *
                                   static_cast<double>(from_edge) /
                                   static_cast<double>(ramp_samples)));
        }
      }
      envelope.push_back(level);
    }
  }
  envelope.insert(envelope.end(),
                  static_cast<std::size_t>(kCaptureRateHz * 0.25), 0.0F);

  IqCaptureMetadata metadata;
  metadata.format = IqSampleFormat::Ci16Le;
  metadata.sample_rate_hz = kCaptureRateHz;
  metadata.center_frequency_hz = kCaptureCenterHz;
  metadata.hardware = "Synthetic test receiver";
  metadata.description = "Two-stage replay chain fixture";
  metadata.datetime_utc = "2026-01-01T00:00:00.000Z";
  IqWriter writer;
  if (!writer.open(data_path.string(), metadata)) return false;

  const double offset_hz = kSignalFrequencyHz - kCaptureCenterHz;
  const double step = 2.0 * std::numbers::pi * offset_hz / kCaptureRateHz;
  // A deterministic floor, so the fixture reproduces byte for byte on every
  // platform while still giving the detector something to rise above.
  std::uint32_t noise_state = 0x13579BDFU;
  const auto noise = [&noise_state] {
    noise_state = noise_state * 1'664'525U + 1'013'904'223U;
    return (static_cast<float>(noise_state >> 8U) / 8'388'608.0F - 1.0F) *
           0.002F;
  };
  RealtimeSampleBlock block;
  block.stream = {.kind = StreamKind::ComplexIq,
                  .sample_rate_hz = kCaptureRateHz,
                  .center_frequency_hz = kCaptureCenterHz,
                  .channel_count = 1};
  // std::size_t, not std::uint64_t: the two are the same width here and
  // different types on Linux, where a std::min of the two spellings deduces
  // nothing at all.
  std::size_t written = 0;
  while (written < envelope.size()) {
    block.sample_count =
        std::min(block.samples.size(), envelope.size() - written);
    block.sequence = written / block.samples.size();
    block.timestamp_ns = static_cast<std::uint64_t>(
        static_cast<long double>(written) * 1'000'000'000.0L / kCaptureRateHz);
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      const double phase = step * static_cast<double>(written + index);
      const float level = envelope[written + index] * 0.35F;
      block.samples[index] = {
          level * static_cast<float>(std::cos(phase)) + noise(),
          level * static_cast<float>(std::sin(phase)) + noise()};
    }
    if (!writer.writeBlock(block)) return false;
    written += block.sample_count;
  }
  writer.close();
  return true;
}

struct ReplayOutcome {
  bool track_found{false};
  double track_frequency_hz{0.0};
  std::string text;
  double detector_lower_hz{0.0};
  double detector_upper_hz{0.0};
  double detector_bin_width_hz{0.0};
  std::uint64_t wide_blocks{0};
  std::uint64_t rejected_blocks{0};
  std::uint64_t decoder_blocks{0};
  double decoder_sample_rate_hz{0.0};
  cwassistant::core::IqBlockStatus last_rejection{
      cwassistant::core::IqBlockStatus::Accepted};
};

// Exactly what the replay harness does with a capture, minus the reporting:
// overview transform, decimator, decoder transform, windowed detection bins,
// then the channel bank.
ReplayOutcome replayThroughChain(const std::filesystem::path& data_path,
                                 const cwassistant::test::IqDecoderWindow&
                                     window,
                                 const double match_frequency_hz) {
  using namespace std::chrono_literals;
  using namespace cwassistant::core;
  ReplayOutcome outcome;
  IqReplaySource source;
  if (!source.open(data_path.string(), {.kind = StreamKind::ComplexIq}) ||
      !source.start()) {
    return outcome;
  }
  cwassistant::test::IqReplayChain chain;
  if (!chain.configure(window)) return outcome;
  CwChannelBank channels;
  RealtimeSampleBlock block;
  while (source.read(block, 0ms)) {
    const auto frame = chain.process(block);
    if (frame.block != nullptr) {
      for (const auto& spectrum : frame.spectra) {
        const auto view = chain.detectorView(spectrum);
        outcome.detector_lower_hz = view.lower_frequency_hz;
        outcome.detector_upper_hz = view.upper_frequency_hz;
        outcome.detector_bin_width_hz = spectrum.bin_width_hz;
        static_cast<void>(channels.updateSpectrum(
            spectrum.timestamp_ns, view.lower_frequency_hz,
            view.upper_frequency_hz, view.bins_dbfs, false));
      }
      static_cast<void>(channels.processSamples(*frame.block));
      for (const auto& diagnostic : channels.allTrackDiagnostics()) {
        if (std::abs(diagnostic.presentation_frequency_hz -
                     match_frequency_hz) > 60.0) {
          continue;
        }
        outcome.track_found = true;
        outcome.track_frequency_hz = diagnostic.presentation_frequency_hz;
        if (diagnostic.text.size() > outcome.text.size())
          outcome.text = diagnostic.text;
      }
    }
    chain.advance();
  }
  outcome.wide_blocks = chain.wideBlocks();
  outcome.rejected_blocks = chain.rejectedBlocks();
  outcome.decoder_blocks = chain.decoderBlocks();
  outcome.decoder_sample_rate_hz = chain.decoderSampleRateHz();
  outcome.last_rejection = chain.lastRejection();
  return outcome;
}

void testCaptureReachesTheDecoder() {
  const auto data_path = scratchPath("cwa_iq_capture_replay.sigmf-data");
  removeRecording(data_path);
  expect(writeSyntheticCapture(data_path, "CQ DE W1AW"),
         "the synthetic capture is written");

  cwassistant::core::IqReplaySource source;
  expect(source.open(data_path.string(),
                     {.kind = cwassistant::core::StreamKind::ComplexIq}),
         "the capture opens for replay");
  const auto window = cwassistant::test::defaultIqDecoderWindow(source);
  expect(window.center_frequency_hz == kCaptureCenterHz,
         "the default decoder window sits at the capture's own centre");
  expect(window.bandwidth_hz ==
             cwassistant::test::kDefaultIqDecoderBandwidthHz,
         "the default decoder width is the application's own");

  const cwassistant::test::IqDecoderWindow requested{
      .center_frequency_hz = kSignalFrequencyHz,
      .bandwidth_hz = kWindowBandwidthHz};
  const auto outcome =
      replayThroughChain(data_path, requested, kSignalFrequencyHz);
  expect(outcome.rejected_blocks == 0U && outcome.decoder_blocks > 0U,
         "every capture block reaches the decoder branch");
  // The decimator chooses from a fixed set of rates, so this is the branch
  // running well below the capture rate rather than at it.
  expect(outcome.decoder_sample_rate_hz == 48'000.0,
         "the decoder branch runs at the decimated rate, not the capture "
         "rate");
  // Detection reads the requested window and not the whole decimated stream.
  // Without the slice this span would be the decoder branch's full 48 kHz,
  // and every signal outside the operator's window would be discovered.
  expect(std::abs(outcome.detector_lower_hz -
                  (kSignalFrequencyHz - kWindowBandwidthHz * 0.5)) <=
                 outcome.detector_bin_width_hz &&
             std::abs(outcome.detector_upper_hz -
                      (kSignalFrequencyHz + kWindowBandwidthHz * 0.5)) <=
                 outcome.detector_bin_width_hz,
         "detection sees the requested window and nothing wider");
  expect(outcome.track_found,
         "a track is tracked at the signal's absolute RF frequency");
  // The callsign rather than the CQ: a first character keyed out of silence
  // is the one element whose leading edge the detector has no floor estimate
  // for yet, and demanding it would make the fixture fail for a reason that is
  // not the chain.
  expect(outcome.text.find("W1AW") != std::string::npos,
         "the capture decodes to text: \"" + outcome.text + "\"");
  removeRecording(data_path);
}

void testWindowOutsideThePassbandIsRefusedNotSilent() {
  const auto data_path =
      scratchPath("cwa_iq_capture_replay_outside.sigmf-data");
  removeRecording(data_path);
  // One character: this case asserts that nothing reaches the decoder, so a
  // fixture long enough to decode would only cost CI time to prove it.
  expect(writeSyntheticCapture(data_path, "E"),
         "the synthetic capture is written for the passband case");

  // Far enough out that the window's lower edge leaves the acquired span. The
  // decimator answers RejectedDescriptor and nothing reaches detection, while
  // an overview spectrum would still paint normally -- which is why a replay
  // has to be able to say so rather than report an empty decode.
  const cwassistant::test::IqDecoderWindow outside{
      .center_frequency_hz = kCaptureCenterHz + 33'000.0,
      .bandwidth_hz = kWindowBandwidthHz};
  const cwassistant::core::StreamDescriptor stream{
      .kind = cwassistant::core::StreamKind::ComplexIq,
      .sample_rate_hz = kCaptureRateHz,
      .center_frequency_hz = kCaptureCenterHz,
      .channel_count = 1};
  expect(!cwassistant::test::iqDecoderWindowFitsPassband(outside, stream),
         "a window past the passband edge is reported as not fitting");
  const cwassistant::test::IqDecoderWindow inside{
      .center_frequency_hz = kCaptureCenterHz + 20'000.0,
      .bandwidth_hz = kWindowBandwidthHz};
  expect(cwassistant::test::iqDecoderWindowFitsPassband(inside, stream),
         "a window inside the passband edge is reported as fitting");
  // The decoder slice has to fit, not merely its centre. This window is tuned
  // 2 kHz inside the Nyquist edge and is still refused, because its upper half
  // is not: a rule written on the centre alone accepts it and then decodes a
  // window half of which does not exist.
  const cwassistant::test::IqDecoderWindow straddling{
      .center_frequency_hz = kCaptureCenterHz + 29'000.0,
      .bandwidth_hz = kWindowBandwidthHz};
  expect(!cwassistant::test::iqDecoderWindowFitsPassband(straddling, stream),
         "a window whose centre is inside but whose edge is not is refused");

  const auto outcome =
      replayThroughChain(data_path, outside, kSignalFrequencyHz);
  expect(outcome.wide_blocks > 0U &&
             outcome.rejected_blocks == outcome.wide_blocks,
         "every block is refused when the window is outside the passband");
  expect(outcome.decoder_blocks == 0U && !outcome.track_found,
         "nothing reaches detection from a window outside the passband");
  expect(outcome.last_rejection ==
             cwassistant::core::IqBlockStatus::RejectedDescriptor,
         "the refusal names the descriptor, not the samples");
  removeRecording(data_path);
}

void testAnnotationDigestCoversBothHalvesOfThePair() {
  const auto data_path = scratchPath("cwa_iq_capture_replay_hash.sigmf-data");
  removeRecording(data_path);
  expect(writeSyntheticCapture(data_path, "CQ"),
         "the synthetic capture is written for the digest case");
  const auto metadata_path =
      cwassistant::core::IqWriter::metadataPathFor(data_path.string());

  const auto payload_only = cwassistant::test::fileSha256(data_path.string());
  const auto pair =
      cwassistant::test::filesSha256({data_path.string(), metadata_path});
  expect(!pair.empty() && pair.size() == 64U,
         "the pair digest is a SHA-256");
  expect(pair != payload_only,
         "the pair digest is not merely the payload's digest");

  // The sidecar carries the sample rate and every capture segment's centre
  // frequency. Those decide what a decoder window means and therefore what a
  // track's absolute RF is, so an annotation set bound to the payload alone
  // would still verify after the score had been changed underneath it.
  std::string sidecar;
  {
    std::ifstream input(metadata_path, std::ios::binary);
    sidecar.assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
  }
  const auto centre = sidecar.find("\"core:frequency\": 12000");
  expect(centre != std::string::npos,
         "the sidecar declares the capture's centre frequency");
  if (centre != std::string::npos) {
    std::string retuned = sidecar;
    retuned.replace(centre, std::string("\"core:frequency\": 12000").size(),
                    "\"core:frequency\": 14000");
    std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
    output.write(retuned.data(),
                 static_cast<std::streamsize>(retuned.size()));
  }
  expect(cwassistant::test::fileSha256(data_path.string()) == payload_only,
         "rewriting the sidecar leaves the payload's digest unchanged");
  expect(cwassistant::test::filesSha256({data_path.string(), metadata_path}) !=
             pair,
         "rewriting the sidecar changes the pair digest");
  removeRecording(data_path);
}

std::string readSource(const char* path) {
  std::ifstream source(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{source},
          std::istreambuf_iterator<char>{}};
}

// A short capture is normal when the writer says the operator stopped it, and
// suspicious when it says nothing. The replay report is the only place a
// reader of a scored capture can learn which, so the fields are held here.
void testReplayReportsWhyTheCaptureEnded() {
  const std::string replay = readSource(CWA_CAPTURE_REPLAY_PATH);
  expect(!replay.empty(), "the capture replay source is readable");
  for (const std::string_view field :
       {std::string_view{"stop_reason="}, std::string_view{"ends_mid_sample="},
        std::string_view{"capture_segment "}, std::string_view{"decoder_window "},
        std::string_view{".sigmf-data"}, std::string_view{".sigmf-meta"}}) {
    expect(replay.find(field) != std::string::npos,
           "the capture report carries " + std::string(field));
  }
}

}  // namespace

int main() {
  testCaptureReachesTheDecoder();
  testWindowOutsideThePassbandIsRefusedNotSilent();
  testAnnotationDigestCoversBothHalvesOfThePair();
  testReplayReportsWhyTheCaptureEnded();
  if (failures == 0) std::cout << "IQ capture replay tests passed\n";
  return failures == 0 ? 0 : 1;
}
