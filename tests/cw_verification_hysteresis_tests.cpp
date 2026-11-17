// Deterministic tests for the plausibility gate's sustained-failure
// hysteresis, for what a demoted track is held for, and for decoder recovery
// on a track that has already verified.
//
// These three are one mechanism. A verified stream whose recent copy turns
// into a run of E and T used to be demoted on the first frame that read that
// way -- the only gate in the bank that did not wait for sustained failure --
// and a demoted track was then given a candidate's fraction of a second
// rather than the advertised decoded retention, so it died at the first word
// gap. The colour slot it had been holding was taken over by a new track on
// the same carrier, which is what an operator sees as a stream freezing,
// going grey, and coming back as somebody else. Measured across twenty-two
// receiver captures, this was 67 of 114 demotions and 44 marker handovers.
//
// What these tests pin down is that the demotion is still there and still
// refuses what it was calibrated to refuse, and that it now has to be right
// for `verification_exit_seconds` before it may act on a stream that has
// already proved itself.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

#include "cwassistant/core/cw_channel_bank.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

using cwassistant::core::CwChannelBank;
using cwassistant::core::CwChannelBankConfig;
using cwassistant::core::CwTrackState;
using cwassistant::core::CwVerificationReason;

constexpr double kSampleRateHz = 8'000.0;
constexpr double kCarrierHz = 500.0;

// One carrier, keyed on command, driven through the bank exactly as the live
// path drives it: a spectrum frame and the matching audio block per step.
class Carrier {
 public:
  explicit Carrier(const CwChannelBankConfig& config) : bank_(config) {}

