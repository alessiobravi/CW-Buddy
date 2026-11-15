// Numerical-invariant tests for the dependency-free core.
//
// Two things are checked here, neither of which any behavioural test was in a
// position to notice:
//
//   * A tracking oscillator is a unit-magnitude complex number stepped once
//     per sample. The step is not exactly unit magnitude once its cosine and
//     sine are rounded to float, so the error compounds and the oscillator
//     has to be renormalized periodically. Whether that happens must not
//     depend on how many samples the caller happens to put in a block. This
//     is a bug fix, and these cases fail against the code before it.
//   * WavWriter and WavReplaySource deliberately do not use the same PCM16
//     scale. Each end's convention is pinned separately, and the gap between
//     them is bounded, so that the decision survives somebody noticing the
//     two constants differ. These cases passed before and must keep passing.
//
// Both are verified end to end, through the public interfaces, because the
// oscillators and the scale factors are private implementation details and
// the point is the behaviour they produce.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numbers>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cwassistant/core/cw_channel_bank.hpp"
#include "cwassistant/core/cw_character_lane_frontend.hpp"
#include "cwassistant/core/sample_block.hpp"
#include "cwassistant/core/wav_replay_source.hpp"
#include "cwassistant/core/wav_writer.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string decibels(const float value) {
  char text[32]{};
  std::snprintf(text, sizeof(text), "%.4f dB", static_cast<double>(value));
  return text;
}

// -------------------------------------------------------------------------
// Channel-bank tracking oscillators
// -------------------------------------------------------------------------

struct BankLevels {
  bool measured{false};
  float settled_db{0.0F};
  float final_db{0.0F};
};

