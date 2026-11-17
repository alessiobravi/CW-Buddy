// The robust keying-level anchor, and the two measurements that fix it where
// it is.
//
// `DSP-006`'s shipped slice regularizes the responsive two-level keying
// tracker against a 256-sample (512 ms at the production 500 Hz evidence
// rate) amplitude history. Otsu's between-class criterion splits that
// history, group medians represent space and mark, and the split may steer
// the live levels only when both groups hold at least a tenth of the window,
// the medians differ by at least two to one in amplitude, and the split
// explains at least 70% of the variation. Nothing tested any of that
// directly: the 70% gate is calibrated against four private no-CW captures
// that cannot be CI fixtures, so a change that weakened it would have been
// caught by nothing a contributor can run.
//
// What this file pins is the presence test rather than the capture outcome:
// a clean keyed carrier must reach the anchor, and band noise must publish
// nothing. It also corrects the record on what the 70% gate is for. The
// gate's calibration is real -- at 65% the `204852` capture asserted `N8EHW`
// and at 70% all four stay clean -- but the mechanism is not that noise fails
// the gate. Measured here, white noise through the narrowband path reaches
// 0.73 and 0.76 explained variation on two draws of three and engages the
// anchor on both. A filtered-noise envelope is heavy-tailed, and a two-class
// split of a heavy-tailed sample genuinely does explain most of its variance.
// What keeps those captures clean is the verification lifecycle downstream,
// which publishes no channel for a callsign to be asserted on.
//
// Two measurements are recorded here so they are not made a third time.
//
// 1. Choosing the split by Morse duration fit rather than by Otsu does not
//    work, and the reason is not the fit. Replacing the selection criterion
//    with a coarse-to-fine search over candidate threshold levels, scoring
//    each by how well the resulting runs fit one- and three-unit marks and
//    one-, three- and seven-unit gaps -- the mechanism a third-party
//    classical decoder uses -- measured neutral: paired full-surface
//    character error 0.2548 to 0.2574, wrong callsigns 26 to 26, with one
//    seed set regressing. The axis itself is the limit. Shifting the chosen
//    split by a constant offset across its whole admissible range moves
//    paired full-surface error only between 0.2641 and 0.2414 (offsets +48
//    to -48 samples of 256), so the entire placement axis is worth 0.023
//    absolute -- below the 0.03 the surface gate asks for and below that
//    surface's own seed spread. Otsu's choice sits in the middle of that
//    range, not at an end, so there is no systematic placement bias to
//    correct.
//
// 2. The anchor's influence rate, not its placement, is what binds. Raising
//    it from 0.015 to 0.25 per fit improves paired full-surface error from
//    0.2548 to 0.1597 and the registered grid from 0.3434 to 0.2292, every
//    seed set improving, while verification, the deterministic timing
//    matrix, the quality gate and the private corpus all hold. It is paid
//    for on the only benchmark carrying fading, drift and hand-sent
//    weighting: held-out receiver character error rises from 0.4576 to
//    0.5254 against a 0.55 gate, and the capture whose speed is lowest, 14
//    WPM, degrades most -- 512 ms holds about six elements there, so the
//    history the anchor trusts is thinnest exactly where it is given the
//    most authority. That trade is why the rate is still 0.015.
#include "cwassistant/core/cw_channel_bank.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "cwassistant/core/spectrum_analyzer.hpp"