  void feed(const bool keyed, const int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) {
      bins_.assign(bins_.size(), -110.0F);
      if (keyed) bins_[static_cast<std::size_t>(kCarrierHz)] = -65.0F;
      static_cast<void>(bank_.updateSpectrum(now_, 0.0, 1'000.0, bins_));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = kSampleRateHz;
      block.timestamp_ns = now_;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            keyed ? 0.30F * static_cast<float>(std::sin(phase_)) : 0.0F, 0.0F};
        phase_ += 2.0 * std::numbers::pi * kCarrierHz / kSampleRateHz;
      }
      static_cast<void>(bank_.processSamples(block));
      now_ += 10'000'000U;
    }
  }

  // A dot and a dash at about 20 words per minute, each followed by a
  // character gap. Nothing else is sent, so every character the decoder can
  // produce from this is an E or a T: it is the exact copy the plausibility
  // gate exists to refuse.
  void sendSimpleCharacters(const int repetitions) {
    for (int repetition = 0; repetition < repetitions; ++repetition) {
      feed(true, 60);
      feed(false, 180);
      feed(true, 180);
      feed(false, 180);
    }
  }

  // Silence with no carrier at all, which is what the retention clock
  // measures.
  void idle(const int milliseconds) { feed(false, milliseconds); }

  [[nodiscard]] const CwChannelBank& bank() const noexcept { return bank_; }
  [[nodiscard]] CwChannelBank& bank() noexcept { return bank_; }

  // The one track the fixture creates, or nullopt once it has expired.
  [[nodiscard]] std::optional<cwassistant::core::CwTrackDiagnostic> track(
      const std::uint64_t id) const {
    for (const auto& diagnostic : bank_.allTrackDiagnostics()) {
      if (diagnostic.id == id) return diagnostic;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<cwassistant::core::CwTrackDiagnostic> onCarrier()
      const {
    for (const auto& diagnostic : bank_.allTrackDiagnostics()) {
      if (std::abs(diagnostic.frequency_hz - kCarrierHz) < 20.0)
        return diagnostic;
    }
    return std::nullopt;
  }

 private:
  CwChannelBank bank_;
  std::vector<float> bins_ = std::vector<float>(1'001, -110.0F);
  std::uint64_t now_ = 0;
  double phase_ = 0.0;
};

// A bank that verifies a carrier on its spectral evidence alone, so that the
// plausibility gate is the only thing in these fixtures that can demote it.
CwChannelBankConfig permissiveConfig() {
  return {
      .minimum_separation_hz = 45.0,
      .minimum_spectral_observations = 1,
      .minimum_verification_symbols = 0,
      .minimum_key_transitions = 0,
      .minimum_cadence_observations = 0,
      .minimum_verification_timing_quality = 0.0F,
      .minimum_verification_cadence_quality = 0.0F,
      .minimum_character_confidence = 0.0F,
      .minimum_plausibility_check_characters = 6,
      .maximum_simple_character_fraction = 0.35F,
      .minimum_narrowband_coherence = 0.0F,
      .maximum_verification_unknown_fraction = 1.0F,
      .verification_enter_seconds = 0.0,
  };
}

// A verified stream whose recent copy turns implausible keeps its identity
// while the reading is still a transient, and the operator keeps the
// transcript it had already earned.
void test_transient_implausible_text_does_not_demote_a_verified_stream() {
  CwChannelBankConfig config = permissiveConfig();
  config.verification_exit_seconds = 6.0;
  // Recovery is a separate mechanism with its own test below. Pushed out of
  // reach here so this fixture measures the hysteresis and nothing else.
  config.decoder_recovery_seconds = 1'000.0;
  Carrier carrier(config);

  carrier.feed(true, 400);
  const auto established = carrier.onCarrier();
  expect(established.has_value() &&
             established->verification_state == CwTrackState::Verified,
         "hysteresis fixture establishes a verified carrier");
  if (!established.has_value()) return;
  const std::uint64_t id = established->id;
  const auto& published = carrier.bank().channels();
  const std::uint8_t color =
      published.empty() ? 0U : published.front().color_index;

  // About three seconds of copy that reads as timing noise: half the exit
  // window, so a gate with hysteresis has not finished deciding.
  carrier.sendSimpleCharacters(5);
  const auto during = carrier.track(id);
  expect(during.has_value() &&
             during->verification_state == CwTrackState::Verified,
         "a verified stream survives a few seconds of implausible copy "
         "instead of being demoted on the first frame that reads that way");
  expect(during.has_value() &&
             during->verification_reason ==
                 CwVerificationReason::ImplausibleCharacterDistribution,
         "the stream reports the implausible reading while it stands, so "
         "the text and the station name it would carry are withheld");
  const auto& channels = carrier.bank().channels();
  expect(channels.size() == 1 && channels.front().id == id &&
             channels.front().color_index == color,
         "the published marker keeps its id and its colour across the "
         "transient");
  expect(channels.size() == 1 && channels.front().active &&
             channels.front().verification_state == CwTrackState::Verified,
         "and the marker stays live rather than greying out, so the carrier "
         "the operator can see is still shown as a stream they can tune");
}

// The defence the gate was calibrated for is kept, not removed: copy that
// stays implausible past the exit window still loses verification.
void test_sustained_implausible_text_still_demotes() {
  CwChannelBankConfig config = permissiveConfig();
  config.verification_exit_seconds = 6.0;
  config.decoder_recovery_seconds = 1'000.0;
  Carrier carrier(config);

  carrier.feed(true, 400);
  const auto established = carrier.onCarrier();
  expect(established.has_value() &&
             established->verification_state == CwTrackState::Verified,
         "sustained-failure fixture establishes a verified carrier");
  if (!established.has_value()) return;
  const std::uint64_t id = established->id;

  // Well past the six-second exit window.
  carrier.sendSimpleCharacters(20);
  const auto after = carrier.track(id);
  expect(after.has_value() &&
             after->verification_state != CwTrackState::Verified &&
             after->verification_reason ==
                 CwVerificationReason::ImplausibleCharacterDistribution,
         "copy that stays implausible for longer than the exit window still "
         "demotes the stream");
  const auto& demoted_channels = carrier.bank().channels();
  expect(demoted_channels.empty() ||
             (demoted_channels.size() == 1 &&
              !demoted_channels.front().active &&
              demoted_channels.front().verification_state ==
                  CwTrackState::Lost),
         "a demoted stream stops being a live channel: what is left is the "
         "remembered region, greyed and inactive, on its own expiry clock");
}

// Nothing here relaxes the gate for the population it was calibrated
// against. A carrier that has never verified is refused as soon as its text
// reads as timing noise, with no grace at all.
void test_unverified_implausible_text_is_refused_immediately() {
  CwChannelBankConfig config = permissiveConfig();
  // Out of reach, so this track cannot verify on any evidence.
  config.minimum_verification_symbols = 10'000;
  config.verification_exit_seconds = 6.0;
  config.decoder_recovery_seconds = 1'000.0;
  Carrier carrier(config);

  carrier.feed(true, 400);
  carrier.sendSimpleCharacters(5);
  const auto refused = carrier.onCarrier();
  expect(refused.has_value() &&
             refused->verification_state != CwTrackState::Verified &&
             refused->verification_reason ==
                 CwVerificationReason::ImplausibleCharacterDistribution,
         "a carrier that has never verified is still refused on implausible "
         "text immediately, with no grace window");
  for (const auto& channel : carrier.bank().channels()) {
    expect(channel.callsign.empty(),
           "and it names no station, so there is nothing for a callsign "
           "to be hung on");
  }
}

// A demoted track is not a candidate. It keeps a hold long enough for the one
// path that can rescue it to run, instead of the fraction of a second a
// carrier the bank has never believed in is given.
void test_a_demoted_track_outlives_a_candidate_hold() {
  CwChannelBankConfig config = permissiveConfig();
  // Demote on the first implausible frame, so the fixture measures retention
  // and not the exit window.
  config.verification_exit_seconds = 0.0;
  config.decoder_recovery_seconds = 3.0;
  // A candidate on this bank is given a quarter of a second, so the two holds
  // cannot be confused for one another.
  config.empty_track_retention_seconds = 0.25;
  config.unverified_track_retention_seconds = 0.25;
  Carrier carrier(config);

  carrier.feed(true, 400);
  const auto established = carrier.onCarrier();
  expect(established.has_value() &&
             established->verification_state == CwTrackState::Verified,
         "retention fixture establishes a verified carrier");
  if (!established.has_value()) return;
  const std::uint64_t id = established->id;

  carrier.sendSimpleCharacters(6);
  const auto demoted = carrier.track(id);
  expect(demoted.has_value() &&
             demoted->verification_state != CwTrackState::Verified,
         "retention fixture demotes the established carrier");

  // One second of nothing at all: four candidate holds, and well inside the
  // recovery window.
  carrier.idle(1'000);
  expect(carrier.track(id).has_value(),
         "a track that has been verified survives a word gap after it is "
         "demoted rather than dying at the first one");

  // Past the recovery window, with the carrier genuinely gone, it is given
  // up like anything else.
  carrier.idle(4'000);
  expect(!carrier.track(id).has_value(),
         "and it is still given up once the carrier has been gone longer "
         "than the hold");
}

// The rescue path used to exclude every track that had ever verified, which
// excluded exactly the tracks the plausibility gate kills. Lifting that lets
// a stuck decoder reacquire in place, keeping the identity the operator is
// watching and the transcript it has already shown them.
void test_recovery_reaches_a_track_that_has_verified() {
  CwChannelBankConfig config = permissiveConfig();
  config.verification_exit_seconds = 0.0;
  config.decoder_recovery_seconds = 0.5;
  Carrier carrier(config);

  carrier.feed(true, 400);
  const auto established = carrier.onCarrier();
  expect(established.has_value() &&
             established->verification_state == CwTrackState::Verified,
         "recovery fixture establishes a verified carrier");
  if (!established.has_value()) return;
  const std::uint64_t id = established->id;

  carrier.sendSimpleCharacters(12);
  expect(carrier.bank().verificationDiagnostics().decoder_reacquisitions > 0,
         "a decoder trapped in implausible output reacquires even though "
         "its track has already verified");
  const auto survivor = carrier.track(id);
  expect(survivor.has_value(),
         "the reacquisition happens in place: the track keeps its id, so no "
         "new identity or colour appears on the carrier");
  expect(carrier.bank().allTrackDiagnostics().size() == 1,
         "and no duplicate track is spawned on the same carrier");
}

}  // namespace

int main() {
  test_transient_implausible_text_does_not_demote_a_verified_stream();
  test_sustained_implausible_text_still_demotes();
  test_unverified_implausible_text_is_refused_immediately();
  test_a_demoted_track_outlives_a_candidate_hold();
  test_recovery_reaches_a_track_that_has_verified();
  if (failures == 0) std::cout << "cw_verification_hysteresis_tests: PASS\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