// Feeds the bank one unbroken tone, in blocks of exactly `block_samples`, and
// reports the track's measured level once it has settled and again at the end
// of the run. Nothing about the signal changes in between, so the two numbers
// are measurements of the same thing and must agree.
BankLevels runSteadyTone(const std::size_t block_samples, const double tone_hz,
                         const std::uint64_t total_samples) {
  using namespace cwassistant::core;
  constexpr double kSampleRate = 8'000.0;
  constexpr std::uint64_t kSettleSamples = 400'000;

  CwChannelBank bank({.minimum_verification_symbols = 0});
  std::vector<float> bins(101, -100.0F);
  bins[static_cast<std::size_t>(tone_hz / 10.0)] = -40.0F;

  BankLevels levels;
  RealtimeSampleBlock block;
  block.stream.sample_rate_hz = kSampleRate;
  block.sample_count = block_samples;
  double phase = 0.0;
  std::uint64_t produced = 0;
  while (produced < total_samples) {
    const auto timestamp_ns = static_cast<std::uint64_t>(
        static_cast<long double>(produced) * 1'000'000'000.0L / kSampleRate);
    static_cast<void>(
        bank.updateSpectrum(timestamp_ns, 0.0, 1'000.0, bins, false));
    block.timestamp_ns = timestamp_ns;
    for (std::size_t index = 0; index < block_samples; ++index) {
      block.samples[index] = {0.4F * static_cast<float>(std::sin(phase)),
                              0.0F};
      phase += 2.0 * std::numbers::pi * tone_hz / kSampleRate;
      if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
    }
    static_cast<void>(bank.processSamples(block));

    const std::uint64_t before = produced;
    produced += block_samples;
    const bool settling_point =
        before < kSettleSamples && produced >= kSettleSamples;
    if (settling_point || produced >= total_samples) {
      const auto diagnostics = bank.allTrackDiagnostics();
      if (!diagnostics.empty()) {
        if (settling_point) {
          levels.settled_db = diagnostics.front().snr_db;
          levels.measured = true;
        }
        levels.final_db = diagnostics.front().snr_db;
      }
    }
  }
  return levels;
}

void test_channel_bank_oscillators_survive_short_blocks() {
  // The audio path publishes 2048-sample blocks; the direct-SDR decode path
  // batches decimated samples at a threshold of 512, so its blocks are
  // shorter than the oscillators' 1024-sample renormalization period. Both
  // are driven here with the same unbroken tone, and both must report the
  // same unchanging level: a tone that never varies cannot legitimately
  // produce a level that creeps, and the SDR path cannot legitimately
  // measure a different level from the audio path for identical samples.
  //
  // Two tones, because the per-step magnitude error depends on the frequency:
  // at 8 kHz the 350 Hz step is short of unit magnitude and the 450 Hz step
  // is over it, so the two drift in opposite directions and neither can be
  // mistaken for a settling transient.
  constexpr std::uint64_t kRunSamples = 3'000'000;  // 6.25 minutes at 8 kHz
  constexpr float kToleranceDb = 0.05F;
  for (const double tone_hz : {350.0, 450.0}) {
    const std::string tone = std::to_string(static_cast<int>(tone_hz)) + " Hz";
    const BankLevels sdr_blocks = runSteadyTone(512, tone_hz, kRunSamples);
    const BankLevels audio_blocks = runSteadyTone(2'048, tone_hz, kRunSamples);
    const bool measured = sdr_blocks.measured && audio_blocks.measured;
    expect(measured, "a steady tone at " + tone + " produces a tracked "
                                                  "channel to measure");
    if (!measured) continue;

    const float creep = std::abs(sdr_blocks.final_db - sdr_blocks.settled_db);
    expect(creep < kToleranceDb,
           "a tone at " + tone +
               " measures the same level after six minutes of 512-sample "
               "blocks as it did when it settled (drifted " +
               decibels(creep) + ")");
    const float between_paths =
        std::abs(sdr_blocks.final_db - audio_blocks.final_db);
    expect(between_paths < kToleranceDb,
           "the same tone at " + tone +
               " measures the same level whether it arrives in 512-sample "
               "or 2048-sample blocks (differed by " +
               decibels(between_paths) + ")");
  }
}

// -------------------------------------------------------------------------
// Character-lane frontend oscillators
// -------------------------------------------------------------------------

struct LaneWindows {
  std::uint64_t count{0};
  std::vector<float> first;
  std::vector<float> last;
};

// Feeds the lane frontend one unbroken carrier in blocks of `block_samples`
// and keeps the second and last feature windows it emits. The first window is
// skipped because it contains the lane filter's startup transient; every
// window after it sees the same steady state.
//
// The carrier sits exactly on the lane's center frequency, so the feature
// mixer places it exactly on an FFT bin (425 Hz with 25 Hz bins) and every
// bin's magnitude is independent of where in the tone's cycle a frame began.
// Anything that then moves a feature between two windows is a change in gain,
// which is what an oscillator losing its unit magnitude produces.
LaneWindows runLaneTone(const std::size_t block_samples,
                        const std::uint64_t total_samples) {
  using namespace cwassistant::core;
  constexpr double kInputRate = 48'000.0;
  constexpr double kCarrierHz = 700.0;
  const CwCharacterFeatureContract contract{
      .working_sample_rate_hz = 3'200.0,
      .fft_length = 128,
      .hop_length = 128,
      .minimum_frequency_hz = 400.0,
      .maximum_frequency_hz = 500.0,
      .isolated_tone_hz = 425.0,
      .lane_bandwidth_hz = 40.0,
      .lane_filter_taps = 129,
      .window_seconds = 5.0,
      .stride_seconds = 1.0};

  CwCharacterLaneFrontend lane(contract);
  lane.reset({.track_id = 1}, kCarrierHz);

  LaneWindows windows;
  RealtimeSampleBlock block;
  block.stream.sample_rate_hz = kInputRate;
  block.sample_count = block_samples;
  double phase = 0.0;
  std::uint64_t produced = 0;
  CwCharacterFeatureWindow window;
  while (produced < total_samples) {
    block.timestamp_ns = static_cast<std::uint64_t>(
        static_cast<long double>(produced) * 1'000'000'000.0L / kInputRate);
    for (std::size_t index = 0; index < block_samples; ++index) {
      block.samples[index] = {0.5F * static_cast<float>(std::sin(phase)),
                              0.0F};
      phase += 2.0 * std::numbers::pi * kCarrierHz / kInputRate;
      if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
    }
    lane.process(block);
    produced += block_samples;
    while (lane.takeWindow(window)) {
      ++windows.count;
      if (windows.count == 2) windows.first = window.features;
      if (windows.count >= 2) windows.last = window.features;
    }
  }
  return windows;
}

void test_character_lane_oscillators_survive_long_runs() {
  // The lane has two oscillators. The decimating mixer's was renormalized on
  // a block-local counter, so it was never renormalized at all on a path
  // whose blocks are shorter than the period; the feature mixer's was never
  // renormalized on any path. Both multiply every feature the lane emits, so
  // their drift is a slow gain error in the model's input that no decode
  // failure would ever point at.
  //
  // An unbroken carrier produces an unchanging spectrum, so the last window
  // of a long run must be the same as the first steady one.
  constexpr std::uint64_t kRunSamples = 7'500'000;  // 2.6 minutes at 48 kHz
  const LaneWindows windows = runLaneTone(512, kRunSamples);
  expect(windows.count >= 3 && !windows.first.empty() &&
             windows.first.size() == windows.last.size(),
         "a long steady carrier produces repeated lane feature windows");
  if (windows.count < 3 || windows.first.empty() ||
      windows.first.size() != windows.last.size()) {
    return;
  }

  float peak = 0.0F;
  float largest_difference = 0.0F;
  for (std::size_t index = 0; index < windows.first.size(); ++index) {
    peak = std::max(peak, std::abs(windows.first[index]));
    largest_difference = std::max(
        largest_difference,
        std::abs(windows.first[index] - windows.last[index]));
  }
  expect(peak > 0.05F, "the lane's features carry the tone at a usable level");
  // A tenth of a percent of the strongest feature. Float rounding over half a
  // million samples of identical filtering accounts for far less than this;
  // an un-renormalized oscillator accounts for much more.
  const float tolerance = 0.001F * peak;
  char report[96]{};
  std::snprintf(report, sizeof(report), "%.6f against a tolerance of %.6f",
                static_cast<double>(largest_difference),
                static_cast<double>(tolerance));
  expect(largest_difference <= tolerance,
         std::string("lane features for an unchanging tone are unchanged "
                     "after half a million samples: worst bin moved by ") +
             report);
}

// -------------------------------------------------------------------------
// WAV capture/replay scaling conventions
// -------------------------------------------------------------------------

void write_u16_le(std::ostream& stream, const std::uint16_t value) {
  const char bytes[2]{static_cast<char>(value & 0xFFU),
                      static_cast<char>((value >> 8U) & 0xFFU)};
  stream.write(bytes, sizeof(bytes));
}

void write_u32_le(std::ostream& stream, const std::uint32_t value) {
  const char bytes[4]{static_cast<char>(value & 0xFFU),
                      static_cast<char>((value >> 8U) & 0xFFU),
                      static_cast<char>((value >> 16U) & 0xFFU),
                      static_cast<char>((value >> 24U) & 0xFFU)};
  stream.write(bytes, sizeof(bytes));
}

// A canonical 44-byte-header mono PCM16 file holding exactly these codes.
void writePcm16Wav(const std::filesystem::path& path,
                   const std::vector<std::int16_t>& codes) {
  const auto data_size = static_cast<std::uint32_t>(codes.size() * 2U);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write("RIFF", 4);
  write_u32_le(output, 36U + data_size);
  output.write("WAVEfmt ", 8);
  write_u32_le(output, 16);
  write_u16_le(output, 1);
  write_u16_le(output, 1);
  write_u32_le(output, 8'000);
  write_u32_le(output, 8'000U * 2U);
  write_u16_le(output, 2);
  write_u16_le(output, 16);
  output.write("data", 4);
  write_u32_le(output, data_size);
  for (const std::int16_t code : codes) {
    write_u16_le(output, static_cast<std::uint16_t>(code));
  }
}

// The PCM codes in a canonical mono PCM16 file, whose data chunk starts at
// byte 44 -- the layout both writePcm16Wav above and WavWriter emit.
std::vector<std::int16_t> readPcm16Codes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<unsigned char> bytes(
      (std::istreambuf_iterator<char>(input)),
      std::istreambuf_iterator<char>());
  std::vector<std::int16_t> codes;
  for (std::size_t offset = 44; offset + 1 < bytes.size(); offset += 2) {
    codes.push_back(static_cast<std::int16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(
                                       bytes[offset + 1])
                                   << 8U)));
  }
  return codes;
}

void test_wav_scaling_conventions_are_deliberate() {
  using namespace std::chrono_literals;
  using namespace cwassistant::core;

  // WavWriter and WavReplaySource do not use the same scale, and that is a
  // decision: the writer cannot use 32768 without overflowing int16 on a
  // full-scale sample, and the reader -- whose main job is other people's
  // recordings -- follows the 2^(bits-1) convention those files are written
  // against. Each end is pinned here separately, because the thing that
  // would quietly undo the decision is somebody "fixing" one constant to
  // match the other.

  // The reader's end: every code decodes as code/32768 exactly, which is
  // what makes the most negative code exactly -1.0 and what makes the
  // conversion free of rounding at every code.
  const std::vector<std::int16_t> codes{0,      1,       -1,     12'288,
                                        16'384, -16'384, 32'767, -32'768};
  const auto foreign_path = std::filesystem::temp_directory_path() /
                            "cwa_wav_reader_convention.wav";
  writePcm16Wav(foreign_path, codes);
  {
    WavReplaySource reader;
    RealtimeSampleBlock replayed;
    const bool read_back = reader.open(foreign_path.string(), {}) &&
                           reader.start() && reader.read(replayed, 0ms) &&
                           replayed.sample_count == codes.size();
    expect(read_back, "a foreign PCM16 recording replays every frame");
    if (read_back) {
      std::size_t mismatches = 0;
      for (std::size_t index = 0; index < codes.size(); ++index) {
        if (replayed.samples[index].real() !=
            static_cast<float>(codes[index]) / 32'768.0F) {
          ++mismatches;
        }
      }
      expect(mismatches == 0,
             "PCM16 decodes by the 2^(bits-1) convention, exactly, for "
             "every code");
      expect(replayed.samples[7].real() == -1.0F,
             "the most negative PCM16 code decodes to exactly -1.0, which "
             "is the reason for that convention");
    }
    reader.stop();
  }
  std::error_code removal_error;
  std::filesystem::remove(foreign_path, removal_error);

  // The writer's end: full scale is +/-32767, equidistant from zero and
  // inside int16 in both directions.
  const auto capture_path = std::filesystem::temp_directory_path() /
                            "cwa_wav_writer_convention.wav";
  std::vector<float> written{0.0F, 1.0F, -1.0F, 0.25F, -0.25F};
  for (std::size_t index = 0; index < 500; ++index) {
    written.push_back(0.97F * static_cast<float>(std::sin(
                                  0.013 * static_cast<double>(index))));
  }
  {
    WavWriter writer;
    expect(writer.open(capture_path.string(), 8'000.0),
           "the capture writer opens a file for the convention check");
    RealtimeSampleBlock block;
    block.stream.sample_rate_hz = 8'000.0;
    block.sample_count = written.size();
    for (std::size_t index = 0; index < written.size(); ++index) {
      block.samples[index] = {written[index], 0.0F};
    }
    expect(writer.writeBlock(block) &&
               writer.framesWritten() == written.size(),
           "the capture writer accepts every sample of the convention check");
    writer.close();
  }
  const auto captured_codes = readPcm16Codes(capture_path);
  const bool captured =
      captured_codes.size() == written.size() && captured_codes.size() > 3;
  expect(captured, "the capture file holds one PCM16 code per sample");
  if (captured) {
    expect(captured_codes[0] == 0 && captured_codes[1] == 32'767 &&
               captured_codes[2] == -32'767,
           "a captured full-scale sample is written as +/-32767: the rails "
           "stay equidistant from zero and neither overflows int16");
  }

  // What the two conventions together cost: quantization, plus one part in
  // 32768, and nothing else. Bounding it here is what keeps the asymmetry a
  // known 0.00027 dB rather than something that could grow unnoticed.
  {
    WavReplaySource reader;
    RealtimeSampleBlock replayed;
    const bool read_back = reader.open(capture_path.string(), {}) &&
                           reader.start() && reader.read(replayed, 0ms) &&
                           replayed.sample_count == written.size();
    expect(read_back, "a written capture replays every captured frame");
    if (read_back) {
      float worst_excess = 0.0F;
      float worst_error = 0.0F;
      for (std::size_t index = 0; index < written.size(); ++index) {
        const float error =
            std::abs(replayed.samples[index].real() - written[index]);
        // Half a step of PCM16 quantization, plus the one-part-in-32768
        // scale difference, which is proportional to the sample.
        const float bound = (0.5F + std::abs(written[index])) / 32'768.0F;
        worst_error = std::max(worst_error, error);
        worst_excess = std::max(worst_excess, error - bound);
      }
      char report[128]{};
      std::snprintf(report, sizeof(report),
                    "worst error %.9f, worst excess over the bound %.9f",
                    static_cast<double>(worst_error),
                    static_cast<double>(worst_excess));
      expect(worst_excess <= 0.0F,
             std::string("a capture replays within half a PCM16 step plus "
                         "the documented one part in 32768, and no further: ") +
                 report);
    }
    reader.stop();
  }
  std::filesystem::remove(capture_path, removal_error);
}

}  // namespace

int main() {
  test_channel_bank_oscillators_survive_short_blocks();
  test_character_lane_oscillators_survive_long_runs();
  test_wav_scaling_conventions_are_deliberate();
  if (failures == 0) {
    std::cout << "All core numerics tests passed\n";
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