namespace {

using cwassistant::core::CwChannelBank;
using cwassistant::core::CwChannelBankConfig;
using cwassistant::core::CwTrackDiagnostic;
using cwassistant::core::RealtimeSampleBlock;
using cwassistant::core::SpectrumAnalyzer;

void expect(const bool condition, const std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

// mt19937's integer sequence is standardized; normal_distribution's mapping is
// not. Twelve summed uniforms keep this waveform identical across libc++,
// libstdc++ and MSVC, exactly as the surface benchmark's generator does.
float portableGaussianLike(std::mt19937& generator) noexcept {
  constexpr double scale = 1.0 / 4'294'967'296.0;
  double sum = 0.0;
  for (int draw = 0; draw < 12; ++draw)
    sum += (static_cast<double>(generator()) + 0.5) * scale;
  return static_cast<float>(sum - 6.0);
}

std::string_view morse(const char symbol) {
  switch (symbol) {
    case 'C': return "-.-.";
    case 'D': return "-..";
    case 'E': return ".";
    case 'Q': return "--.-";
    case 'T': return "-";
    case 'W': return ".--";
    case '1': return ".----";
    case 'A': return ".-";
    default: return "";
  }
}

constexpr double kSampleRate = 48'000.0;
constexpr double kToneHz = 700.0;
constexpr float kNoiseAmplitude = 0.02F;

// Keyed audio at a stated signal-to-noise ratio, with raised-cosine edges, on
// the same scale the surface benchmark uses so that the levels this test
// reasons about are the levels the decoder is measured on.
std::vector<float> keyedAudio(const std::string& message, const double wpm,
                              const float snr_db, const unsigned seed,
                              const double seconds) {
  const double dot = 1.2 / wpm;
  const double rise = 0.005;
  const auto count = static_cast<std::size_t>(seconds * kSampleRate);
  std::vector<float> audio(count, 0.0F);
  const float amplitude =
      kNoiseAmplitude *
      std::sqrt(std::pow(10.0F, snr_db / 10.0F) /
                static_cast<float>(kSampleRate / 2.0 / 120.0));
  std::mt19937 generator(seed);
  double phase = 0.0;
  std::size_t index = static_cast<std::size_t>(0.4 * kSampleRate);
  while (index < count) {
    for (const char symbol : message) {
      if (symbol == ' ') {
        index += static_cast<std::size_t>(4.0 * dot * kSampleRate);
        continue;
      }
      const std::string_view elements = morse(symbol);
      for (std::size_t element = 0; element < elements.size(); ++element) {
        const double length =
            (elements[element] == '-' ? 3.0 * dot : dot);
        const auto span = static_cast<std::size_t>(length * kSampleRate);
        for (std::size_t step = 0; step < span && index < count;
             ++step, ++index) {
          const double into = step / kSampleRate;
          const double from_end = length - into;
          const double attack =
              into < rise ? 0.5 - 0.5 * std::cos(std::numbers::pi * into / rise)
                          : 1.0;
          const double decay =
              from_end < rise
                  ? 0.5 - 0.5 * std::cos(std::numbers::pi * from_end / rise)
                  : 1.0;
          phase += 2.0 * std::numbers::pi * kToneHz / kSampleRate;
          audio[index] += static_cast<float>(amplitude *
                                             std::min(attack, decay) *
                                             std::sin(phase));
        }
        index += static_cast<std::size_t>(dot * kSampleRate);
      }
      index += static_cast<std::size_t>(2.0 * dot * kSampleRate);
    }
  }
  std::mt19937 noise_generator(seed + 1U);
  for (std::size_t step = 0; step < count; ++step)
    audio[step] += kNoiseAmplitude * portableGaussianLike(noise_generator);
  return audio;
}

std::vector<float> noiseOnlyAudio(const unsigned seed, const double seconds) {
  const auto count = static_cast<std::size_t>(seconds * kSampleRate);
  std::vector<float> audio(count, 0.0F);
  std::mt19937 generator(seed);
  for (std::size_t step = 0; step < count; ++step)
    audio[step] = kNoiseAmplitude * portableGaussianLike(generator);
  return audio;
}

struct AnchorObservation {
  bool ever_active{false};
  float highest_explained_variation{0.0F};
  float highest_separation_db{0.0F};
  std::size_t tracks_seen{0};
  std::size_t published_channels{0};
  std::string published_callsigns;
};

AnchorObservation observe(const std::vector<float>& audio) {
  SpectrumAnalyzer analyzer({.audio_upper_frequency_hz = 3'000.0});
  CwChannelBankConfig config;
  config.robust_keying_level_history = true;
  CwChannelBank bank(config);
  RealtimeSampleBlock block;
  block.stream.sample_rate_hz = kSampleRate;
  AnchorObservation observation;
  std::size_t position = 0;
  std::uint64_t now = 0;
  while (position < audio.size()) {
    const std::size_t take =
        std::min<std::size_t>(1'024, audio.size() - position);
    block.sample_count = take;
    block.timestamp_ns = now;
    for (std::size_t index = 0; index < take; ++index)
      block.samples[index] = {audio[position + index], 0.0F};
    for (const auto& snapshot : analyzer.process(block)) {
      static_cast<void>(bank.updateSpectrum(
          snapshot.timestamp_ns, snapshot.lower_frequency_hz,
          snapshot.upper_frequency_hz, snapshot.instantaneous_bins_dbfs,
          false));
    }
    for (const auto& channel : bank.processSamples(block)) {
      ++observation.published_channels;
      if (!channel.callsign.empty() &&
          observation.published_callsigns.find(channel.callsign) ==
              std::string::npos) {
        observation.published_callsigns += channel.callsign;
        observation.published_callsigns += ' ';
      }
    }
    // The filter cascade settles from zero, so the opening of every stream
    // contains a step the history reads as two levels. That transient is an
    // artefact of starting, not evidence about the band, so it is skipped and
    // the question asked is whether the band can SUSTAIN what it shows.
    constexpr std::uint64_t kSettlingNanoseconds = 3'000'000'000ULL;
    if (now >= kSettlingNanoseconds) {
      const std::vector<CwTrackDiagnostic> tracks = bank.allTrackDiagnostics();
      observation.tracks_seen =
          std::max(observation.tracks_seen, tracks.size());
      for (const auto& track : tracks) {
        observation.ever_active =
            observation.ever_active || track.robust_keying_level_anchor_active;
        observation.highest_explained_variation =
            std::max(observation.highest_explained_variation,
                     track.keying_level_explained_variation);
        observation.highest_separation_db =
            std::max(observation.highest_separation_db,
                     track.keying_level_separation_db);
      }
    }
    position += take;
    now += static_cast<std::uint64_t>(static_cast<long double>(take) *
                                      1'000'000'000.0L / kSampleRate);
  }
  return observation;
}

// A keyed carrier must reach the anchor, at every speed the history can
// resolve. This is the half the anchor exists for, and it is what the
// surface benchmark's paired gain is made of.
void testKeyedCarrierReachesTheAnchor() {
  for (const double wpm : {14.0, 22.0, 35.0}) {
    const auto audio = keyedAudio("CQ CQ DE W1AW", wpm, 20.0F, 2'026U, 12.0);
    const auto observed = observe(audio);
    expect(observed.published_channels > 0,
           "a keyed carrier must publish at least one channel");
    expect(observed.ever_active,
           "the robust keying anchor must engage on a clean keyed carrier");
    expect(observed.highest_explained_variation >= 0.90F,
           "a keyed carrier's split must explain nearly all of the variation,"
           " well above the 0.70 acceptance gate");
    // Two to one in amplitude is 6 dB in power, and that is the acceptance
    // gate. A clean carrier must clear it with room, not scrape it.
    expect(observed.highest_separation_db >= 25.0F,
           "a clean keyed carrier must show a wide two-level separation");
  }
}

// What band noise does, and what it does not do -- which is not what the
// record assumed.
//
// The 70% explained-variation gate is often described as what stops a window
// of noise manufacturing a second keying state. Measured, it is not: white
// noise through the narrowband path reaches 0.73 and engages the anchor, with
// the live tracker reporting more than 20 dB of apparent separation, because
// a filtered-noise envelope is heavy-tailed and a two-class split of a
// heavy-tailed sample genuinely explains most of its variance. Two of three
// independent noise draws do it; the third never raises a candidate to look
// at, which is why the assertion below pools them. What holds the no-CW
// recordings at zero false callsigns is downstream of the anchor: the
// verification lifecycle publishes no channel at all, so there is nothing for
// a callsign to be asserted on. That is the assertion worth having, because
// it is the one that is true, and because it is exactly the discipline a
// decoder scored against this project's captures turned out to have none of.
void testNoiseSustainsNoPublication() {
  float highest_noise_explained_variation = 0.0F;
  bool anchor_ever_engaged_on_noise = false;
  for (const unsigned seed : {11U, 2'027U, 90'210U}) {
    const auto audio = noiseOnlyAudio(seed, 12.0);
    const auto observed = observe(audio);
    expect(observed.published_channels == 0,
           "band noise must publish no channel");
    expect(observed.published_callsigns.empty(),
           "band noise must assert no callsign");
    highest_noise_explained_variation =
        std::max(highest_noise_explained_variation,
                 observed.highest_explained_variation);
    anchor_ever_engaged_on_noise =
        anchor_ever_engaged_on_noise || observed.ever_active;
  }
  // Pooled across the draws, because not every draw even raises a candidate
  // to look at. Two of these three reach 0.73 and 0.76 and engage the anchor.
  // Pinning that keeps the sentence above honest: if a future change makes
  // the gate a real noise rejector, this is where the record gets corrected
  // rather than quietly inherited.
  expect(anchor_ever_engaged_on_noise &&
             highest_noise_explained_variation >= 0.70F,
         "the 70% gate is not a noise rejector in isolation, and the record"
         " must not start saying that it is");
}

// Switching the history off must change the answer. The surface benchmark
// gates on the paired difference between these two paths, so a build in
// which the flag had quietly stopped being read would still pass every
// absolute figure while measuring one path twice.
void testHistoryFlagIsHonoured() {
  const auto audio = keyedAudio("CQ CQ DE W1AW", 22.0, 20.0F, 2'026U, 12.0);
  SpectrumAnalyzer analyzer({.audio_upper_frequency_hz = 3'000.0});
  CwChannelBankConfig config;
  config.robust_keying_level_history = false;
  CwChannelBank bank(config);
  RealtimeSampleBlock block;
  block.stream.sample_rate_hz = kSampleRate;
  bool ever_active = false;
  float highest_explained_variation = 0.0F;
  std::size_t position = 0;
  std::uint64_t now = 0;
  while (position < audio.size()) {
    const std::size_t take =
        std::min<std::size_t>(1'024, audio.size() - position);
    block.sample_count = take;
    block.timestamp_ns = now;
    for (std::size_t index = 0; index < take; ++index)
      block.samples[index] = {audio[position + index], 0.0F};
    for (const auto& snapshot : analyzer.process(block)) {
      static_cast<void>(bank.updateSpectrum(
          snapshot.timestamp_ns, snapshot.lower_frequency_hz,
          snapshot.upper_frequency_hz, snapshot.instantaneous_bins_dbfs,
          false));
    }
    static_cast<void>(bank.processSamples(block));
    for (const auto& track : bank.allTrackDiagnostics()) {
      ever_active = ever_active || track.robust_keying_level_anchor_active;
      highest_explained_variation =
          std::max(highest_explained_variation,
                   track.keying_level_explained_variation);
    }
    position += take;
    now += static_cast<std::uint64_t>(static_cast<long double>(take) *
                                      1'000'000'000.0L / kSampleRate);
  }
  expect(!ever_active,
         "no anchor may engage when the robust history is switched off");
  expect(highest_explained_variation == 0.0F,
         "no split evidence may be reported when the history is switched off");
}

}  // namespace

int main() {
  testKeyedCarrierReachesTheAnchor();
  testNoiseSustainsNoPublication();
  testHistoryFlagIsHonoured();
  std::cout << "cw_keying_level_anchor_tests: PASS\n";
}
