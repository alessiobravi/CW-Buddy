#include "cwassistant/core/cw_channel_bank.hpp"
#include "cwassistant/core/cw_vocabulary.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/cw_callsign_prefixes.hpp"

namespace cwassistant::core {
namespace {
// Drops participants whose opening characters name no allocated country, by
// the same reasoning as the stream label. A QSO list is read as a list of
// stations, so an impossible prefix in it is as misleading there as it is on
// the card's own heading.
[[nodiscard]] std::vector<std::string> allocatedParticipants(
    std::vector<std::string> participants) {
  const auto& prefixes = cwSharedCallsignPrefixes();
  std::erase_if(participants, [&prefixes](const std::string& participant) {
    return !prefixes.isAllocatedPrefix(participant);
  });
  return participants;
}

// Removes runs of characters that are almost certainly fragments rather than
// copy, leaving the copy around them intact.
//
// E, T, I, A, N and M are the one- and two-element characters. When the keying
// evidence breaks up, what comes out is a run of them, and that happens inside
// an otherwise good track as readily as in a bad one: a track that reads
// "CQ POTA DE SN5WLF" correctly several times fills the spaces between with
// single-element runs. Suppressing whole tracks on this measure is therefore
// wrong, and measurably so -- across the capture corpus, tracks that recovered
// a correct callsign reach runs of ten themselves.
//
// Six is the shortest run that is safe. Real copy does reach four and five: a
// callsign rarely exceeds three, but ordinary text does. So the run is
// replaced by a single space, which says plainly that something here was not
// copyable, rather than joining unrelated text together as deletion would.
std::string suppressFragmentRuns(const std::string_view text) {
  constexpr std::string_view kShortCharacters = "ETIANM";
  constexpr std::size_t kMinimumFragmentRun = 6;
  std::string result;
  result.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    // Measure a run from here, counting only the characters: spaces inside a
    // run of fragments are part of the same damage, not a break in it.
    std::size_t scan = index;
    std::size_t characters = 0;
    std::size_t last_character_end = index;
    while (scan < text.size()) {
      const char symbol = text[scan];
      if (symbol == ' ') {
        ++scan;
        continue;
      }
      if (kShortCharacters.find(symbol) == std::string_view::npos) break;
      ++characters;
      ++scan;
      last_character_end = scan;
    }
    if (characters >= kMinimumFragmentRun) {
      if (!result.empty() && result.back() != ' ') result.push_back(' ');
      index = last_character_end;
      continue;
    }
    result.push_back(text[index]);
    ++index;
  }
  return result;
}


constexpr std::array<double, 3> kNarrowbandWidthsHz{60.0, 120.0, 240.0};
// The two widths the keyer-resolution test compares. They are the narrowest
// and widest the bank already filters every track at, so the measurement costs
// no extra signal path: only the two power sums that are computed anyway are
// read a second time. The pair is also the one the capture evidence behind
// `maximum_single_keyer_speed_ratio` was measured with.
constexpr std::size_t kKeyerNarrowWidthIndex = 0;
constexpr std::size_t kKeyerWideWidthIndex = 2;
// Mark power must exceed space power by this factor before a width's run
// history is believed. A two-level model fitted to one population of noise
// always finds some split; without this bar an idle channel reads as fast
// chatter at both widths, and because the wide filter chatters four times as
// fast as the narrow one, silence alone would look exactly like a pileup.
//
// Fourteen decibels, and it cannot simply be raised further: the tracker it
// reads is deliberately asymmetric, so on real keying the separation it
// reports is optimistic, while two superimposed carriers keep the channel
// energised through the beat and report a separation that is pessimistic.
// Measured on the synthetic fixtures, raising this to 18 dB stopped
// recognising genuine two-keyer channels at every speed and simultaneously
// produced a new false reading on a clean 40 WPM one. The level guard below
// carries the weak-signal case instead.
constexpr float kKeyerRunLevelSeparation = 25.0F;
// Fraction of recent evidence frames on which the NARROW width must have shown
// a believable two-level split before the comparison is allowed to refuse a
// track.
//
// The ratio only means something if the reference half of it was measurable.
// Noise fragments the wide filter as readily as a neighbour does, so on a
// channel too weak or too damaged for even the narrowest filter to see clean
// keying, the two causes cannot be told apart at all and the honest answer is
// no answer. Measured on the synthetic fixtures: the two clean single keyers
// that were misread as several -- 16 WPM at 12 and at 10 dB -- showed narrow
// duties of 0.58 and 0.49, while every clean channel that read correctly and
// every genuine two-keyer channel down to 12 dB sat at 0.62 or above. A
// genuine pileup keeps this high precisely because its own carrier is still
// dominant inside 60 Hz, where a neighbour 60 Hz away is more than twenty
// decibels down.
//
// Known limit, stated rather than hidden: this leaves one case standing. A
// clean 16 WPM carrier at 12 dB still reads as several keyers, because its
// duty crosses the bar for long enough to carry a verdict. Raising the bar to
// 0.65 does not remove it and starts costing genuine two-keyer channels, and
// on the decoder surface benchmark the case costs nothing measurable -- the
// same 26 correct and 7 wrong callsigns, and a per-seed-set character error
// within a hundredth of the pre-gate build. It stays on the list.
constexpr float kKeyerMinimumNarrowDuty = 0.60F;
// A noise excursion smaller than this fraction of the keying separation does
// not open or close a run. Real fragmentation from a superimposed neighbour
// swings the whole separation; this only stops the envelope chattering across
// its own midpoint. Applied symmetrically so it biases neither mark nor space.
constexpr float kKeyerRunHysteresisFraction = 0.15F;
// Evidence frames between verdict evaluations, and how many consecutive
// evaluations must agree before the standing verdict changes. Half a second at
// the default evidence rate, so three of them is about one and a half seconds
// of consistent evidence -- long enough that a pileup thinning for one word,
// or a DX pausing mid-transmission, cannot flip a track between publishing
// text and withholding it.
constexpr std::uint16_t kKeyerEvaluationFrames = 250;
constexpr std::uint8_t kKeyerVerdictEvaluations = 3;
// Tracked carriers that must sit inside the joint-separation radius, besides
// the track itself, before a collapsed cadence is read as several keyers
// rather than as poor copy.
//
// Two, so three carriers in the neighbourhood. One neighbour is the two-signal
// case, which the speed-ratio comparison already answers well -- on the
// two-carrier synthetic fixture it crosses its limit and refuses, while the
// cadence there falls only to 0.637 and would not -- so reading the cadence
// there costs what the ratio was already earning. Three or more is the case
// the ratio goes silent on. Measured on the decoder surface benchmark,
// this is exactly the difference between a build whose per-seed-set character
// error is identical to the pre-change baseline and one that pays 0.014 and
// 0.021 on two of the three sets: the benchmark's low-fit tracks reach one
// neighbour, never two, while the capture's pileup carriers reach seven and
// eight.
constexpr std::uint8_t kKeyerCadenceMinimumNeighbours = 2;
// Frequency radius inside which a later stage would have to solve carriers
// jointly, and the largest number of sources such a solve stays conditioned
// for. Both come from the separation experiment on the operator's capture:
// neighbours within about 220 Hz were solved together, three to six sources
// worked, and the dense centre of the pileup -- six or more stations at 50 to
// 70 Hz spacing -- did not. Recorded here only to mark which refused tracks
// are worth that work; nothing in this file attempts the solve.
constexpr double kJointSeparationRadiusHz = 220.0;
constexpr std::size_t kJointSeparationMaximumSources = 5;
constexpr double kCandidateMatchHoldSeconds = 0.75;
constexpr double kManualSelectionReuseToleranceHz = 12.0;
constexpr double kCharacterRefinementEvidenceSeconds = 3.0;
constexpr std::size_t kMaximumPresentationText = 2'048;

void trimPresentationText(std::string& text) {
  if (text.size() <= kMaximumPresentationText) return;
  text.erase(0, text.size() - kMaximumPresentationText);
}

template <std::size_t Size>
void sortPrefix(std::array<double, Size>& values,
                const std::size_t requested_count) noexcept {
  const std::size_t count = std::min(requested_count, Size);
  for (std::size_t index = 1; index < count; ++index) {
    const double value = values[index];
    std::size_t insertion = index;
    while (insertion > 0U && values[insertion - 1U] > value) {
      values[insertion] = values[insertion - 1U];
      --insertion;
    }
    values[insertion] = value;
  }
}

[[nodiscard]] std::string composePresentationText(
    const std::string& prefix, const std::string& source_text) {
  if (prefix.empty()) {
    std::string result = source_text;
    trimPresentationText(result);
    return result;
  }
  if (source_text.empty()) return prefix;
  std::string result = prefix;
  result += " | ";
  result += source_text;
  trimPresentationText(result);
  return result;
}

}  // namespace

const char* cwTrackStateName(const CwTrackState state) noexcept {
  switch (state) {
    case CwTrackState::Candidate: return "candidate";
    case CwTrackState::MorseLikely: return "morse-likely";
    case CwTrackState::Verified: return "verified";
    case CwTrackState::Lost: return "lost";
  }
  return "candidate";
}

bool cwTextContainsDistinctiveToken(const std::string_view text) noexcept {
  // Whole tokens only. A CQ found inside a longer run of letters is far more
  // likely to be three noise elements that happened to land together than a
  // station calling, and this evidence is only worth having while it stays
  // harder to counterfeit than the gates it stands in for.
  // The set lives in dictionaries/cw-distinctive-tokens.txt, deliberately
  // narrower than the exchange vocabulary; see the note there.
  const auto& vocabulary = cwSharedVocabulary();
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t end = std::min(text.find(' ', begin), text.size());
    const std::string_view token = text.substr(begin, end - begin);
    if (!token.empty()) {
      if (vocabulary.isDistinctiveToken(token)) return true;
    }
    if (end == text.size()) break;
    begin = end + 1;
  }
  return false;
}

const char* cwVerificationReasonName(
    const CwVerificationReason reason) noexcept {
  switch (reason) {
    case CwVerificationReason::NeedsSpectralPersistence:
      return "needs-spectral-persistence";
    case CwVerificationReason::NeedsKeyingEdges: return "needs-keying-edges";
    case CwVerificationReason::NeedsCadenceEvidence:
      return "needs-cadence-evidence";
    case CwVerificationReason::LowNarrowbandCoherence:
      return "low-narrowband-coherence";
    case CwVerificationReason::LowCadenceQuality: return "low-cadence-quality";
    case CwVerificationReason::NeedsDecodedSymbols:
      return "needs-decoded-symbols";
    case CwVerificationReason::TooManyUnknownSymbols:
      return "too-many-unknown-symbols";
    case CwVerificationReason::LowTimingQuality: return "low-timing-quality";
    case CwVerificationReason::LowCharacterConfidence:
      return "low-character-confidence";
    case CwVerificationReason::NeedsSustainedEvidence:
      return "needs-sustained-evidence";
    case CwVerificationReason::ImplausibleCharacterDistribution:
      return "implausible-character-distribution";
    case CwVerificationReason::Verified: return "verified";
    case CwVerificationReason::SignalLost: return "signal-lost";
    case CwVerificationReason::UnresolvedKeyerOverlap:
      return "unresolved-keyer-overlap";
  }
  return "needs-spectral-persistence";
}

double cwKeyingSpeedFromRuns(const std::span<const std::uint16_t> run_frames,
                             const double frame_rate_hz) noexcept {
  if (run_frames.empty() || !(frame_rate_hz > 0.0)) return 0.0;
  // Bounded and on the stack: this runs inside the evidence path, and the
  // caller's window is a fixed ring far smaller than this.
  constexpr std::size_t kMaximumRuns = 256;
  std::array<std::uint16_t, kMaximumRuns> ordered{};
  const std::size_t count = std::min(run_frames.size(), kMaximumRuns);
  std::copy_n(run_frames.begin(), count, ordered.begin());
  const std::size_t middle = count / 2;
  std::nth_element(ordered.begin(),
                   ordered.begin() + static_cast<std::ptrdiff_t>(middle),
                   ordered.begin() + static_cast<std::ptrdiff_t>(count));
  const double element_frames = static_cast<double>(ordered[middle]);
  if (!(element_frames > 0.0)) return 0.0;
  // PARIS: one word is fifty elements per minute-normalised unit, so a speed
  // in words per minute is 1.2 divided by the element length in seconds.
  return 1.2 / (element_frames / frame_rate_hz);
}

bool isCharacterDistributionImplausible(
    const std::string& text, const std::uint16_t minimum_characters,
    const float maximum_simple_character_fraction) noexcept {
  if (text.size() < minimum_characters) return false;
  std::size_t letters = 0;
  std::size_t simple = 0;
  for (const char character : text) {
    if (character == ' ') continue;
    ++letters;
    if (character == 'E' || character == 'T') ++simple;
  }
  return letters > 0 &&
         static_cast<float>(simple) / static_cast<float>(letters) >
             maximum_simple_character_fraction;
}

std::uint32_t cwCountCallsignOccurrences(
    const std::string_view text, const std::string_view callsign) noexcept {
  if (callsign.empty() || text.size() < callsign.size()) return 0;
  // Slash is part of a callsign token (F/DK7SS, DK7SS/P), so a match that
  // touches one is a match inside a longer call, not a reading of this one.
  const auto token_character = [](const char symbol) noexcept {
    return (symbol >= '0' && symbol <= '9') ||
           (symbol >= 'A' && symbol <= 'Z') ||
           (symbol >= 'a' && symbol <= 'z') || symbol == '/';
  };
  std::uint32_t occurrences = 0;
  for (std::size_t index = text.find(callsign); index != std::string_view::npos;
       index = text.find(callsign, index + 1U)) {
    const std::size_t end = index + callsign.size();
    if ((index == 0U || !token_character(text[index - 1U])) &&
        (end == text.size() || !token_character(text[end]))) {
      ++occurrences;
    }
  }
  return occurrences;
}

CwStreamLabel cwApplyStreamLabelReading(CwStreamLabel label,
                                        const std::string_view reading,
                                        const std::string_view primary_text,
                                        const std::string_view refined_text,
                                        const std::uint32_t corroborations) {
  // Silence is not evidence of a new station, and neither is a stretch of copy
  // too poor to read a call out of. Both arrive here as an empty reading, and
  // both leave the stream wearing the name it already had.
  if (reading.empty()) return label;
  // A caller asking for no corroboration at all is asking for the name to
  // follow every reading; zero would instead establish an empty name.
  const std::uint32_t bar = std::max<std::uint32_t>(corroborations, 1U);
  const std::uint32_t support = std::max<std::uint32_t>(
      {1U, cwCountCallsignOccurrences(primary_text, reading),
       cwCountCallsignOccurrences(refined_text, reading)});

  if (reading == label.callsign) {
    label.support = std::max(label.support, support);
    // A contested name that has now been corroborated is a name again: what
    // was withheld was a single unsupported reading, not this station's
    // identity.
    if (label.support >= bar) label.contested = false;
    // The station said its name again. Whatever was arguing that this is
    // somebody else has just been answered, so it starts over rather than
    // keeping credit earned against a name that is still being confirmed.
    label.challenger.clear();
    label.challenger_support = 0;
    return label;
  }
  if (label.support < bar) {
    // Provisional. One clean identification is enough to name a stream -- an
    // operator wants the call the moment it is copied -- it is only enough to
    // *keep* the name that has to be earned twice.
    //
    // Displacing one uncorroborated name with another is the one case where
    // naming the stream at all is wrong. Two single readings that contradict
    // each other say the copy is not good enough to read a call out of; they
    // do not say the later one is right. On the operator's pileup capture the
    // one stream that identified clearly wore EH4ST for ninety seconds and
    // then E5Q for the last two, and the station was EH3ST: the flip was the
    // evidence that neither reading could be trusted, and the operator saw the
    // second one as though it were an answer. The reading is still taken, so
    // it is there to be confirmed; until something confirms it the stream is
    // published without a name.
    const bool displaced = !label.callsign.empty();
    label.callsign.assign(reading);
    label.support = support;
    label.contested = displaced && support < bar;
    label.challenger.clear();
    label.challenger_support = 0;
    return label;
  }
  if (reading == label.challenger) {
    label.challenger_support = std::max(label.challenger_support, support);
  } else {
    label.challenger.assign(reading);
    label.challenger_support = support;
  }
  if (label.challenger_support >= bar) {
    // A rival that cleared the same bar is a station identifying, not a
    // misdecode, so it takes the stream over with the evidence it brought.
    label.callsign = std::move(label.challenger);
    label.support = label.challenger_support;
    label.contested = false;
    label.challenger.clear();
    label.challenger_support = 0;
  }
  return label;
}

CwChannelBank::Track::Track(const std::uint64_t track_id,
                            const double frequency,
                            const std::uint64_t timestamp_ns)
    : id(track_id),
      frequency_hz(frequency),
      identity_origin_frequency_hz(frequency),
      presentation_frequency_hz(frequency),
      last_detected_ns(timestamp_ns),
      last_frequency_update_ns(timestamp_ns),
      last_candidate_match_ns(timestamp_ns) {}

CwChannelBank::CwChannelBank(CwChannelBankConfig config) : config_(config) {
  sanitizeConfig();
}

void CwChannelBank::configure(CwChannelBankConfig config) noexcept {
  // Settings that have their own setters are carried across rather than reset
  // to the struct defaults. A caller changing one unrelated value passes a
  // freshly constructed config, and replacing the whole thing silently undid
  // everything set through a setter: the station callsign already survived
  // only because the application happened to re-apply it afterwards, and the
  // weak-signal gate would have been reset the same way. Whoever owns a value
  // keeps it.
  const auto own_callsign = std::move(config_.own_callsign);
  const bool decode_weak_signals = config_.decode_weak_signals;
  const float minimum_decode_snr_db = config_.minimum_decode_snr_db;
  config_ = std::move(config);
  if (config_.own_callsign.empty()) config_.own_callsign = own_callsign;
  config_.decode_weak_signals = decode_weak_signals;
  config_.minimum_decode_snr_db = minimum_decode_snr_db;
  sanitizeConfig();
}

void CwChannelBank::setWeakSignalDecoding(
    const bool enabled, const float minimum_decode_snr_db) noexcept {
  config_.decode_weak_signals = enabled;
  config_.minimum_decode_snr_db =
      std::clamp(minimum_decode_snr_db, 0.0F, 40.0F);
}

void CwChannelBank::setKeyingModel(const CwKeyingModel model) noexcept {
  if (keying_model_ == model) return;
  keying_model_ = model;
  applyKeyingModel();
}

void CwChannelBank::applyKeyingModel() noexcept {
  for (auto& track : tracks_) track.decoder.setKeyingModel(keying_model_);
}

void CwChannelBank::setOwnCallsign(std::string callsign) {
  config_.own_callsign = std::move(callsign);
}

void CwChannelBank::sanitizeConfig() noexcept {
  config_.acquisition_snr_db =
      std::clamp(config_.acquisition_snr_db, 3.0F, 30.0F);
  config_.retention_snr_db =
      std::clamp(config_.retention_snr_db, 0.0F, config_.acquisition_snr_db);
  config_.detection_dynamic_range_db =
      std::clamp(config_.detection_dynamic_range_db, 40.0F, 140.0F);
  config_.minimum_peak_prominence_db =
      std::clamp(config_.minimum_peak_prominence_db, 0.0F, 30.0F);
  config_.minimum_near_peak_prominence_db =
      std::clamp(config_.minimum_near_peak_prominence_db, 0.0F,
                 config_.minimum_peak_prominence_db);
  config_.prominence_reference_offset_hz =
      std::clamp(config_.prominence_reference_offset_hz, 40.0, 1'000.0);
  config_.prominence_reference_width_hz =
      std::clamp(config_.prominence_reference_width_hz, 20.0,
                 config_.prominence_reference_offset_hz);
  config_.minimum_separation_hz =
      std::clamp(config_.minimum_separation_hz, 5.0, 500.0);
  config_.tracking_tolerance_hz = std::clamp(
      config_.tracking_tolerance_hz, config_.minimum_separation_hz, 1'000.0);
  config_.empty_track_retention_seconds =
      std::clamp(config_.empty_track_retention_seconds, 0.5, 30.0);
  config_.decoded_track_retention_seconds =
      std::clamp(config_.decoded_track_retention_seconds,
                 config_.empty_track_retention_seconds, 300.0);
  config_.unverified_track_retention_seconds =
      std::clamp(config_.unverified_track_retention_seconds, 0.2, 5.0);
  // One is "no extra hold at all", which a caller must be able to ask for; the
  // upper bound keeps a remembered region from outstaying the operator's
  // memory of the station that made it.
  config_.identified_stream_retention_multiplier =
      std::clamp(config_.identified_stream_retention_multiplier, 1.0, 4.0);
  config_.color_identity_retention_seconds =
      std::clamp(config_.color_identity_retention_seconds, 300.0, 3'600.0);
  config_.parked_track_retention_seconds =
      std::clamp(config_.parked_track_retention_seconds, 5.0, 1'800.0);
  config_.color_identity_tolerance_hz = std::clamp(
      config_.color_identity_tolerance_hz, 5.0, config_.tracking_tolerance_hz);
  config_.narrowband_width_hz =
      std::clamp(config_.narrowband_width_hz, 40.0, 500.0);
  config_.noise_reference_offset_hz = std::clamp(
      config_.noise_reference_offset_hz, kNarrowbandWidthsHz.back(), 2'000.0);
  config_.evidence_rate_hz =
      std::clamp(config_.evidence_rate_hz, 100.0, 2'000.0);
  config_.maximum_tracks = std::clamp<std::size_t>(
      config_.maximum_tracks, 1, kCwMaximumTrackCapacity);
  if (!std::isfinite(config_.detector_averaging_seconds)) {
    config_.detector_averaging_seconds = 0.05;
  }
  config_.detector_averaging_seconds =
      std::clamp(config_.detector_averaging_seconds, 0.0, 1.0);
  if (!std::isfinite(config_.detector_frame_interval_seconds)) {
    config_.detector_frame_interval_seconds = 1.0 / 60.0;
  }
  config_.detector_frame_interval_seconds =
      std::clamp(config_.detector_frame_interval_seconds, 0.0, 1.0);
  config_.minimum_spectral_observations =
      std::clamp<std::uint16_t>(config_.minimum_spectral_observations, 1, 50);
  config_.minimum_verification_symbols =
      std::clamp<std::uint16_t>(config_.minimum_verification_symbols, 0, 20);
  config_.minimum_key_transitions =
      std::clamp<std::uint16_t>(config_.minimum_key_transitions, 2, 100);
  config_.minimum_cadence_observations =
      std::clamp<std::uint16_t>(config_.minimum_cadence_observations, 1, 50);
  config_.minimum_verification_timing_quality =
      std::clamp(config_.minimum_verification_timing_quality, 0.0F, 1.0F);
  config_.minimum_verification_cadence_quality =
      std::clamp(config_.minimum_verification_cadence_quality, 0.0F, 1.0F);
  config_.minimum_character_confidence =
      std::clamp(config_.minimum_character_confidence, 0.0F, 1.0F);
  config_.minimum_narrowband_coherence =
      std::clamp(config_.minimum_narrowband_coherence, 0.0F, 1.0F);
  config_.maximum_verification_unknown_fraction =
      std::clamp(config_.maximum_verification_unknown_fraction, 0.0F, 1.0F);
  config_.minimum_plausibility_check_characters = std::clamp<std::uint16_t>(
      config_.minimum_plausibility_check_characters, 10, 500);
  config_.maximum_simple_character_fraction =
      std::clamp(config_.maximum_simple_character_fraction, 0.0F, 1.0F);
  config_.track_identity_tolerance_hz = std::clamp(
      config_.track_identity_tolerance_hz, 5.0, config_.tracking_tolerance_hz);
  config_.presentation_reanchor_limit_hz =
      std::clamp(config_.presentation_reanchor_limit_hz, 5.0,
                 std::max(5.0, config_.tracking_tolerance_hz - 5.0));
  config_.presentation_follow_deadband_hz =
      std::clamp(config_.presentation_follow_deadband_hz, 1.0,
                 config_.presentation_reanchor_limit_hz * 0.5);
  config_.presentation_follow_slew_hz_per_second =
      std::clamp(config_.presentation_follow_slew_hz_per_second, 0.25, 10.0);
  config_.presentation_follow_stable_seconds =
      std::clamp(config_.presentation_follow_stable_seconds, 0.5, 5.0);
  config_.presentation_follow_maximum_drift_hz_per_second = std::clamp(
      config_.presentation_follow_maximum_drift_hz_per_second, 0.5, 20.0);
  config_.presentation_follow_maximum_mad_hz =
      std::clamp(config_.presentation_follow_maximum_mad_hz, 1.0, 15.0);
  config_.track_replacement_margin_db =
      std::clamp(config_.track_replacement_margin_db, 0.0F, 20.0F);
  config_.verification_enter_seconds =
      std::clamp(config_.verification_enter_seconds, 0.0, 5.0);
  config_.verification_exit_seconds =
      std::clamp(config_.verification_exit_seconds,
                 config_.verification_enter_seconds, 15.0);
  config_.decoder_recovery_seconds =
      std::clamp(config_.decoder_recovery_seconds, 0.5, 15.0);
  if (!std::isfinite(config_.maximum_single_keyer_speed_ratio)) {
    config_.maximum_single_keyer_speed_ratio = 1.5F;
  }
  if (!std::isfinite(config_.minimum_single_keyer_cadence_fit)) {
    config_.minimum_single_keyer_cadence_fit = 0.50F;
  }
  // Below 1.25 the clean carrier the default was measured against (1.19) would
  // itself be silenced once noise moved it a little; above 4.0 nothing in that
  // capture, whose worst carrier read 3.50, would ever be refused. Outside
  // that range the setting no longer expresses a judgement, so it is not
  // offered. `resolve_overlapping_keyers` is the way to turn the test off.
  config_.maximum_single_keyer_speed_ratio =
      std::clamp(config_.maximum_single_keyer_speed_ratio, 1.25F, 4.0F);
  config_.minimum_single_keyer_cadence_fit =
      std::clamp(config_.minimum_single_keyer_cadence_fit, 0.30F, 0.70F);
}

void CwChannelBank::parkTracksOutsideBand(
    const std::uint64_t timestamp_ns) noexcept {
  const bool bounds_known =
      spectrum_range_initialized_ &&
      last_spectrum_upper_frequency_hz_ > last_spectrum_lower_frequency_hz_;
  if (!bounds_known) return;
  for (Track& track : tracks_) {
    const bool outside =
        track.frequency_hz <= 0.0 ||
        track.frequency_hz < last_spectrum_lower_frequency_hz_ ||
        track.frequency_hz > last_spectrum_upper_frequency_hz_;
    if (outside) {
      if (!track.parked) {
        track.parked = true;
        track.parked_since_ns = timestamp_ns;
      }
      continue;
    }
    if (!track.parked) continue;
    // Back inside the analysed band. Give it a fresh lease rather than leaving
    // it holding a detection timestamp from before the excursion, which would
    // expire it on the next sweep before it could re-acquire.
    track.parked = false;
    track.parked_since_ns = 0;
    track.last_detected_ns = timestamp_ns;
    track.last_candidate_match_ns = timestamp_ns;
  }
}

void CwChannelBank::noteInputDiscontinuity() noexcept {
  // Everything downstream of the antenna starts again: the filters hold state
  // from samples that no longer connect to the ones arriving, and the decoder
  // must not read the seam as a keying edge.
  for (Track& track : tracks_) {
    resetFilter(track);
    track.update = track.decoder.suspendInput(expected_sample_timestamp_ns_);
    track.decoder_input_suspended = true;
  }
  monitor_audio_.clear();
  monitor_oscillator_ = {1.0F, 0.0F};
  stream_initialized_ = false;
  sample_timing_initialized_ = false;
  expected_sample_timestamp_ns_ = 0;
  // The spectrum bounds are deliberately left alone. The next frame carries
  // the new window's bounds and the out-of-band rule there decides which
  // tracks are now outside it, which is the same decision made for a VFO move
  // and must not be made twice in two ways.
}

void CwChannelBank::reset() noexcept {
  tracks_.clear();
  snapshots_.clear();
  character_refinement_tracks_.clear();
  monitor_audio_.clear();
  monitor_oscillator_ = {1.0F, 0.0F};
  retained_observations_.clear();
  color_leases_ = {};
  next_track_id_ = 1;
  expected_sample_timestamp_ns_ = 0;
  last_spectrum_timestamp_ns_ = 0;
  last_spectrum_lower_frequency_hz_ = 0.0;
  last_spectrum_upper_frequency_hz_ = 0.0;
  spectrum_range_initialized_ = false;
  stream_initialized_ = false;
  sample_timing_initialized_ = false;
  verified_transitions_ = 0;
  expired_unverified_tracks_ = 0;
  decoder_reacquisitions_ = 0;
}

void CwChannelBank::setMonitor(const CwMonitorMode mode,
                               const std::uint64_t track_id,
                               const double reference_tone_hz) noexcept {
  const std::array ids{track_id};
  setMonitorTracks(mode, ids, reference_tone_hz);
}

void CwChannelBank::setMonitorTracks(
    const CwMonitorMode mode, const std::span<const std::uint64_t> track_ids,
    const double reference_tone_hz) noexcept {
  const double sanitized_tone =
      std::isfinite(reference_tone_hz)
          ? std::clamp(reference_tone_hz, 200.0, 1'500.0)
          : 700.0;
  std::array<std::uint64_t, kCwMaximumTrackCapacity> sanitized_tracks{};
  std::size_t sanitized_count = 0;
  if (mode == CwMonitorMode::SelectedTrack) {
    for (const std::uint64_t track_id : track_ids) {
      if (track_id == 0U || sanitized_count >= sanitized_tracks.size() ||
          std::find(sanitized_tracks.cbegin(),
                    sanitized_tracks.cbegin() +
                        static_cast<std::ptrdiff_t>(sanitized_count),
                    track_id) !=
              sanitized_tracks.cbegin() +
                  static_cast<std::ptrdiff_t>(sanitized_count)) {
        continue;
      }
      sanitized_tracks[sanitized_count++] = track_id;
    }
  }
  const bool tracks_changed =
      sanitized_count != monitored_track_count_ ||
      !std::equal(sanitized_tracks.cbegin(),
                  sanitized_tracks.cbegin() +
                      static_cast<std::ptrdiff_t>(sanitized_count),
                  monitored_track_ids_.cbegin());
  if (monitor_mode_ != mode || tracks_changed ||
      monitor_reference_tone_hz_ != sanitized_tone) {
    monitor_oscillator_ = {1.0F, 0.0F};
  }
  monitor_mode_ = mode;
  monitored_track_ids_ = sanitized_tracks;
  monitored_track_count_ = sanitized_count;
  monitor_reference_tone_hz_ = sanitized_tone;
  monitor_audio_.clear();
}

const std::vector<CwChannelSnapshot>& CwChannelBank::updateSpectrum(
    const std::uint64_t timestamp_ns, const double lower_frequency_hz,
    const double upper_frequency_hz, const std::span<const float> raw_bins_dbfs,
    const bool rebuild_snapshot) {
  if (raw_bins_dbfs.size() < 3 || !std::isfinite(lower_frequency_hz) ||
      !std::isfinite(upper_frequency_hz) ||
      upper_frequency_hz <= lower_frequency_hz) {
    return snapshots_;
  }

  // Detection averages the supplied spectrum here, over a fixed time constant
  // measured in seconds rather than in frames. Display-side averaging and the
  // configured display frame rate therefore cannot change which candidates are
  // discovered, how prominent they appear, or how quickly they persist.
  const bool bounds_moved =
      spectrum_range_initialized_ &&
      (lower_frequency_hz != last_spectrum_lower_frequency_hz_ ||
       upper_frequency_hz != last_spectrum_upper_frequency_hz_);
  last_spectrum_lower_frequency_hz_ = lower_frequency_hz;
  last_spectrum_upper_frequency_hz_ = upper_frequency_hz;
  last_spectrum_timestamp_ns_ = timestamp_ns;
  spectrum_range_initialized_ = true;

  // The analysed band moved under the tracks. On direct IQ this is what a
  // receiver retune looks like: the decoder window is re-centred on the new
  // capture, so the slice being decoded changes while the stations in it keep
  // their absolute frequencies. A track the new window no longer covers is
  // parked rather than left to expire, on exactly the rule a VFO move uses,
  // because in both cases the operator moved the receiver and the station's
  // position is still known.
  if (bounds_moved) parkTracksOutsideBand(timestamp_ns);

  // Detection runs on its own fixed cadence and consumes exactly one spectrum
  // frame per tick. Frames supplied faster than that are ignored entirely
  // rather than folded in, so a display line rate that is a multiple of the
  // detector rate reproduces the detector rate's result exactly.
  const double detector_interval_seconds =
      detector_pass_initialized_ && timestamp_ns > last_detector_pass_ns_
          ? static_cast<double>(timestamp_ns - last_detector_pass_ns_) /
                1'000'000'000.0
          : 0.0;
  if (detector_pass_initialized_ &&
      config_.detector_frame_interval_seconds > 0.0 &&
      detector_interval_seconds <
          config_.detector_frame_interval_seconds * 0.999) {
    return snapshots_;
  }
  last_detector_pass_ns_ = timestamp_ns;
  detector_pass_initialized_ = true;

  if (detector_bins_power_.size() != raw_bins_dbfs.size()) {
    detector_bins_power_.assign(raw_bins_dbfs.size(), 0.0F);
    detector_bins_dbfs_.assign(raw_bins_dbfs.size(), -200.0F);
    detector_average_initialized_ = false;
  }
  const float averaging_alpha =
      !detector_average_initialized_ ||
              config_.detector_averaging_seconds <= 0.0
          ? 1.0F
          : static_cast<float>(
                1.0 - std::exp(-std::max(detector_interval_seconds, 0.001) /
                               config_.detector_averaging_seconds));
  for (std::size_t bin = 0; bin < raw_bins_dbfs.size(); ++bin) {
    const float level = raw_bins_dbfs[bin];
    const float power =
        std::isfinite(level) ? std::pow(10.0F, 0.1F * level) : 0.0F;
    detector_bins_power_[bin] +=
        averaging_alpha * (power - detector_bins_power_[bin]);
    detector_bins_dbfs_[bin] =
        detector_bins_power_[bin] > 0.0F
            ? 10.0F * std::log10(detector_bins_power_[bin])
            : -200.0F;
  }
  detector_average_initialized_ = true;
  const std::span<const float> bins_dbfs{detector_bins_dbfs_};

  // Spectral persistence accrues in wall-clock time and is then expressed in
  // 60 Hz-equivalent observations, so a track needs the same real evidence at
  // any display frame rate. The quantum and decay interval reproduce the
  // previous per-frame behaviour exactly at the default 60 frames per second.
  constexpr double kObservationQuantumMs = 1'000.0 / 60.0;
  constexpr double kPersistenceDecayIntervalMs = 500.0;
  const double frame_interval_ms = detector_interval_seconds > 0.0
                                       ? detector_interval_seconds * 1'000.0
                                       : kObservationQuantumMs;
  // One spectrum frame is at most one independent observation however long the
  // preceding gap was, so a single frame after silence cannot manufacture
  // persistence. Raising the frame rate above the 60 Hz reference therefore no
  // longer accelerates qualification; it only subdivides it.
  const double matched_credit_ms =
      std::min(frame_interval_ms, kObservationQuantumMs);
  const double unmatched_credit_ms =
      std::min(frame_interval_ms, kPersistenceDecayIntervalMs);
  const auto creditMatchedEvidence = [&](Track& track) {
    track.unmatched_evidence_credit_ms = 0.0;
    track.matched_evidence_credit_ms += matched_credit_ms;
    if (track.matched_evidence_credit_ms >= kObservationQuantumMs) {
      track.matched_evidence_credit_ms -= kObservationQuantumMs;
      if (track.spectral_observations <
          std::numeric_limits<std::uint16_t>::max()) {
        ++track.spectral_observations;
      }
    }
  };
  const auto creditUnmatchedEvidence = [&](Track& track) {
    track.matched_evidence_credit_ms = 0.0;
    track.unmatched_evidence_credit_ms += unmatched_credit_ms;
    if (track.unmatched_evidence_credit_ms >= kPersistenceDecayIntervalMs) {
      track.unmatched_evidence_credit_ms -= kPersistenceDecayIntervalMs;
      if (track.spectral_observations > 0) --track.spectral_observations;
    }
  };

  const double bin_width_hz = (upper_frequency_hz - lower_frequency_hz) /
                              static_cast<double>(bins_dbfs.size() - 1);
  const float measured_noise_dbfs = estimateNoise(bins_dbfs);
  const float strongest_dbfs =
      *std::max_element(bins_dbfs.begin(), bins_dbfs.end());
  const float noise_dbfs = std::max(
      measured_noise_dbfs, strongest_dbfs - config_.detection_dynamic_range_db);
  std::vector<Candidate> candidates;
  candidates.reserve(std::min<std::size_t>(bins_dbfs.size(), 64));
  for (std::size_t bin = 1; bin + 1 < bins_dbfs.size(); ++bin) {
    const float level = bins_dbfs[bin];
    if (!std::isfinite(level) || level < bins_dbfs[bin - 1] ||
        level < bins_dbfs[bin + 1]) {
      continue;
    }
    const float left = bins_dbfs[bin - 1];
    const float right = bins_dbfs[bin + 1];
    const float denominator = left - 2.0F * level + right;
    const double fractional_bin =
        std::abs(denominator) > 1.0e-6F
            ? std::clamp(0.5 * static_cast<double>(left - right) /
                             static_cast<double>(denominator),
                         -0.5, 0.5)
            : 0.0;
    const float interpolated_level =
        level - static_cast<float>(0.25 * static_cast<double>(left - right) *
                                   fractional_bin);
    float near_left_sum = 0.0F;
    float near_right_sum = 0.0F;
    std::size_t near_count = 0;
    for (std::size_t offset = 2; offset <= 4; ++offset) {
      if (bin < offset || bin + offset >= bins_dbfs.size()) continue;
      near_left_sum += bins_dbfs[bin - offset];
      near_right_sum += bins_dbfs[bin + offset];
      ++near_count;
    }
    if (near_count == 0) continue;
    const float near_reference =
        std::max(near_left_sum / static_cast<float>(near_count),
                 near_right_sum / static_cast<float>(near_count));
    if (interpolated_level - near_reference <
        config_.minimum_near_peak_prominence_db) {
      continue;
    }
    const auto reference_offset_bins = static_cast<std::ptrdiff_t>(std::max(
        2.0,
        std::round(config_.prominence_reference_offset_hz / bin_width_hz)));
    const auto reference_half_width_bins = static_cast<std::ptrdiff_t>(
        std::max(1.0, std::round(0.5 * config_.prominence_reference_width_hz /
                                 bin_width_hz)));
    const auto reference_average = [&](const std::ptrdiff_t center) {
      float sum = 0.0F;
      std::size_t count = 0;
      for (std::ptrdiff_t offset = -reference_half_width_bins;
           offset <= reference_half_width_bins; ++offset) {
        const auto index = center + offset;
        if (index < 0 || index >= static_cast<std::ptrdiff_t>(bins_dbfs.size()))
          continue;
        const float value = bins_dbfs[static_cast<std::size_t>(index)];
        if (!std::isfinite(value)) continue;
        sum += value;
        ++count;
      }
      return std::pair{sum, count};
    };
    const auto [left_sum, left_count] = reference_average(
        static_cast<std::ptrdiff_t>(bin) - reference_offset_bins);
    const auto [right_sum, right_count] = reference_average(
        static_cast<std::ptrdiff_t>(bin) + reference_offset_bins);
    if (left_count == 0 && right_count == 0) continue;
    float local_reference = -std::numeric_limits<float>::infinity();
    if (left_count > 0)
      local_reference =
          std::max(local_reference, left_sum / static_cast<float>(left_count));
    if (right_count > 0)
      local_reference = std::max(local_reference,
                                 right_sum / static_cast<float>(right_count));
    if (interpolated_level - local_reference <
        config_.minimum_peak_prominence_db) {
      continue;
    }
    const float snr_db = interpolated_level - noise_dbfs;
    if (snr_db < config_.acquisition_snr_db) continue;
    candidates.push_back({
        .frequency_hz =
            lower_frequency_hz +
            (static_cast<double>(bin) + fractional_bin) * bin_width_hz,
        .snr_db = snr_db,
        .preferred_track_id = 0,
    });
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
              return left.snr_db > right.snr_db;
            });

  std::vector<Candidate> separated;
  separated.reserve(candidates.size());
  std::vector<bool> reserved(candidates.size(), false);
  // Preserve the ridge nearest each established decoder before choosing the
  // globally strongest separated peaks. Without this pass, a stronger skirt
  // peak inside minimum_separation_hz can suppress the real carrier, then
  // walk a verified track away through a sequence of individually small
  // innovations while a duplicate track is created on the actual signal.
  std::vector<const Track*> established_tracks;
  established_tracks.reserve(tracks_.size());
  for (const auto& track : tracks_) {
    if (track.operator_selected || track.ever_verified ||
        track.verification_state == CwTrackState::MorseLikely ||
        track.verification_state == CwTrackState::Verified) {
      established_tracks.push_back(&track);
    }
  }
  std::stable_sort(
      established_tracks.begin(), established_tracks.end(),
      [](const Track* left, const Track* right) {
        const auto priority = [](const Track& track) {
          if (track.operator_selected) return 4;
          if (track.verification_state == CwTrackState::Verified) return 3;
          if (track.ever_verified) return 2;
          if (track.verification_state == CwTrackState::MorseLikely) return 1;
          return 0;
        };
        const int left_priority = priority(*left);
        const int right_priority = priority(*right);
        if (left_priority != right_priority)
          return left_priority > right_priority;
        if (left->spectral_observations != right->spectral_observations)
          return left->spectral_observations > right->spectral_observations;
        if (left->spectral_snr_db != right->spectral_snr_db)
          return left->spectral_snr_db > right->spectral_snr_db;
        return left->id < right->id;
      });
  for (const Track* track_pointer : established_tracks) {
    const auto& track = *track_pointer;
    const double elapsed_seconds =
        timestamp_ns > track.last_frequency_update_ns
            ? static_cast<double>(timestamp_ns -
                                  track.last_frequency_update_ns) /
                  1'000'000'000.0
            : 0.0;
    const double predicted_frequency =
        track.frequency_hz +
        track.drift_hz_per_second * std::min(elapsed_seconds, 0.25);
    std::size_t nearest_index = candidates.size();
    double best_reservation_score = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (reserved[index]) continue;
      const auto& candidate = candidates[index];
      const double predicted_distance =
          std::abs(candidate.frequency_hz - predicted_frequency);
      const double presentation_distance =
          std::abs(candidate.frequency_hz - track.presentation_frequency_hz);
      if (track.spectral_observations >=
              config_.minimum_spectral_observations &&
          (std::min(predicted_distance, presentation_distance) >
               config_.track_identity_tolerance_hz ||
           std::abs(candidate.frequency_hz -
                    track.identity_origin_frequency_hz) >
               config_.tracking_tolerance_hz)) {
        continue;
      }
      // Prediction keeps real drifting carriers eligible, while the stable
      // presentation center breaks ties against a noise ridge that has begun
      // walking the internal DSP center. If the true ridge returns alongside
      // that noise, it therefore reacquires the established identity instead
      // of creating a duplicate at the original frequency.
      const double reservation_score =
          predicted_distance + 1.5 * presentation_distance;
      if (reservation_score <= best_reservation_score) {
        best_reservation_score = reservation_score;
        nearest_index = index;
      }
    }
    if (nearest_index == candidates.size()) continue;
    Candidate protected_candidate = candidates[nearest_index];
    const bool duplicates_reserved_identity =
        !track.operator_selected &&
        std::any_of(separated.cbegin(), separated.cend(),
                    [&](const Candidate& selected) {
                      return std::abs(protected_candidate.frequency_hz -
                                      selected.frequency_hz) <
                             config_.minimum_separation_hz;
                    });
    if (duplicates_reserved_identity) continue;
    protected_candidate.preferred_track_id = track.id;
    separated.push_back(protected_candidate);
    reserved[nearest_index] = true;
  }
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (reserved[index]) continue;
    const auto& candidate = candidates[index];
    const bool overlaps = std::any_of(
        separated.cbegin(), separated.cend(), [&](const Candidate& selected) {
          return std::abs(candidate.frequency_hz - selected.frequency_hz) <
                 config_.minimum_separation_hz;
        });
    if (!overlaps) separated.push_back(candidate);
  }

  for (auto& track : tracks_) track.matched = false;
  for (const auto& candidate : separated) {
    auto nearest = tracks_.end();
    double nearest_distance = config_.tracking_tolerance_hz;
    for (auto track = tracks_.begin(); track != tracks_.end(); ++track) {
      if (track->matched) continue;
      if (candidate.preferred_track_id != 0U &&
          candidate.preferred_track_id != track->id) {
        continue;
      }
      const double elapsed_seconds =
          timestamp_ns > track->last_frequency_update_ns
              ? static_cast<double>(timestamp_ns -
                                    track->last_frequency_update_ns) /
                    1'000'000'000.0
              : 0.0;
      // Drift is a short-term predictor, not permission to coast through a
      // word/message gap. Unbounded extrapolation let a modest noisy estimate
      // move the match tens or hundreds of hertz while the key was up.
      const double prediction_seconds = std::min(elapsed_seconds, 0.25);
      const double predicted_frequency =
          track->frequency_hz + track->drift_hz_per_second * prediction_seconds;
      const double predicted_distance =
          std::abs(predicted_frequency - candidate.frequency_hz);
      const double distance =
          candidate.preferred_track_id == track->id
              ? std::min(predicted_distance,
                         std::abs(track->presentation_frequency_hz -
                                  candidate.frequency_hz))
              : predicted_distance;
      // Once a track has accumulated enough evidence, a large innovation is
      // a different signal, not ordinary drift. Refusing that association is
      // what prevents an old decoder/text history from walking across nearby
      // peaks and being attached to a new station.
      if (track->spectral_observations >=
              config_.minimum_spectral_observations &&
          (distance > config_.track_identity_tolerance_hz ||
           std::abs(candidate.frequency_hz -
                    track->identity_origin_frequency_hz) >
               config_.tracking_tolerance_hz)) {
        continue;
      }
      if (distance <= nearest_distance) {
        nearest = track;
        nearest_distance = distance;
      }
    }
    if (nearest == tracks_.end()) {
      // A retained automatic identity owns its configured separation cell
      // across spectrum frames. Reuse an unmatched occupant (including a
      // short-lived acquisition candidate) or suppress the second ridge when
      // that occupant already consumed a stronger candidate this frame. This
      // prevents one keyed carrier's changing FFT sidelobes from spawning a
      // bank full of decoders without preventing explicit close manual probes.
      auto cell_occupant = tracks_.end();
      double cell_distance = config_.minimum_separation_hz;
      bool occupied_by_matched_track = false;
      for (auto track = tracks_.begin(); track != tracks_.end(); ++track) {
        if (track->operator_selected ||
            track->verification_state == CwTrackState::Lost) {
          continue;
        }
        const double distance =
            std::min(std::abs(candidate.frequency_hz -
                              track->identity_origin_frequency_hz),
                     std::abs(candidate.frequency_hz -
                              track->presentation_frequency_hz));
        if (distance >= config_.minimum_separation_hz) continue;
        if (track->matched) {
          occupied_by_matched_track = true;
          continue;
        }
        if (distance <= cell_distance) {
          cell_distance = distance;
          cell_occupant = track;
        }
      }
      if (cell_occupant != tracks_.end()) {
        nearest = cell_occupant;
      } else if (occupied_by_matched_track) {
        continue;
      }
    }
    if (nearest == tracks_.end()) {
      if (tracks_.size() >= config_.maximum_tracks) {
        auto victim = tracks_.end();
        float victim_score = std::numeric_limits<float>::infinity();
        for (auto track = tracks_.begin(); track != tracks_.end(); ++track) {
          if (track->matched ||
              track->verification_state == CwTrackState::Verified) {
            continue;
          }
          const float score =
              track->spectral_snr_db +
              0.10F * static_cast<float>(std::min<std::uint16_t>(
                          track->spectral_observations, 20)) +
              (track->verification_state == CwTrackState::MorseLikely ? 8.0F
                                                                      : 0.0F);
          if (score < victim_score) {
            victim_score = score;
            victim = track;
          }
        }
        if (victim == tracks_.end()) continue;
        const double unmatched_seconds =
            timestamp_ns > victim->last_candidate_match_ns
                ? static_cast<double>(timestamp_ns -
                                      victim->last_candidate_match_ns) /
                      1'000'000'000.0
                : 0.0;
        const bool replace =
            candidate.snr_db >=
                victim->spectral_snr_db + config_.track_replacement_margin_db ||
            unmatched_seconds >= 0.25 ||
            victim->spectral_observations <
                config_.minimum_spectral_observations;
        if (!replace) continue;
        ++expired_unverified_tracks_;
        tracks_.erase(victim);
      }
      tracks_.emplace_back(next_track_id_++, candidate.frequency_hz,
                           timestamp_ns);
      nearest = std::prev(tracks_.end());
      // A track is created with the bank's default decoder, so a newly
      // acquired signal has to be told which model is actually selected.
      nearest->decoder.setKeyingModel(keying_model_);
    }
    nearest->matched = true;
    if (nearest->decoder_input_suspended)
      nearest->update = nearest->decoder.resumeInput(timestamp_ns);
    nearest->decoder_input_suspended = false;
    creditMatchedEvidence(*nearest);
    const double elapsed_seconds =
        timestamp_ns > nearest->last_frequency_update_ns
            ? static_cast<double>(timestamp_ns -
                                  nearest->last_frequency_update_ns) /
                  1'000'000'000.0
            : 0.0;
    if (elapsed_seconds > 0.0 && elapsed_seconds <= 1.0) {
      const double predicted_frequency =
          nearest->frequency_hz +
          nearest->drift_hz_per_second * elapsed_seconds;
      const double innovation = candidate.frequency_hz - predicted_frequency;
      nearest->frequency_hz = predicted_frequency + 0.45 * innovation;
      nearest->drift_hz_per_second = std::clamp(
          nearest->drift_hz_per_second + 0.08 * innovation / elapsed_seconds,
          -200.0, 200.0);
    } else {
      nearest->frequency_hz +=
          0.45 * (candidate.frequency_hz - nearest->frequency_hz);
      nearest->drift_hz_per_second = 0.0;
    }
    nearest->last_frequency_update_ns = timestamp_ns;
    nearest->last_detected_ns = timestamp_ns;
    nearest->last_candidate_match_ns = timestamp_ns;
    nearest->consecutive_spectrum_misses = 0;
    observePresentationFrequency(*nearest, candidate.frequency_hz,
                                 timestamp_ns);
    if (nearest->verification_state == CwTrackState::Verified ||
        (nearest->operator_selected && !nearest->ever_verified)) {
      followVerifiedPresentation(*nearest, timestamp_ns);
    }
    if (nearest->color_assigned && nearest->ever_verified) {
      assignOrRefreshColor(*nearest, timestamp_ns);
    }
  }

  // An operator-selected slice gets a bounded lower-floor association at the
  // pointed center even when the global peak detector does not admit it. This
  // is measured evidence (center-bin power above the current spectrum noise),
  // not synthetic persistence: every ordinary keying, cadence, coherence,
  // timing, character, and sustained-entry gate still applies unchanged.
  for (auto& track : tracks_) {
    if (!track.operator_selected || track.matched) continue;
    const float selected_snr = spectralSnr(track, lower_frequency_hz,
                                           bin_width_hz, bins_dbfs, noise_dbfs);
    if (selected_snr < config_.retention_snr_db) continue;
    track.matched = true;
    if (track.decoder_input_suspended)
      track.update = track.decoder.resumeInput(timestamp_ns);
    track.decoder_input_suspended = false;
    creditMatchedEvidence(track);
    track.last_detected_ns = timestamp_ns;
    track.last_candidate_match_ns = timestamp_ns;
    track.consecutive_spectrum_misses = 0;
  }

  for (auto& track : tracks_) {
    track.spectral_snr_db = spectralSnr(track, lower_frequency_hz, bin_width_hz,
                                        bins_dbfs, noise_dbfs);
    if (!track.matched) {
      if (track.consecutive_spectrum_misses <
          std::numeric_limits<std::uint16_t>::max()) {
        ++track.consecutive_spectrum_misses;
      }
      // Persistence is recent evidence, not a lifetime counter. A slow decay
      // tolerates keyed gaps while ensuring abandoned candidates eventually
      // lose their admission advantage. The decay is driven by elapsed time so
      // it matches the accrual rule above at any spectrum frame rate.
      creditUnmatchedEvidence(track);
      track.drift_hz_per_second *= 0.92;
      if (std::abs(track.drift_hz_per_second) < 0.1)
        track.drift_hz_per_second = 0.0;
    }
    if (track.matched &&
        (track.verification_state == CwTrackState::Verified ||
         track.operator_selected) &&
        track.spectral_snr_db >= config_.retention_snr_db) {
      track.last_detected_ns = timestamp_ns;
    }
  }

  const auto expired = [&](const Track& track) {
    // A parked track is waiting for the dial, not fading. It is held on its
    // own generous clock so that tuning away from a station and back does not
    // cost its identity, and bounded so sweeping the band cannot accumulate
    // parked tracks without limit.
    if (track.parked) {
      if (timestamp_ns < track.parked_since_ns) return false;
      const double parked_seconds =
          static_cast<double>(timestamp_ns - track.parked_since_ns) /
          1'000'000'000.0;
      return parked_seconds > config_.parked_track_retention_seconds;
    }
    const bool unverified_manual =
        track.operator_selected && !track.ever_verified;
    const std::uint64_t last_activity_ns =
        unverified_manual ? track.operator_selected_ns : track.last_detected_ns;
    if (timestamp_ns < last_activity_ns) return false;
    const double age_seconds =
        static_cast<double>(timestamp_ns - last_activity_ns) / 1'000'000'000.0;
    const double retention =
        unverified_manual ? config_.decoded_track_retention_seconds
        : track.verification_state != CwTrackState::Verified
            ? (!track.update.text.empty() ||
                       !track.update.provisional_text.empty() ||
                       track.verification_state == CwTrackState::MorseLikely
                   ? std::max(config_.unverified_track_retention_seconds,
                              config_.empty_track_retention_seconds)
                   : config_.unverified_track_retention_seconds)
            : config_.decoded_track_retention_seconds;
    return age_seconds > retention;
  };
  for (auto& track : tracks_) {
    if (!expired(track)) continue;
    if (track.verification_state != CwTrackState::Verified)
      ++expired_unverified_tracks_;
    track.verification_state = CwTrackState::Lost;
    track.verification_reason = CwVerificationReason::SignalLost;
  }
  std::erase_if(tracks_, [](const Track& track) {
    return track.verification_state == CwTrackState::Lost;
  });
  if (rebuild_snapshot) rebuildSnapshots(timestamp_ns);
  return snapshots_;
}

std::uint64_t CwChannelBank::selectFrequency(
    const double frequency_hz) noexcept {
  if (!spectrum_range_initialized_ || !std::isfinite(frequency_hz) ||
      frequency_hz < last_spectrum_lower_frequency_hz_ ||
      frequency_hz > last_spectrum_upper_frequency_hz_) {
    return 0;
  }

  auto selected = tracks_.end();
  // Manual selection must be able to split close pileup callers that the
  // automatic peak de-duplication intentionally treats as one broad region.
  // Reuse only a click on effectively the same audio lane.
  double nearest_distance = kManualSelectionReuseToleranceHz;
  for (auto track = tracks_.begin(); track != tracks_.end(); ++track) {
    const double distance = std::abs(track->frequency_hz - frequency_hz);
    if (distance <= nearest_distance) {
      nearest_distance = distance;
      selected = track;
    }
  }

  if (selected == tracks_.end()) {
    if (tracks_.size() >= config_.maximum_tracks) {
      auto victim = tracks_.end();
      float victim_score = std::numeric_limits<float>::infinity();
      for (auto track = tracks_.begin(); track != tracks_.end(); ++track) {
        if (track->verification_state == CwTrackState::Verified ||
            track->operator_selected) {
          continue;
        }
        const float score =
            track->spectral_snr_db +
            0.10F * static_cast<float>(std::min<std::uint16_t>(
                        track->spectral_observations, 20)) +
            (track->verification_state == CwTrackState::MorseLikely ? 8.0F
                                                                    : 0.0F);
        if (score < victim_score) {
          victim_score = score;
          victim = track;
        }
      }
      if (victim == tracks_.end()) return 0;
      ++expired_unverified_tracks_;
      tracks_.erase(victim);
    }
    tracks_.emplace_back(next_track_id_++, frequency_hz,
                         last_spectrum_timestamp_ns_);
    selected = std::prev(tracks_.end());
    selected->decoder.setKeyingModel(keying_model_);
    selected->decoder_input_suspended = true;
  }

  selected->operator_selected = true;
  selected->operator_selected_ns = last_spectrum_timestamp_ns_;
  if (!selected->ever_verified) {
    selected->frequency_hz = frequency_hz;
    selected->identity_origin_frequency_hz = frequency_hz;
    selected->presentation_frequency_hz = frequency_hz;
    selected->drift_hz_per_second = 0.0;
    selected->last_frequency_update_ns = last_spectrum_timestamp_ns_;
    selected->presentation_frequency_evidence_count = 0;
    selected->presentation_frequency_evidence_index = 0;
    selected->presentation_follow_stable_since_ns = 0;
    selected->last_presentation_evidence_ns = 0;
    resetFilter(*selected);
  }
  // Selection is only an instruction to analyze this slice. It deliberately
  // does not add persistence, keying, cadence, or text evidence. Subsequent
  // measured carrier associations may move its DSP and presentation centers
  // through the same bounded tracker used by automatically detected streams.
  rebuildSnapshots(last_spectrum_timestamp_ns_);
  return selected->id;
}

const std::vector<CwChannelSnapshot>& CwChannelBank::processSamples(
    const RealtimeSampleBlock& block) {
  monitor_audio_.clear();
  if (block.sample_count == 0 || block.sample_count > block.samples.size() ||
      !std::isfinite(block.stream.sample_rate_hz) ||
      block.stream.sample_rate_hz <= 0.0) {
    return snapshots_;
  }

  const bool same_stream =
      stream_initialized_ && stream_.kind == block.stream.kind &&
      stream_.sample_rate_hz == block.stream.sample_rate_hz &&
      stream_.center_frequency_hz == block.stream.center_frequency_hz;
  if (!same_stream) {
    stream_ = block.stream;
    stream_initialized_ = true;
    sample_timing_initialized_ = false;
    for (auto& track : tracks_) resetFilter(track);
  }

  if (sample_timing_initialized_) {
    const std::uint64_t difference =
        block.timestamp_ns > expected_sample_timestamp_ns_
            ? block.timestamp_ns - expected_sample_timestamp_ns_
            : expected_sample_timestamp_ns_ - block.timestamp_ns;
    const auto tolerance_ns = static_cast<std::uint64_t>(
        std::ceil(2.0 * 1'000'000'000.0 / block.stream.sample_rate_hz));
    if (difference > tolerance_ns) {
      for (auto& track : tracks_) {
        track.decoder.reset();
        track.update = {};
        track.decoder_rejection_samples = 0;
        resetFilter(track);
      }
    }
  }

  const double sample_rate_hz = block.stream.sample_rate_hz;
  if (monitor_mode_ == CwMonitorMode::FullReceiver) {
    monitor_audio_.reserve(block.sample_count);
    for (std::size_t index = 0; index < block.sample_count; ++index)
      monitor_audio_.push_back(block.samples[index].real());
  }
  const std::size_t evidence_samples = static_cast<std::size_t>(
      std::max(1.0, std::round(sample_rate_hz / config_.evidence_rate_hz)));
  std::array<float, kNarrowbandWidthsHz.size()> filter_alphas{};
  for (std::size_t width = 0; width < kNarrowbandWidthsHz.size(); ++width) {
    const double cutoff_hz =
        std::min(kNarrowbandWidthsHz[width] * 0.5, sample_rate_hz * 0.2);
    filter_alphas[width] = static_cast<float>(
        1.0 - std::exp(-2.0 * std::numbers::pi * cutoff_hz / sample_rate_hz));
  }
  const float reference_alpha = filter_alphas[1];
  const double monitor_angle =
      2.0 * std::numbers::pi * monitor_reference_tone_hz_ / sample_rate_hz;
  const std::complex<float> monitor_step{
      static_cast<float>(std::cos(monitor_angle)),
      static_cast<float>(std::sin(monitor_angle))};

  const auto is_monitored = [this](const std::uint64_t track_id) {
    return monitor_mode_ == CwMonitorMode::SelectedTrack &&
           std::find(monitored_track_ids_.cbegin(),
                     monitored_track_ids_.cbegin() +
                         static_cast<std::ptrdiff_t>(monitored_track_count_),
                     track_id) !=
               monitored_track_ids_.cbegin() +
                   static_cast<std::ptrdiff_t>(monitored_track_count_);
  };
  const std::size_t active_monitor_count = static_cast<std::size_t>(
      std::count_if(tracks_.cbegin(), tracks_.cend(),
                    [&is_monitored](const Track& track) {
                      return is_monitored(track.id);
                    }));
  if (active_monitor_count > 0U) {
    monitor_audio_.assign(block.sample_count, 0.0F);
  }
  const float monitor_mix_gain =
      active_monitor_count > 0U
          ? 1.0F / std::sqrt(static_cast<float>(active_monitor_count))
          : 0.0F;
  const float monitor_attack = static_cast<float>(
      1.0 - std::exp(-1.0 / std::max(1.0, 0.005 * sample_rate_hz)));
  const float monitor_release = static_cast<float>(
      1.0 - std::exp(-1.0 / std::max(1.0, 1.5 * sample_rate_hz)));
  std::complex<float> advanced_monitor_oscillator = monitor_oscillator_;

  measureTrackNeighbourhoods();

  for (auto& track : tracks_) {
    const bool monitored_track = is_monitored(track.id);
    // A track that will not be decoded needs no baseband at all. Everything
    // below -- three oscillators and five three-stage complex cascades for
    // every sample of every track -- exists to produce keying evidence, and
    // for a track nothing is reading that evidence is pure cost. Measured, the
    // per-track chain is what sets the pipeline's ceiling: it grows in
    // proportion to tracked signals at roughly nine tenths of a second per
    // track per twenty seconds of audio, while building the spectrum costs the
    // same for one signal as for twenty-four.
    //
    // Which level decides matters. The spectrum's estimate is free but is not
    // on the same scale as the level the keying envelope measures, and judging
    // one against the other's threshold skipped tracks that decode perfectly
    // well -- measured, that cost two and a half points of character error
    // across the synthetic surface. So the track's own measured level decides,
    // and the circularity is broken by giving every track a fixed warm-up
    // during which it is always filtered. Only a track that has had that long
    // to show its level and is still below the threshold is skipped.
    //
    // A monitored track is always filtered, because the operator is listening
    // to it, and so is one the operator selected.
    constexpr std::uint32_t kDecodeLevelWarmupBlocks = 150;
    if (track.decode_level_observations < kDecodeLevelWarmupBlocks) {
      ++track.decode_level_observations;
    } else if (!monitored_track && !track.operator_selected &&
               !config_.decode_weak_signals &&
               track.peak_decode_level_db < config_.minimum_decode_snr_db) {
      track.center_power_sums = {};
      track.lower_power_sum = 0.0F;
      track.upper_power_sum = 0.0F;
      continue;
    }
    std::complex<float> track_monitor_oscillator = monitor_oscillator_;
    const double center_hz =
        block.stream.kind == StreamKind::Audio
            ? track.frequency_hz
            : track.frequency_hz - block.stream.center_frequency_hz;
    const double reference_offset = config_.noise_reference_offset_hz;
    const auto oscillator_step = [sample_rate_hz](const double frequency_hz) {
      const double angle =
          -2.0 * std::numbers::pi * frequency_hz / sample_rate_hz;
      return std::complex<float>(static_cast<float>(std::cos(angle)),
                                 static_cast<float>(std::sin(angle)));
    };
    const auto center_step = oscillator_step(center_hz);
    const auto lower_step = oscillator_step(center_hz - reference_offset);
    const auto upper_step = oscillator_step(center_hz + reference_offset);
    if (!track.filter_initialized) {
      resetFilter(track);
      track.filter_initialized = true;
    }

    const auto filtered = [](const std::complex<float> input, const float alpha,
                             std::array<std::complex<float>, 3>& stages) {
      stages[0] += alpha * (input - stages[0]);
      stages[1] += alpha * (stages[0] - stages[1]);
      stages[2] += alpha * (stages[1] - stages[2]);
      return stages[2];
    };
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      const auto sample =
          block.stream.kind == StreamKind::Audio
              ? std::complex<float>(block.samples[index].real(), 0.0F)
              : block.samples[index];
      const auto center_mixed = sample * track.center_oscillator;
      for (std::size_t width = 0; width < kNarrowbandWidthsHz.size(); ++width) {
        const auto center = filtered(center_mixed, filter_alphas[width],
                                     track.center_filters[width]);
        track.center_power_sums[width] += std::norm(center);
        if (monitored_track && width == track.selected_width_index) {
          const float amplitude = std::abs(center);
          const float envelope_rate = amplitude > track.monitor_peak_envelope
                                          ? monitor_attack
                                          : monitor_release;
          track.monitor_peak_envelope +=
              envelope_rate * (amplitude - track.monitor_peak_envelope);
          // SDR sample levels vary by device, gain mode and driver. Normalize
          // only the selected narrowband monitor after filtering, with a fast
          // attack and slow release so gaps remain quiet instead of pumping.
          const float monitor_gain =
              std::clamp(0.28F / std::max(track.monitor_peak_envelope, 0.001F),
                         0.5F, 200.0F);
          monitor_audio_[index] += 2.0F * monitor_mix_gain * monitor_gain *
                                   (center * track_monitor_oscillator).real();
        }
      }
      const auto lower = filtered(sample * track.lower_oscillator,
                                  reference_alpha, track.lower_filter);
      const auto upper = filtered(sample * track.upper_oscillator,
                                  reference_alpha, track.upper_filter);
      track.lower_power_sum += std::norm(lower);
      track.upper_power_sum += std::norm(upper);
      ++track.accumulated_samples;

      track.center_oscillator *= center_step;
      track.lower_oscillator *= lower_step;
      track.upper_oscillator *= upper_step;
      if (monitored_track) track_monitor_oscillator *= monitor_step;
      if ((index & 1'023U) == 1'023U) {
        const auto normalize = [](std::complex<float>& oscillator) {
          const float magnitude = std::abs(oscillator);
          if (magnitude > 0.0F) oscillator /= magnitude;
        };
        normalize(track.center_oscillator);
        normalize(track.lower_oscillator);
        normalize(track.upper_oscillator);
        if (monitored_track) normalize(track_monitor_oscillator);
      }

      if (track.accumulated_samples < evidence_samples) continue;
      const float scale = 1.0F / static_cast<float>(track.accumulated_samples);
      constexpr float kPowerFloor = 1.0e-12F;
      const float observed_lower = track.lower_power_sum * scale;
      const float observed_upper = track.upper_power_sum * scale;
      if (!track.noise_initialized) {
        track.lower_noise_power = observed_lower;
        track.upper_noise_power = observed_upper;
        track.noise_initialized = true;
      } else {
        const auto update_noise = [](float& estimate, const float observed) {
          const float smoothing = observed < estimate ? 0.20F : 0.025F;
          estimate += smoothing * (observed - estimate);
        };
        update_noise(track.lower_noise_power, observed_lower);
        update_noise(track.upper_noise_power, observed_upper);
      }
      // A single quiet sideband must not make every center-bin fluctuation
      // look like a keyed carrier. The geometric mean remains tolerant of a
      // nearby interferer on one side without inheriting the old min-side
      // floor underestimate.
      const float reference_power =
          std::max(std::sqrt(std::max(track.lower_noise_power, kPowerFloor) *
                             std::max(track.upper_noise_power, kPowerFloor)),
                   kPowerFloor);
      std::array<float, kNarrowbandWidthsHz.size()> width_snr{};
      for (std::size_t width = 0; width < kNarrowbandWidthsHz.size(); ++width) {
        const float center_power = track.center_power_sums[width] * scale;
        const float noise_scale = static_cast<float>(
            kNarrowbandWidthsHz[width] / kNarrowbandWidthsHz[1]);
        width_snr[width] =
            10.0F *
            std::log10(std::max(center_power, kPowerFloor) /
                       std::max(reference_power * noise_scale, kPowerFloor));
      }
      // How many keyers is this channel carrying? One keyer reads nearly the
      // same speed however wide the filter around it is; several closer
      // together than any usable filter do not, because the wider filter
      // admits the neighbours and their runs fragment. Both widths are already
      // filtered and their levels already computed above, so the question
      // costs two scalar trackers rather than a second signal path.
      if (config_.resolve_overlapping_keyers) {
        updateKeyerResolution(track, width_snr[kKeyerNarrowWidthIndex],
                              width_snr[kKeyerWideWidthIndex],
                              sample_rate_hz /
                                  static_cast<double>(evidence_samples));
      }
      const float center_localization_ratio =
          track.center_power_sums[2] > kPowerFloor
              ? track.center_power_sums[0] / track.center_power_sums[2]
              : 1.0F;
      // For ideal wideband noise, 60 Hz contains roughly one quarter of the
      // energy in 240 Hz (-6 dB); a centered tone approaches equal energy in
      // both filters (0 dB). Normalize and bound that physical range so the
      // verification threshold has stable meaning and cannot explode when
      // the wide filter happens to be near its numerical floor.
      const float localization_db =
          10.0F * std::log10(std::max(center_localization_ratio, kPowerFloor));
      const float center_localization =
          std::clamp((localization_db + 6.0F) / 6.0F, 0.0F, 1.0F);
      track.narrowband_coherence = track.total_width_observations == 0
                                       ? center_localization
                                       : 0.92F * track.narrowband_coherence +
                                             0.08F * center_localization;
      // Analysis width follows the keying bandwidth the signal actually needs.
      // A keyed carrier occupies roughly four times its element rate, so the
      // requirement is about 4 * (WPM / 1.2) Hz: 60 Hz carries speeds to about
      // 18 WPM, 120 Hz to about 36, and 240 Hz beyond. Choosing the narrowest
      // width that still passes the keying gives the best noise rejection
      // without rounding short elements into each other, which is what made
      // fast signals decode as strings of single-element characters. Selecting
      // on comparative filter power instead let a fast signal sit in a filter
      // too narrow to resolve its own dits.
      const double measured_wpm =
          std::max(track.update.wpm,
                   track.decoder.timingControlCadenceConfidence() >= 0.45F
                       ? track.decoder.timingControlWpm()
                       : 0.0);
      std::size_t preferred = 1;
      if (measured_wpm > 0.0) {
        // 3.5 measured best across the speed/noise surface: the theoretical
        // occupancy is about four element rates, but the outermost sidebands
        // carry little energy and admitting them costs more in noise than the
        // edge sharpness they buy.
        // Two constraints, not one. A width must be wide enough to pass the
        // keying sidebands (about 3.5 element rates) AND settle fast enough
        // that its own group delay is small against an element. The three
        // cascaded single-pole sections give a delay of 3/(pi*width) seconds,
        // so the 60 Hz path costs 15.9 ms - a quarter of a 20 WPM element -
        // which rounds real elements into each other. Selecting purely on
        // bandwidth put ordinary 20 WPM signals in that filter and measurably
        // degraded receiver captures.
        // Wide enough to pass the keying sidebands, which occupy about 3.5
        // element rates. The narrowest 60 Hz path is deliberately excluded:
        // three cascaded single-pole sections give it 3/(pi*60) = 15.9 ms of
        // group delay, a quarter of a 20 WPM element, so it rounds real
        // elements into each other. Selecting it on comparative filter power
        // measurably degraded receiver captures - one lost its callsign
        // entirely and another reported the wrong one.
        // Width must pass the keying sidebands, which occupy about 3.5
        // element rates: 120 Hz therefore serves speeds to about 41 WPM and
        // 240 Hz beyond. The narrowest 60 Hz path is additionally restricted
        // to genuinely slow signals. Three cascaded single-pole sections give
        // it 3/(pi*60) = 15.9 ms of group delay, which is a sixth of a 12 WPM
        // element but a quarter of a 20 WPM one; measured against receiver
        // captures it helps below roughly 15 WPM and clearly hurts above,
        // where selecting it cost one capture its callsign entirely and made
        // another report the wrong one.
        const double required_width_hz = 3.5 * (measured_wpm / 1.2);
        constexpr double kNarrowPathMaximumWpm = 15.0;
        preferred = required_width_hz <= kNarrowbandWidthsHz[0] &&
                            measured_wpm <= kNarrowPathMaximumWpm
                        ? 0U
                    : required_width_hz <= kNarrowbandWidthsHz[1] ? 1U
                                                                  : 2U;
      }
      // A drifting carrier needs headroom regardless of its speed.
      if (std::abs(track.drift_hz_per_second) >= 30.0) preferred = 2;
      if (track.total_width_observations < 250) {
        ++track.total_width_observations;
      } else if (preferred == track.pending_width_index) {
        if (++track.pending_width_observations >= 20) {
          track.selected_width_index = static_cast<std::uint8_t>(preferred);
          track.pending_width_observations = 0;
        }
      } else {
        track.pending_width_index = static_cast<std::uint8_t>(preferred);
        track.pending_width_observations = 1;
      }
      track.snr_db = width_snr[track.selected_width_index];
      if (center_localization_ratio < 0.02F) track.snr_db = 0.0F;
      // Keying evidence is decided in the linear power domain, at the point
      // half way between the estimated space and mark AMPLITUDES. A keyed
      // envelope crosses its half-amplitude at the nominal edge, so marks and
      // gaps are measured without a systematic length bias.
      const float observed_power = std::pow(10.0F, 0.1F * track.snr_db);
      if (!track.keying_envelope_initialized) {
        track.keying_space_power = observed_power;
        track.keying_mark_power = observed_power * 4.0F;
        track.keying_envelope_initialized = true;
      } else {
        const float space_amplitude =
            std::sqrt(std::max(track.keying_space_power, 0.0F));
        const float mark_amplitude =
            std::sqrt(std::max(track.keying_mark_power, 0.0F));
        // Each observation updates only the level it currently belongs to, so
        // the two components stay separated instead of one slow envelope
        // chasing both states. Weighting both levels by how far the sample
        // belongs to each -- the textbook soft assignment, and what the
        // decoder above now does -- fails here for the opposite reason: near
        // the middle the two responsibilities are both near a half, so an
        // ambiguous sample pulls the levels together instead of leaving them
        // alone, and at 12 WPM and 12 dB the model collapsed outright.
        // Measured, 0.388 mean character error against 0.295.
        const float split_amplitude = 0.5F * (space_amplitude + mark_amplitude);
        if (observed_power < split_amplitude * split_amplitude) {
          track.keying_space_power +=
              (observed_power < track.keying_space_power ? 0.30F : 0.02F) *
              (observed_power - track.keying_space_power);
        } else {
          track.keying_mark_power +=
              (observed_power > track.keying_mark_power ? 0.30F : 0.02F) *
              (observed_power - track.keying_mark_power);
        }
        // Keep the two levels separated so a silent channel cannot collapse
        // the model onto one point and start slicing noise.
        track.keying_mark_power =
            std::max(track.keying_mark_power, track.keying_space_power * 2.0F);
      }
      // Periodically regularise the fast online levels against a bounded
      // history.  Otsu's between-class criterion finds the strongest split;
      // medians inside the two groups then represent steady space and mark
      // without giving keying-edge samples the leverage that a mean or a
      // per-frame assignment would.  A split is accepted only when both
      // groups have support and steady-state amplitudes differ by at least
      // two to one. A window containing only noise therefore cannot
      // manufacture a second keying state.
      const float observed_amplitude_for_history = std::sqrt(observed_power);
      if (config_.robust_keying_level_history) {
        track.keying_level_history[track.keying_level_history_index] =
            observed_amplitude_for_history;
        track.keying_level_history_index =
            (track.keying_level_history_index + 1) %
            Track::kKeyingLevelHistorySize;
        track.keying_level_history_count =
            std::min(track.keying_level_history_count + 1,
                     Track::kKeyingLevelHistorySize);
      }
      if (config_.robust_keying_level_history &&
          ++track.keying_level_fit_countdown >= 16 &&
          track.keying_level_history_count >= 64) {
        track.keying_level_fit_countdown = 0;
        std::array<float, Track::kKeyingLevelHistorySize> ordered{};
        std::copy_n(track.keying_level_history.begin(),
                    track.keying_level_history_count, ordered.begin());
        std::sort(ordered.begin(),
                  ordered.begin() + static_cast<std::ptrdiff_t>(
                                        track.keying_level_history_count));
        const std::size_t count = track.keying_level_history_count;
        const std::size_t minimum_group = std::max<std::size_t>(8, count / 10);
        double total = 0.0;
        double total_squared = 0.0;
        for (std::size_t sample = 0; sample < count; ++sample) {
          total += ordered[sample];
          total_squared +=
              static_cast<double>(ordered[sample]) * ordered[sample];
        }
        double lower_sum = 0.0;
        double best_separation = -1.0;
        std::size_t best_split = 0;
        for (std::size_t split = 1; split < count; ++split) {
          lower_sum += ordered[split - 1];
          if (split < minimum_group || count - split < minimum_group) continue;
          const double lower_mean = lower_sum / static_cast<double>(split);
          const double upper_mean =
              (total - lower_sum) / static_cast<double>(count - split);
          const double difference = upper_mean - lower_mean;
          const double separation = static_cast<double>(split) *
                                    static_cast<double>(count - split) *
                                    difference * difference;
          if (separation > best_separation) {
            best_separation = separation;
            best_split = split;
          }
        }
        if (best_split >= minimum_group &&
            count - best_split >= minimum_group) {
          const double total_variation = std::max(
              total_squared - total * total / static_cast<double>(count),
              1.0e-12);
          const double explained_variation =
              (best_separation / static_cast<double>(count)) / total_variation;
          track.keying_level_explained_variation =
              static_cast<float>(std::clamp(explained_variation, 0.0, 1.0));
          track.robust_keying_level_anchor_active = false;
          const float robust_space_amplitude = ordered[best_split / 2];
          const float robust_mark_amplitude =
              ordered[best_split + (count - best_split) / 2];
          // Require at least 6 dB of steady-state power separation before a
          // historical split may steer the live tracker.  Otsu necessarily
          // finds a split even in one broad noise population; its two halves
          // commonly clear the tracker's minimal sqrt(2) amplitude ordering,
          // but not this stronger evidence gate.
          constexpr float kMinimumAmplitudeSeparation = 2.0F;
          constexpr double kMinimumExplainedVariation = 0.70;
          if (explained_variation >= kMinimumExplainedVariation &&
              robust_space_amplitude > 0.0F &&
              robust_mark_amplitude >=
                  robust_space_amplitude * kMinimumAmplitudeSeparation) {
            constexpr float kRobustAnchorRate = 0.015F;
            const float current_space_amplitude =
                std::sqrt(std::max(track.keying_space_power, 0.0F));
            const float current_mark_amplitude =
                std::sqrt(std::max(track.keying_mark_power, 0.0F));
            const float anchored_space_amplitude =
                current_space_amplitude +
                kRobustAnchorRate *
                    (robust_space_amplitude - current_space_amplitude);
            const float anchored_mark_amplitude =
                current_mark_amplitude +
                kRobustAnchorRate *
                    (robust_mark_amplitude - current_mark_amplitude);
            track.keying_space_power =
                anchored_space_amplitude * anchored_space_amplitude;
            track.keying_mark_power =
                std::max(anchored_mark_amplitude * anchored_mark_amplitude,
                         track.keying_space_power * 2.0F);
            track.robust_keying_level_anchor_active = true;
          }
        }
      }
      // Mark level in dB above the side-noise reference the powers are
      // already measured against. Held for presentation so the operator sees
      // the signal rather than whichever half of the keying cycle happened to
      // be sampled.
      if (track.keying_envelope_initialized) {
        track.keying_mark_snr_db =
            10.0F * std::log10(std::max(track.keying_mark_power, 1.0e-12F));
      }
      const float space_amplitude =
          std::sqrt(std::max(track.keying_space_power, 0.0F));
      const float mark_amplitude =
          std::sqrt(std::max(track.keying_mark_power, 0.0F));
      const float middle_amplitude = 0.5F * (space_amplitude + mark_amplitude);
      const float observed_amplitude = std::sqrt(observed_power);
      // Scatter about the assigned level, tracked slowly so a burst of marks
      // cannot make the channel look quiet. A sample caught on a keying edge
      // sits legitimately between the levels and says nothing about noise, so
      // it is excluded: at 40 WPM the dot is barely two detector frames long
      // and edge samples would otherwise dominate the estimate, flattening
      // the decision on exactly the clean fast signals it should sharpen.
      const float half_span_amplitude =
          std::max(0.5F * (mark_amplitude - space_amplitude), 1.0e-6F);
      const bool nearer_space = observed_amplitude < middle_amplitude;
      const float level_residual =
          observed_amplitude -
          (nearer_space ? space_amplitude : mark_amplitude);
      // A mark carries signal plus noise and a space carries noise alone, so
      // the two levels do not scatter equally. Tracking them separately is
      // what lets the ratio state that a quiet space is more certain than a
      // fading mark, rather than averaging the two into one blunt figure.
      float& level_variance = nearer_space ? track.keying_space_variance
                                           : track.keying_mark_variance;
      // Admit a sample into a level's scatter only if it is plausibly ON that
      // level. A keying edge sweeps through both levels' territory, and an
      // edge sample admitted here inflates one level's scatter far above the
      // other's -- which, with the boundary now sensitive to that ratio,
      // moves the decision by a lot. Gate on the level's own established
      // spread, falling back to a fraction of the level separation before any
      // spread is known.
      if (level_variance <= 0.0F) {
        level_variance = level_residual * level_residual;
      } else if (std::abs(level_residual) <= 0.5F * half_span_amplitude) {
        level_variance +=
            0.02F * (level_residual * level_residual - level_variance);
      }
      // A mark carries the space's noise plus the signal's own fluctuation,
      // so its scatter can never be the smaller of the two. Keying edges sweep
      // through both levels and can transiently inflate the space estimate
      // past the mark's, which inverts the boundary shift and pushes the
      // decision away from the mark instead of toward it -- on a clean signal
      // that mistimes every element badly enough to lose the text entirely.
      // Constrain the pair the same way the levels themselves are kept
      // ordered.
      track.keying_space_variance = std::min(track.keying_space_variance,
                                             track.keying_mark_variance > 0.0F
                                                 ? track.keying_mark_variance
                                                 : track.keying_space_variance);
      // Log-likelihood ratio between the mark and space hypotheses under the
      // two-level model. With both levels carrying the same scatter this is
      // the separation times the signed distance from the midpoint, over that
      // scatter: the standard soft-decision result. The slope is measured
      // rather than chosen, so it steepens when the levels separate cleanly
      // and flattens toward zero -- meaning "no information" -- when they do
      // not. That is the honest statement for a channel carrying no CW, and
      // it replaces both the hand-set decision gain and the separation weight
      // that used to push such a channel toward key-up. A hard slicer instead
      // discards this margin, which is what fragmented weak elements: at 6 dB
      // nine marks in ten broke up and mean mark length fell to 18 ms against
      // a true 60 ms, even though spectral acquisition never failed.
      const float variance_floor =
          std::max(1.0e-12F, 1.0e-4F * middle_amplitude * middle_amplitude);
      const float space_variance =
          std::max(track.keying_space_variance, variance_floor);
      const float mark_variance =
          std::max(track.keying_mark_variance, variance_floor);
      // Likelihood ratio under the two-level model with each level carrying
      // its own scatter. Unequal scatter moves the decision off half
      // amplitude, and here that is wanted rather than tolerated: a mark
      // carries signal plus noise and a space carries noise alone, so the
      // boundary sits nearer the mark and noise excursions stop producing
      // marks. That is the whole weak-signal gain.
      const float space_offset = observed_amplitude - space_amplitude;
      const float mark_offset = observed_amplitude - mark_amplitude;
      const float log_likelihood_ratio =
          0.5F * (space_offset * space_offset / space_variance -
                  mark_offset * mark_offset / mark_variance);
      track.keying_snr_db =
          track.decoder.evidenceForLogLikelihoodRatio(log_likelihood_ratio);
      const auto timestamp_ns =
          block.timestamp_ns +
          static_cast<std::uint64_t>(static_cast<long double>(index) *
                                     1'000'000'000.0L / sample_rate_hz);
      const bool candidate_match_held =
          timestamp_ns < track.last_candidate_match_ns ||
          static_cast<long double>(timestamp_ns -
                                   track.last_candidate_match_ns) /
                  1'000'000'000.0L <=
              kCandidateMatchHoldSeconds;
      // A track too weak to be copied is simply not fed to its decoder. It is
      // deliberately not suspended: suspension is the association-loss path
      // and is only resumed from there, so a track suspended for being quiet
      // would never be decoded again once it grew loud.
      //
      // The gate reads the strongest level the track has reached, not the
      // level of the moment. A signal is weak while it is still being
      // acquired, and gating on that suppressed it before it could establish
      // itself: measured on the capture corpus, it cost two of eight
      // recovered callsigns, one of whose settled level was thirty-five
      // decibels. An operator-selected track is always decoded, because an
      // operator saying "this is a signal" is better evidence than a level.
      const float decode_level_db = track.keying_envelope_initialized
          ? track.keying_mark_snr_db : track.snr_db;
      track.peak_decode_level_db =
          std::max(track.peak_decode_level_db, decode_level_db);
      // A parked track is outside the processed passband and there is nothing
      // at its frequency to decode. Feeding it would append whatever noise sits
      // at the filter's edge to a transcript the operator is keeping, so it
      // holds its text untouched until the dial brings the station back.
      const bool loud_enough_to_decode = !track.parked &&
          (config_.decode_weak_signals || track.operator_selected ||
           track.peak_decode_level_db >= config_.minimum_decode_snr_db);
      if (!candidate_match_held) {
        if (!track.decoder_input_suspended) {
          // Drain a possibly keyed acoustic segment once, but do not claim an
          // operator turn ended: an ordinary slow-CW word gap can exceed this
          // association hold. Reacquisition applies the decoder's longer
          // sustained-silence rule before creating a semantic turn.
          track.update = track.decoder.suspendInput(timestamp_ns);
          track.decoder_input_suspended = true;
        }
      } else if (!track.decoder_input_suspended && loud_enough_to_decode) {
        track.update = track.decoder.process(timestamp_ns, track.keying_snr_db);
        updateVerification(track, timestamp_ns);
        recoverRejectedDecoder(track);
      }
      track.center_power_sums = {};
      track.lower_power_sum = 0.0F;
      track.upper_power_sum = 0.0F;
      track.accumulated_samples = 0;
    }
    if (monitored_track) advanced_monitor_oscillator = track_monitor_oscillator;
  }

  if (active_monitor_count > 0U) {
    monitor_oscillator_ = advanced_monitor_oscillator;
    for (float& sample : monitor_audio_)
      sample = std::clamp(sample, -1.0F, 1.0F);
  }

  expected_sample_timestamp_ns_ =
      block.timestamp_ns +
      static_cast<std::uint64_t>(static_cast<long double>(block.sample_count) *
                                 1'000'000'000.0L / sample_rate_hz);
  sample_timing_initialized_ = true;
  rebuildSnapshots(expected_sample_timestamp_ns_);
  return snapshots_;
}

const std::vector<CwChannelSnapshot>& CwChannelBank::channels() const noexcept {
  return snapshots_;
}

CwVerificationDiagnostics CwChannelBank::verificationDiagnostics() const {
  CwVerificationDiagnostics result{
      .verified_transitions = verified_transitions_,
      .expired_unverified_tracks = expired_unverified_tracks_,
      .decoder_reacquisitions = decoder_reacquisitions_,
  };
  for (const auto& track : tracks_) {
    switch (track.verification_state) {
      case CwTrackState::Candidate: ++result.candidate_tracks; break;
      case CwTrackState::MorseLikely: ++result.morse_likely_tracks; break;
      case CwTrackState::Verified: ++result.verified_tracks; break;
      case CwTrackState::Lost: break;
    }
    const auto reason = static_cast<std::size_t>(track.verification_reason);
    if (reason < result.current_reason_counts.size())
      ++result.current_reason_counts[reason];
    result.maximum_decoded_symbols =
        std::max(result.maximum_decoded_symbols, track.update.decoded_symbols);
    result.maximum_key_transitions =
        std::max(result.maximum_key_transitions, track.update.key_transitions);
    result.best_timing_quality =
        std::max(result.best_timing_quality, track.update.timing_quality);
    result.best_cadence_quality =
        std::max(result.best_cadence_quality, track.update.cadence_quality);
    if (track.verification_state == CwTrackState::Verified &&
        track.distinctive_token_seen &&
        track.update.timing_quality <
            config_.minimum_verification_timing_quality) {
      ++result.pattern_verified_tracks;
    }
    if (track.verification_state == CwTrackState::Verified &&
        track.keyer_overlap_unresolved) {
      ++result.unresolved_overlap_tracks;
      if (track.overlap_neighbour_count + 1U <= kJointSeparationMaximumSources)
        ++result.joint_separation_candidates;
    }
    result.best_narrowband_coherence =
        std::max(result.best_narrowband_coherence, track.narrowband_coherence);
  }
  return result;
}

float CwChannelBank::estimateNoise(
    const std::span<const float> bins_dbfs) const {
  std::vector<float> finite;
  finite.reserve(bins_dbfs.size());
  for (const float value : bins_dbfs) {
    if (std::isfinite(value)) finite.push_back(value);
  }
  if (finite.empty()) return -200.0F;
  const std::size_t index = finite.size() / 2;
  std::nth_element(finite.begin(),
                   finite.begin() + static_cast<std::ptrdiff_t>(index),
                   finite.end());
  return finite[index];
}

float CwChannelBank::spectralSnr(const Track& track,
                                 const double lower_frequency_hz,
                                 const double bin_width_hz,
                                 const std::span<const float> bins_dbfs,
                                 const float noise_dbfs) const {
  const double position =
      (track.frequency_hz - lower_frequency_hz) / bin_width_hz;
  const auto center = static_cast<std::ptrdiff_t>(std::llround(position));
  float peak = -200.0F;
  for (std::ptrdiff_t offset = -1; offset <= 1; ++offset) {
    const std::ptrdiff_t index = center + offset;
    if (index < 0 || index >= static_cast<std::ptrdiff_t>(bins_dbfs.size()))
      continue;
    peak = std::max(peak, bins_dbfs[static_cast<std::size_t>(index)]);
  }
  return std::isfinite(peak) ? peak - noise_dbfs : -100.0F;
}

void CwChannelBank::resetFilter(Track& track) noexcept {
  track.center_filters = {};
  track.lower_filter = {};
  track.upper_filter = {};
  track.center_oscillator = {1.0F, 0.0F};
  track.lower_oscillator = {1.0F, 0.0F};
  track.upper_oscillator = {1.0F, 0.0F};
  track.center_power_sums = {};
  track.lower_power_sum = 0.0F;
  track.upper_power_sum = 0.0F;
  track.accumulated_samples = 0;
  track.lower_noise_power = 0.0F;
  track.upper_noise_power = 0.0F;
  track.monitor_peak_envelope = 0.0F;
  track.selected_width_index = 1;
  track.pending_width_index = 1;
  track.pending_width_observations = 0;
  track.total_width_observations = 0;
  track.noise_initialized = false;
  track.keying_snr_db = 0.0F;
  track.keying_mark_snr_db = 0.0F;
  track.keying_space_power = 0.0F;
  track.keying_mark_power = 0.0F;
  track.keying_space_variance = 0.0F;
  track.keying_mark_variance = 0.0F;
  track.keying_level_history = {};
  track.keying_level_history_count = 0;
  track.keying_level_history_index = 0;
  track.keying_level_fit_countdown = 0;
  track.keying_level_explained_variation = 0.0F;
  track.robust_keying_level_anchor_active = false;
  track.keying_envelope_initialized = false;
  // The run histories describe what the filters were hearing, so they go with
  // the filters. The standing verdict deliberately does not: a receiver retune
  // or a stream change moves the signal path, not the stations, and a track
  // that was in the middle of a pileup before the dial moved is still in one
  // after it. Clearing the verdict here would republish a window of noise
  // transcripts on every retune until the evidence rebuilt.
  for (auto& runs : track.keyer_runs) runs = {};
  // The cadence high-water goes with them, and for the same reason: it records
  // what one signal path was carrying, and the path is what changed.
  track.keyer_cadence_peak = 0.0F;
  track.keyer_evaluation_countdown = 0;
  track.filter_initialized = false;
}

void CwChannelBank::shiftTrackedFrequencies(
    const double audio_hz_delta) noexcept {
  if (audio_hz_delta == 0.0 || !std::isfinite(audio_hz_delta)) return;
  for (Track& track : tracks_) {
    track.frequency_hz += audio_hz_delta;
    track.identity_origin_frequency_hz += audio_hz_delta;
    track.presentation_frequency_hz += audio_hz_delta;
    for (std::size_t index = 0;
         index < track.presentation_frequency_evidence_count; ++index) {
      track.presentation_frequency_evidence[index] += audio_hz_delta;
    }
    track.presentation_follow_median_hz += audio_hz_delta;
    track.presentation_follow_stable_since_ns = 0;
    track.last_presentation_follow_update_ns = 0;
    resetFilter(track);
  }
  for (ColorLease& lease : color_leases_) {
    if (!lease.occupied) continue;
    lease.frequency_hz += audio_hz_delta;
    if (lease.frequency_hz <= 0.0) lease = {};
  }
  for (RetainedObservation& observation : retained_observations_) {
    observation.snapshot.frequency_hz += audio_hz_delta;
    observation.snapshot.presentation_frequency_hz += audio_hz_delta;
  }
  std::erase_if(retained_observations_,
                [](const RetainedObservation& observation) {
                  return observation.snapshot.frequency_hz <= 0.0;
                });
  // A retune can carry a track out of the processed passband, at either end.
  // That is not a lost signal and must not be treated as one: the operator
  // turned the dial, the station's position is known exactly, and turning the
  // dial back puts it at a computable place. Letting it expire through the
  // ordinary retention timeout -- which is what used to happen, deliberately
  // -- destroyed the identity, the transcript and the audio monitor of the
  // very station being tuned around, and it returned as a new, unrecognised
  // track. The same rule serves a retune of the IQ decoder window, so the two
  // cannot disagree about what "out of band" means.
  parkTracksOutsideBand(expected_sample_timestamp_ns_);
  // A track parked below zero has no meaningful position to return to, so it
  // is the one case still dropped outright.
  std::erase_if(tracks_,
                [](const Track& track) { return track.frequency_hz <= 0.0; });
  rebuildSnapshots(expected_sample_timestamp_ns_);
}

void CwChannelBank::measureTrackNeighbourhoods() noexcept {
  for (Track& track : tracks_) {
    std::size_t neighbours = 0;
    double nearest_hz = 0.0;
    for (const Track& other : tracks_) {
      if (other.id == track.id) continue;
      const double separation =
          std::abs(other.frequency_hz - track.frequency_hz);
      if (separation > kJointSeparationRadiusHz) continue;
      ++neighbours;
      if (nearest_hz == 0.0 || separation < nearest_hz) nearest_hz = separation;
    }
    track.overlap_neighbour_count =
        static_cast<std::uint8_t>(std::min<std::size_t>(neighbours, 255));
    track.nearest_neighbour_separation_hz = nearest_hz;
  }
}

void CwChannelBank::updateKeyerResolution(
    Track& track, const float narrow_snr_db, const float wide_snr_db,
    const double evidence_frame_rate_hz) noexcept {
  // One evidence frame folded into one width's two-level model and run
  // history. Nothing here decides anything about the decode; it only counts
  // how often this width's envelope crosses between its own two levels.
  const auto observe = [](Track::KeyingRuns& runs, const float snr_db) {
    const float power = std::pow(10.0F, 0.1F * snr_db);
    if (!runs.initialized) {
      runs.space_power = power;
      runs.mark_power = power * 4.0F;
      runs.initialized = true;
      return;
    }
    const float space_amplitude = std::sqrt(std::max(runs.space_power, 0.0F));
    const float mark_amplitude = std::sqrt(std::max(runs.mark_power, 0.0F));
    const float amplitude = std::sqrt(std::max(power, 0.0F));
    const float middle_amplitude = 0.5F * (space_amplitude + mark_amplitude);
    // Same asymmetric assignment the decoder's own level tracker uses: each
    // observation moves only the level it belongs to, quickly when it is
    // beyond that level and slowly when it is inside, so the two stay apart
    // instead of one envelope chasing both. No separation floor, though --
    // levels that collapse together are the signal that this channel is not
    // being keyed at all, and the admission test below reads exactly that.
    if (amplitude < middle_amplitude) {
      runs.space_power +=
          (power < runs.space_power ? 0.30F : 0.02F) *
          (power - runs.space_power);
    } else {
      runs.mark_power +=
          (power > runs.mark_power ? 0.30F : 0.02F) * (power - runs.mark_power);
    }
    if (runs.mark_power < runs.space_power) {
      // The assignment above cannot reorder the levels on real keying, but a
      // collapsed model can cross them on noise. Keeping the order defined
      // costs nothing and keeps the separation test below meaningful.
      std::swap(runs.mark_power, runs.space_power);
    }
    const bool believable =
        runs.mark_power >= runs.space_power * kKeyerRunLevelSeparation;
    runs.believable_duty +=
        0.002F * ((believable ? 1.0F : 0.0F) - runs.believable_duty);
    if (!believable) {
      // Not keyed, or not keyed clearly enough to time. Discard the window
      // rather than pausing it, so an estimate is only ever made from one
      // continuous stretch of believable keying.
      //
      // Stitching a window together across the gaps mattered, and measurably:
      // a weak clean signal loses and regains a believable split many times a
      // second, and the moments it is believable are precisely its loudest
      // noise peaks, so a stitched window is assembled from the least
      // representative frames it saw. Measured on the synthetic fixtures, that
      // read a clean 16 WPM carrier at 12 dB as several keyers. The standing
      // verdict is separate state and is not cleared here, so a station that
      // simply stops sending keeps whatever was last decided about it.
      runs.run_frames = 0;
      return;
    }
    const float half_span_amplitude =
        std::max(0.5F * (mark_amplitude - space_amplitude), 1.0e-9F);
    const float hysteresis = kKeyerRunHysteresisFraction * half_span_amplitude;
    const bool key_down =
        runs.key_down ? amplitude > middle_amplitude - hysteresis
                      : amplitude > middle_amplitude + hysteresis;
    if (runs.run_frames != 0U && key_down != runs.key_down) {
      runs.run_history[runs.run_index] = static_cast<std::uint16_t>(
          std::min<std::uint32_t>(runs.run_frames, 65'535U));
      runs.run_index =
          (runs.run_index + 1U) % Track::KeyingRuns::kRunHistorySize;
      runs.run_count = std::min(runs.run_count + 1U,
                                Track::KeyingRuns::kRunHistorySize);
      runs.run_frames = 0;
    }
    runs.key_down = key_down;
    if (runs.run_frames < 65'535U) ++runs.run_frames;
  };
  observe(track.keyer_runs[0], narrow_snr_db);
  observe(track.keyer_runs[1], wide_snr_db);
  track.keyer_cadence_peak = std::max(
      track.keyer_cadence_peak, track.update.acoustic_cadence_confidence);

  if (track.keyer_evaluation_countdown < kKeyerEvaluationFrames) {
    ++track.keyer_evaluation_countdown;
    return;
  }
  track.keyer_evaluation_countdown = 0;

  // Two readings of one question, kept together because each answers where the
  // other cannot, and either refusing the track is enough to refuse it.
  //
  // The speed comparison needs two full run windows at two filter widths and a
  // narrow width solid enough to be the reference half. On a dense pileup that
  // is mostly unavailable -- on the operator's capture it produced no estimate
  // at all for 41 of the 48 published carriers. The cadence fit needs only
  // that the decoder has estimated a cadence, which it has for every track
  // carrying anything at all, and it collapses on exactly the population the
  // ratio goes silent on. Where both can speak they agree; where only one can,
  // that one decides; where neither can, the status quo stands.
  bool speed_answers = false;
  bool speed_overlapping = false;
  const auto& narrow = track.keyer_runs[0];
  const auto& wide = track.keyer_runs[1];
  if (narrow.run_count >= Track::KeyingRuns::kRunHistorySize &&
      wide.run_count >= Track::KeyingRuns::kRunHistorySize) {
    const double narrow_wpm = cwKeyingSpeedFromRuns(
        {narrow.run_history.data(), narrow.run_count}, evidence_frame_rate_hz);
    const double wide_wpm = cwKeyingSpeedFromRuns(
        {wide.run_history.data(), wide.run_count}, evidence_frame_rate_hz);
    if (narrow_wpm > 0.0 && wide_wpm > 0.0) {
      track.keyer_speed_ratio = static_cast<float>(wide_wpm / narrow_wpm);
      // The ratio is recorded either way, because it is evidence a later stage
      // can read, but it may only speak when its reference half was solid.
      if (narrow.believable_duty >= kKeyerMinimumNarrowDuty) {
        speed_answers = true;
        speed_overlapping =
            track.keyer_speed_ratio > config_.maximum_single_keyer_speed_ratio;
      }
    }
  }

  // Zero is the decoder saying it has no cadence estimate, not a channel with
  // a bad one, so it is silence rather than a refusal.
  //
  // And a collapsed cadence only means several keyers where there are several
  // carriers close enough that no usable filter separates them. Alone on a
  // frequency, or with one neighbour, the same collapse means poor copy -- a
  // fast signal in noise, fading, a weak one -- and refusing it would take the
  // transcript away from a station the operator can work. Measured: without
  // this guard a clean 40 WPM carrier with receiver noise and nothing else on
  // the band loses its transcript outright. The neighbourhood is measured once
  // per audio block for every track anyway.
  const bool crowded =
      track.overlap_neighbour_count >= kKeyerCadenceMinimumNeighbours;
  const bool cadence_answers = crowded && track.keyer_cadence_peak > 0.0F;
  const bool cadence_overlapping =
      cadence_answers &&
      track.keyer_cadence_peak < config_.minimum_single_keyer_cadence_fit;

  if (!speed_answers && !cadence_answers) {
    track.keyer_overlap_evaluations = 0;
    track.keyer_single_evaluations = 0;
    return;
  }

  const bool overlapping = speed_overlapping || cadence_overlapping;
  if (overlapping) {
    track.keyer_single_evaluations = 0;
    if (track.keyer_overlap_evaluations < kKeyerVerdictEvaluations)
      ++track.keyer_overlap_evaluations;
  } else {
    track.keyer_overlap_evaluations = 0;
    if (track.keyer_single_evaluations < kKeyerVerdictEvaluations)
      ++track.keyer_single_evaluations;
  }
  if (overlapping &&
      track.keyer_overlap_evaluations >= kKeyerVerdictEvaluations) {
    track.keyer_overlap_unresolved = true;
  }
  if (!overlapping &&
      track.keyer_single_evaluations >= kKeyerVerdictEvaluations) {
    track.keyer_overlap_unresolved = false;
  }
}

void CwChannelBank::updateVerification(Track& track,
                                       const std::uint64_t timestamp_ns) {
  const bool was_verified = track.verification_state == CwTrackState::Verified;
  const std::size_t plausibility_window = std::max<std::size_t>(
      64,
      static_cast<std::size_t>(config_.minimum_plausibility_check_characters) *
          2U);
  const std::string recent_text =
      track.update.text.size() > plausibility_window
          ? track.update.text.substr(track.update.text.size() -
                                     plausibility_window)
          : track.update.text;
  // A track already judged to hold several keyers is exempt from this gate.
  // The gate demotes a track whose text reads as timing noise, on the grounds
  // that a transcript like that is better explained by noise than by a
  // station. For this track that question is already settled and settled the
  // other way: the carrier is real, several stations are keying it, and the
  // implausible text is the arithmetic of their sum rather than evidence about
  // the carrier. Demoting on it would strip the marker, the frequency and the
  // key-state occupancy from precisely the signal the operator is trying to
  // see -- measured, it removes the published channel outright -- and it would
  // protect nobody, because the text is withheld either way.
  if (!track.keyer_overlap_unresolved &&
      isCharacterDistributionImplausible(
          recent_text, config_.minimum_plausibility_check_characters,
          config_.maximum_simple_character_fraction)) {
    track.verification_state = CwTrackState::Candidate;
    track.verification_reason =
        CwVerificationReason::ImplausibleCharacterDistribution;
    track.verification_pass_samples = 0;
    track.verification_fail_samples = 0;
    return;
  }

  // Absence is not contradictory evidence. Keep the verified observation and
  // its marker inactive until the configured decoded-track timeout expires;
  // only an actively matched carrier can accumulate evidence that demotes it.
  // Previously, ordinary key-up/silence decayed the evidence counters and
  // demoted a verified track after about two seconds, bypassing the advertised
  // 30-second retention and causing every later pass to receive a new ID/color.
  if (was_verified && !track.matched &&
      track.spectral_snr_db < config_.retention_snr_db) {
    track.verification_reason = CwVerificationReason::SignalLost;
    return;
  }

  const auto observations = track.spectral_observations;
  const auto symbols = track.update.recent_decoded_symbols > 0
                           ? track.update.recent_decoded_symbols
                           : track.update.decoded_symbols;
  const auto unknown = std::min(track.update.recent_decoded_symbols > 0
                                    ? track.update.recent_unknown_symbols
                                    : track.update.unknown_symbols,
                                symbols);
  const auto cadence_observations =
      track.update.recent_cadence_observations > 0
          ? track.update.recent_cadence_observations
          : track.update.cadence_observations;
  const auto known = symbols - unknown;
  if (track.update.text.size() != track.distinctive_token_scanned_length) {
    track.distinctive_token_scanned_length = track.update.text.size();
    if (!track.distinctive_token_seen)
      track.distinctive_token_seen =
          cwTextContainsDistinctiveToken(recent_text);
  }
  const bool character_refinement_current =
      track.character_refinement_timestamp_ns != 0U &&
      timestamp_ns >= track.character_refinement_timestamp_ns &&
      static_cast<double>(timestamp_ns -
                          track.character_refinement_timestamp_ns) /
              1'000'000'000.0 <=
          kCharacterRefinementEvidenceSeconds;
  // A recognised token is evidence about whether this is Morse that does not
  // come from the timing measures themselves, so it may stand in for the three
  // gates that judge how good the decoded characters are. It deliberately does
  // not stand in for the requirement to have decoded enough of them: a token
  // says the content is genuine, not that there is enough of it, and a track
  // that stops producing symbols must still be able to fall out of
  // verification. Neither this nor a character model can be reached without the
  // carrier, keyed-edge, cadence and coherence gates having already passed, so
  // neither can verify a channel carrying no signal.
  const bool independent_morse_evidence =
      character_refinement_current || track.distinctive_token_seen;
  const float unknown_fraction =
      symbols == 0 ? 1.0F
                   : static_cast<float>(unknown) / static_cast<float>(symbols);
  const float persistence = std::min(
      1.0F, static_cast<float>(observations) /
                static_cast<float>(config_.minimum_spectral_observations));
  const float edge_evidence =
      std::min(1.0F, static_cast<float>(track.update.key_transitions) /
                         static_cast<float>(config_.minimum_key_transitions));
  const float symbol_evidence =
      config_.minimum_verification_symbols == 0
          ? 1.0F
          : std::min(1.0F, static_cast<float>(known) /
                               static_cast<float>(
                                   config_.minimum_verification_symbols));
  track.verification_confidence =
      std::clamp(0.12F * persistence + 0.12F * edge_evidence +
                     0.16F * track.narrowband_coherence +
                     0.18F * track.update.cadence_quality +
                     0.18F * track.update.timing_quality +
                     0.16F * track.update.mean_character_confidence +
                     0.08F * symbol_evidence,
                 0.0F, 1.0F);

  CwTrackState eligible_state = CwTrackState::Candidate;
  CwVerificationReason failure_reason =
      CwVerificationReason::NeedsSpectralPersistence;
  bool passes = false;
  if (observations < config_.minimum_spectral_observations) {
    failure_reason = CwVerificationReason::NeedsSpectralPersistence;
  } else if (config_.minimum_verification_symbols == 0) {
    passes = true;
  } else if (track.update.key_transitions < config_.minimum_key_transitions) {
    failure_reason = CwVerificationReason::NeedsKeyingEdges;
  } else if (cadence_observations < config_.minimum_cadence_observations) {
    failure_reason = CwVerificationReason::NeedsCadenceEvidence;
  } else if (track.narrowband_coherence <
             config_.minimum_narrowband_coherence) {
    failure_reason = CwVerificationReason::LowNarrowbandCoherence;
  } else if (track.update.cadence_quality <
             config_.minimum_verification_cadence_quality) {
    failure_reason = CwVerificationReason::LowCadenceQuality;
  } else {
    eligible_state = CwTrackState::MorseLikely;
    track.ever_morse_likely = true;
    if (!character_refinement_current &&
        known < config_.minimum_verification_symbols) {
      failure_reason = CwVerificationReason::NeedsDecodedSymbols;
    } else if (!independent_morse_evidence &&
               unknown_fraction >
                   config_.maximum_verification_unknown_fraction) {
      failure_reason = CwVerificationReason::TooManyUnknownSymbols;
    } else if (!independent_morse_evidence &&
               track.update.timing_quality <
                   config_.minimum_verification_timing_quality) {
      failure_reason = CwVerificationReason::LowTimingQuality;
    } else if (!independent_morse_evidence &&
               track.update.mean_character_confidence <
                   config_.minimum_character_confidence) {
      failure_reason = CwVerificationReason::LowCharacterConfidence;
    } else {
      passes = true;
    }
  }

  // A verified track that could not be resolved to a single keyer is still a
  // verified keyed carrier -- it has the persistence, the edges, the cadence
  // and the coherence -- so it keeps the Verified state, its marker and its
  // occupancy. What changes is the reason it reports, and that is what
  // withholds its text downstream. Reusing the state machine rather than
  // running a second one beside it means the silence follows the same
  // acquisition, retention, colour and identity rules as everything else.
  const auto verified_reason = [&track]() {
    return track.keyer_overlap_unresolved
               ? CwVerificationReason::UnresolvedKeyerOverlap
               : CwVerificationReason::Verified;
  };

  const auto enter_samples = static_cast<std::uint16_t>(std::clamp(
      std::lround(config_.verification_enter_seconds *
                  config_.evidence_rate_hz),
      1L, static_cast<long>(std::numeric_limits<std::uint16_t>::max())));
  const auto exit_samples = static_cast<std::uint16_t>(std::clamp(
      std::lround(config_.verification_exit_seconds * config_.evidence_rate_hz),
      1L, static_cast<long>(std::numeric_limits<std::uint16_t>::max())));

  if (!passes) {
    track.verification_pass_samples = 0;
    if (was_verified) {
      if (track.verification_fail_samples < exit_samples)
        ++track.verification_fail_samples;
      if (track.verification_fail_samples < exit_samples) {
        track.verification_reason = verified_reason();
        return;
      }
    }
    track.verification_fail_samples = 0;
    track.verification_state = eligible_state;
    track.verification_reason = failure_reason;
    return;
  }

  track.verification_fail_samples = 0;
  if (config_.minimum_verification_symbols == 0) {
    track.verification_state = CwTrackState::Verified;
    track.verification_reason = verified_reason();
    if (!track.ever_verified)
      reanchorPresentationOnFirstVerification(track, timestamp_ns);
    track.ever_verified = true;
    track.operator_selected = false;
    track.verification_confidence = 1.0F;
    assignOrRefreshColor(track, timestamp_ns);
    if (!was_verified) ++verified_transitions_;
    return;
  }
  if (!was_verified) {
    if (track.verification_pass_samples < enter_samples)
      ++track.verification_pass_samples;
    if (track.verification_pass_samples < enter_samples) {
      track.verification_state = CwTrackState::MorseLikely;
      track.verification_reason = CwVerificationReason::NeedsSustainedEvidence;
      return;
    }
  }

  track.verification_pass_samples = enter_samples;
  track.verification_state = CwTrackState::Verified;
  track.verification_reason = verified_reason();
  if (!track.ever_verified)
    reanchorPresentationOnFirstVerification(track, timestamp_ns);
  track.ever_verified = true;
  track.operator_selected = false;
  assignOrRefreshColor(track, timestamp_ns);
  track.verification_cadence_quality = track.update.cadence_quality;
  track.verification_timing_quality = track.update.timing_quality;
  track.verification_character_confidence =
      track.update.mean_character_confidence;
  if (!was_verified) {
    ++verified_transitions_;
  }
}

bool CwChannelBank::acceptCharacterRefinement(
    const std::uint64_t track_id, const std::string& stable_text,
    const std::uint64_t evidence_timestamp_ns) {
  auto track = std::find_if(
      tracks_.begin(), tracks_.end(),
      [track_id](const Track& candidate) { return candidate.id == track_id; });
  if (track == tracks_.end()) return false;
  const std::uint64_t current_timestamp_ns = expected_sample_timestamp_ns_ != 0U
                                                 ? expected_sample_timestamp_ns_
                                                 : evidence_timestamp_ns;
  const bool timestamp_is_current =
      evidence_timestamp_ns != 0U &&
      evidence_timestamp_ns > track->character_refinement_timestamp_ns &&
      (evidence_timestamp_ns <= current_timestamp_ns ||
       evidence_timestamp_ns - current_timestamp_ns <= 1'000'000'000ULL) &&
      (current_timestamp_ns <= evidence_timestamp_ns ||
       current_timestamp_ns - evidence_timestamp_ns <=
           static_cast<std::uint64_t>(kCharacterRefinementEvidenceSeconds *
                                      1'000'000'000.0));
  if (!timestamp_is_current || !track->ever_morse_likely ||
      track->verification_state == CwTrackState::Lost ||
      track->spectral_observations < config_.minimum_spectral_observations ||
      track->update.key_transitions < config_.minimum_key_transitions ||
      !CallsignPolicy::latest_in_text(stable_text)) {
    return false;
  }
  track->character_refinement_timestamp_ns = evidence_timestamp_ns;
  updateVerification(*track, current_timestamp_ns);
  return true;
}

bool CwChannelBank::colorLeaseIsCurrent(
    const ColorLease& lease, const std::uint64_t timestamp_ns) const noexcept {
  if (!lease.occupied) return false;
  if (timestamp_ns < lease.last_seen_ns) return true;
  const long double age_seconds =
      static_cast<long double>(timestamp_ns - lease.last_seen_ns) /
      1'000'000'000.0L;
  return age_seconds <= config_.color_identity_retention_seconds;
}

void CwChannelBank::observePresentationFrequency(
    Track& track, const double candidate_frequency_hz,
    const std::uint64_t timestamp_ns) noexcept {
  if (!std::isfinite(candidate_frequency_hz)) return;
  if (track.last_presentation_evidence_ns > 0U &&
      timestamp_ns > track.last_presentation_evidence_ns &&
      timestamp_ns - track.last_presentation_evidence_ns > 750'000'000U) {
    track.presentation_frequency_evidence_count = 0;
    track.presentation_frequency_evidence_index = 0;
    track.presentation_follow_stable_since_ns = 0;
  }
  track.last_presentation_evidence_ns = timestamp_ns;
  track.presentation_frequency_evidence
      [track.presentation_frequency_evidence_index] = candidate_frequency_hz;
  track.presentation_frequency_evidence_index =
      (track.presentation_frequency_evidence_index + 1U) %
      Track::kPresentationEvidenceWindow;
  track.presentation_frequency_evidence_count =
      std::min(track.presentation_frequency_evidence_count + 1U,
               Track::kPresentationEvidenceWindow);
}

void CwChannelBank::reanchorPresentationOnFirstVerification(
    Track& track, const std::uint64_t timestamp_ns) noexcept {
  double target = track.frequency_hz;
  if (track.presentation_frequency_evidence_count >= 5U) {
    std::array<double, Track::kPresentationEvidenceWindow> values{};
    std::copy_n(track.presentation_frequency_evidence.begin(),
                track.presentation_frequency_evidence_count, values.begin());
    sortPrefix(values, track.presentation_frequency_evidence_count);
    const std::size_t middle = track.presentation_frequency_evidence_count / 2U;
    const double median = values[middle];
    std::array<double, Track::kPresentationEvidenceWindow> deviations{};
    for (std::size_t index = 0;
         index < track.presentation_frequency_evidence_count; ++index) {
      deviations[index] = std::abs(values[index] - median);
    }
    sortPrefix(deviations, track.presentation_frequency_evidence_count);
    // A broad/multimodal candidate history is not safe re-centering evidence.
    // Fall back to the adaptive DSP center, which is already innovation- and
    // identity-bounded, instead of choosing one of two adjacent carriers.
    if (deviations[middle] <= 8.0) target = median;
  }
  const double lower = track.identity_origin_frequency_hz -
                       config_.presentation_reanchor_limit_hz;
  const double upper = track.identity_origin_frequency_hz +
                       config_.presentation_reanchor_limit_hz;
  track.presentation_frequency_hz = std::clamp(target, lower, upper);
  track.presentation_follow_median_hz = track.presentation_frequency_hz;
  track.presentation_follow_stable_since_ns = timestamp_ns;
  track.last_presentation_follow_update_ns = timestamp_ns;
}

void CwChannelBank::followVerifiedPresentation(
    Track& track, const std::uint64_t timestamp_ns) noexcept {
  if (track.presentation_frequency_evidence_count < 12U ||
      track.narrowband_coherence < config_.minimum_narrowband_coherence ||
      std::abs(track.drift_hz_per_second) >
          config_.presentation_follow_maximum_drift_hz_per_second) {
    track.presentation_follow_stable_since_ns = 0;
    return;
  }

  std::array<double, Track::kPresentationEvidenceWindow> values{};
  std::copy_n(track.presentation_frequency_evidence.begin(),
              track.presentation_frequency_evidence_count, values.begin());
  std::sort(values.begin(),
            values.begin() + static_cast<std::ptrdiff_t>(
                                 track.presentation_frequency_evidence_count));
  const std::size_t middle = track.presentation_frequency_evidence_count / 2U;
  const double median = values[middle];
  std::array<double, Track::kPresentationEvidenceWindow> deviations{};
  for (std::size_t index = 0;
       index < track.presentation_frequency_evidence_count; ++index) {
    deviations[index] = std::abs(values[index] - median);
  }
  std::sort(
      deviations.begin(),
      deviations.begin() + static_cast<std::ptrdiff_t>(
                               track.presentation_frequency_evidence_count));
  if (deviations[middle] > config_.presentation_follow_maximum_mad_hz) {
    track.presentation_follow_stable_since_ns = 0;
    return;
  }

  const double movement = median - track.presentation_frequency_hz;
  if (std::abs(movement) <= config_.presentation_follow_deadband_hz) {
    track.presentation_follow_median_hz = median;
    track.presentation_follow_stable_since_ns = timestamp_ns;
    return;
  }
  const bool same_side =
      (movement > 0.0) ==
      (track.presentation_follow_median_hz - track.presentation_frequency_hz >
       0.0);
  if (!same_side || std::abs(median - track.presentation_follow_median_hz) >
                        config_.presentation_follow_deadband_hz) {
    track.presentation_follow_median_hz = median;
    track.presentation_follow_stable_since_ns = timestamp_ns;
    return;
  }
  track.presentation_follow_median_hz = median;
  if (track.presentation_follow_stable_since_ns == 0U ||
      timestamp_ns < track.presentation_follow_stable_since_ns) {
    track.presentation_follow_stable_since_ns = timestamp_ns;
    return;
  }
  const double stable_seconds =
      static_cast<double>(timestamp_ns -
                          track.presentation_follow_stable_since_ns) /
      1'000'000'000.0;
  if (stable_seconds < config_.presentation_follow_stable_seconds) return;

  const double lower = track.identity_origin_frequency_hz -
                       config_.presentation_reanchor_limit_hz;
  const double upper = track.identity_origin_frequency_hz +
                       config_.presentation_reanchor_limit_hz;
  const double target = std::clamp(median, lower, upper);
  const double elapsed_seconds = std::min(
      0.25, track.last_presentation_follow_update_ns > 0U &&
                    timestamp_ns >= track.last_presentation_follow_update_ns
                ? static_cast<double>(
                      timestamp_ns - track.last_presentation_follow_update_ns) /
                      1'000'000'000.0
                : 0.0);
  track.last_presentation_follow_update_ns = timestamp_ns;
  const double maximum_step =
      config_.presentation_follow_slew_hz_per_second * elapsed_seconds;
  track.presentation_frequency_hz += std::clamp(
      target - track.presentation_frequency_hz, -maximum_step, maximum_step);
  track.presentation_frequency_hz =
      std::clamp(track.presentation_frequency_hz, lower, upper);
}

void CwChannelBank::assignOrRefreshColor(
    Track& track, const std::uint64_t timestamp_ns) noexcept {
  if (track.color_assigned) {
    const bool earlier_verified_owner =
        std::any_of(tracks_.cbegin(), tracks_.cend(), [&](const Track& other) {
          return &other != &track && other.id < track.id &&
                 other.color_assigned &&
                 other.verification_state == CwTrackState::Verified &&
                 other.color_index == track.color_index;
        });
    if (earlier_verified_owner) {
      // Repair a collision created by an older build. The established lower
      // ID retains ownership; the later identity must select another color.
      track.color_assigned = false;
    } else {
      ColorLease& lease = color_leases_[track.color_index % kColorLeaseCount];
      // Keep the lease anchored to the frequency that established the visual
      // identity. A verified track can otherwise walk across nearby noise
      // while silent and move the lease away from the carrier it is meant to
      // remember. Known operator retunes move every lease explicitly.
      lease.last_seen_ns = std::max(lease.last_seen_ns, timestamp_ns);
      lease.occupied = true;
      return;
    }
  }

  std::array<bool, kColorLeaseCount> colors_in_use{};
  for (const Track& other : tracks_) {
    if (&other == &track || !other.color_assigned ||
        other.verification_state != CwTrackState::Verified)
      continue;
    colors_in_use[other.color_index % kColorLeaseCount] = true;
  }

  std::size_t selected = kColorLeaseCount;
  double nearest_distance = config_.color_identity_tolerance_hz;
  for (std::size_t index = 0; index < color_leases_.size(); ++index) {
    // A lease may identify a returning carrier only while no concurrently
    // published track owns its color.
    if (colors_in_use[index]) continue;
    const ColorLease& lease = color_leases_[index];
    if (!colorLeaseIsCurrent(lease, timestamp_ns)) continue;
    const double distance =
        std::abs(lease.frequency_hz - track.presentation_frequency_hz);
    if (distance <= nearest_distance) {
      nearest_distance = distance;
      selected = index;
    }
  }

  if (selected == kColorLeaseCount) {
    std::uint64_t oldest_seen = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < color_leases_.size(); ++index) {
      if (colors_in_use[index]) continue;
      const ColorLease& lease = color_leases_[index];
      if (!colorLeaseIsCurrent(lease, timestamp_ns)) {
        selected = index;
        break;
      }
      if (lease.last_seen_ns < oldest_seen) {
        oldest_seen = lease.last_seen_ns;
        selected = index;
      }
    }
  }

  // There is one lease per addressable track slot, so a free color is always
  // expected. Keep a deterministic fallback for defensive robustness.
  if (selected == kColorLeaseCount) {
    selected = static_cast<std::size_t>((track.id - 1U) % kColorLeaseCount);
  }
  track.color_index = static_cast<std::uint8_t>(selected);
  track.color_assigned = true;
  ColorLease& lease = color_leases_[selected];
  if (!colorLeaseIsCurrent(lease, timestamp_ns)) {
    // Freeze the lease at the robust verified carrier center. It does not
    // follow later presentation motion, but also does not inherit a biased
    // first unverified peak that may be far from the actual carrier.
    lease.frequency_hz = track.presentation_frequency_hz;
  }
  lease.last_seen_ns = timestamp_ns;
  lease.occupied = true;
}

void CwChannelBank::recoverRejectedDecoder(Track& track) {
  const bool quality_rejection =
      !track.ever_verified &&
      track.verification_state != CwTrackState::Verified &&
      track.verification_reason ==
          CwVerificationReason::ImplausibleCharacterDistribution &&
      track.update.recent_decoded_symbols >= 8U &&
      track.decoder.timingControlCadenceConfidence() >= 0.55F;
  if (!quality_rejection) {
    track.decoder_rejection_samples = 0;
    return;
  }

  const auto recovery_samples = static_cast<std::uint16_t>(std::clamp(
      std::lround(config_.decoder_recovery_seconds * config_.evidence_rate_hz),
      1L, static_cast<long>(std::numeric_limits<std::uint16_t>::max())));
  if (track.decoder_rejection_samples < recovery_samples)
    ++track.decoder_rejection_samples;
  if (track.decoder_rejection_samples < recovery_samples) return;

  // The carrier/noise filters remain valid; only timing/text state is
  // reacquired. This lets a real transmission take over a frequency that was
  // previously occupied by a persistent noise-derived timing hypothesis.
  // No verified text is rewritten because recovery is restricted to tracks
  // that have never reached verification.
  track.decoder.reset();
  track.update = {};
  track.decoder_rejection_samples = 0;
  track.verification_pass_samples = 0;
  track.verification_fail_samples = 0;
  track.verification_state = CwTrackState::Candidate;
  track.verification_reason = CwVerificationReason::NeedsKeyingEdges;
  ++decoder_reacquisitions_;
}

namespace {

// Speed shown to the operator. Below a few decoded symbols the timing bank has
// nothing to choose between its hypotheses: the value is still the seeded
// default, and whichever anchor briefly leads can be far from the truth. On a
// receiver capture a 27 WPM station read 20 WPM before anything was decoded
// and jumped to 40 WPM on its fourth key transition. Presenting nothing until
// the estimate is supported is honest, and the display already renders an
// absent speed as a dash.
constexpr std::uint32_t kMinimumSymbolsForPresentedSpeed = 3;

double presentedWpm(const CwDecoderUpdate& update) noexcept {
  return update.decoded_symbols >= kMinimumSymbolsForPresentedSpeed ? update.wpm
                                                                    : 0.0;
}

}  // namespace

std::vector<CwTrackDiagnostic> CwChannelBank::allTrackDiagnostics() const {
  std::vector<CwTrackDiagnostic> result;
  result.reserve(tracks_.size());
  for (const auto& track : tracks_) {
    const double match_age_seconds =
        expected_sample_timestamp_ns_ > track.last_candidate_match_ns
            ? static_cast<double>(expected_sample_timestamp_ns_ -
                                  track.last_candidate_match_ns) /
                  1'000'000'000.0
            : 0.0;
    result.push_back({
        .id = track.id,
        .frequency_hz = track.frequency_hz,
        .identity_origin_frequency_hz = track.identity_origin_frequency_hz,
        .presentation_frequency_hz = track.presentation_frequency_hz,
        .drift_hz_per_second = track.drift_hz_per_second,
        .snr_db = track.keying_envelope_initialized ? track.keying_mark_snr_db
                                                    : track.snr_db,
        .narrowband_coherence = track.narrowband_coherence,
        .filter_width_hz = kNarrowbandWidthsHz[track.selected_width_index],
        .verification_state = track.verification_state,
        .verification_reason = track.verification_reason,
        .spectral_observations = track.spectral_observations,
        .key_transitions = track.update.key_transitions,
        .decoded_symbols = track.update.decoded_symbols,
        .unknown_symbols = track.update.unknown_symbols,
        .timing_quality = track.update.timing_quality,
        .cadence_quality = track.update.cadence_quality,
        .mean_character_confidence = track.update.mean_character_confidence,
        .wpm = presentedWpm(track.update),
        .acoustic_wpm = track.update.acoustic_wpm,
        .acoustic_cadence_confidence = track.update.acoustic_cadence_confidence,
        .keying_level_separation_db =
            10.0F * std::log10(std::max(
                        track.keying_mark_power /
                            std::max(track.keying_space_power, 1.0e-12F),
                        1.0F)),
        .keying_level_explained_variation =
            track.keying_level_explained_variation,
        .robust_keying_level_anchor_active =
            track.robust_keying_level_anchor_active,
        .keyer_speed_ratio = track.keyer_speed_ratio,
        .keyer_overlap_unresolved = track.keyer_overlap_unresolved,
        .overlap_neighbour_count = track.overlap_neighbour_count,
        .nearest_neighbour_separation_hz =
            track.nearest_neighbour_separation_hz,
        .joint_separation_candidate =
            track.keyer_overlap_unresolved &&
            track.overlap_neighbour_count + 1U <=
                kJointSeparationMaximumSources,
        .text = suppressFragmentRuns(track.update.text),
        .refined_text = suppressFragmentRuns(track.update.refined_text),
        .acoustic_alternatives = track.update.acoustic_alternatives,
        .provisional_text = track.update.provisional_text,
        .match_age_seconds = match_age_seconds,
        .color_index = track.color_index,
        .matched = track.matched,
        .active = match_age_seconds <= kCandidateMatchHoldSeconds,
        .key_down = !track.decoder_input_suspended && track.update.key_down,
        .operator_selected = track.operator_selected,
    });
  }
  return result;
}

const std::vector<CwCharacterTrackSnapshot>&
CwChannelBank::characterRefinementTracks() const noexcept {
  return character_refinement_tracks_;
}

void CwChannelBank::rebuildSnapshots(const std::uint64_t timestamp_ns) {
  character_refinement_tracks_.clear();
  character_refinement_tracks_.reserve(tracks_.size());
  for (const auto& track : tracks_) {
    const double match_age_seconds =
        timestamp_ns > track.last_candidate_match_ns
            ? static_cast<double>(timestamp_ns -
                                  track.last_candidate_match_ns) /
                  1'000'000'000.0
            : 0.0;
    character_refinement_tracks_.push_back({
        .id = track.id,
        .frequency_hz = track.frequency_hz,
        .presentation_frequency_hz = track.presentation_frequency_hz,
        .snr_db = track.keying_envelope_initialized ? track.keying_mark_snr_db
                                                    : track.snr_db,
        .verification_state = track.verification_state,
        .active = match_age_seconds <= kCandidateMatchHoldSeconds,
        .operator_selected = track.operator_selected,
    });
  }
  for (RetainedObservation& observation : retained_observations_)
    observation.refreshed = false;

  std::vector<std::uint64_t> verified_track_ids;
  verified_track_ids.reserve(tracks_.size());
  for (const Track& track : tracks_) {
    if (track.verification_state == CwTrackState::Verified)
      verified_track_ids.push_back(track.id);
  }

  std::vector<CwChannelSnapshot> selected_unverified;
  selected_unverified.reserve(tracks_.size());
  for (const auto& track : tracks_) {
    if (track.operator_selected && !track.ever_verified &&
        track.verification_state != CwTrackState::Verified) {
      const bool recently_matched =
          timestamp_ns < track.last_candidate_match_ns ||
          static_cast<long double>(timestamp_ns -
                                   track.last_candidate_match_ns) /
                  1'000'000'000.0L <=
              kCandidateMatchHoldSeconds;
      selected_unverified.push_back(CwChannelSnapshot{
          .id = track.id,
          .color_index = 0,
          .frequency_hz = track.frequency_hz,
          .presentation_frequency_hz = track.presentation_frequency_hz,
          .drift_hz_per_second = track.drift_hz_per_second,
          .filter_width_hz = kNarrowbandWidthsHz[track.selected_width_index],
          .snr_db = track.keying_envelope_initialized ? track.keying_mark_snr_db
                                                      : track.snr_db,
          .wpm = presentedWpm(track.update),
          .acoustic_wpm = track.update.acoustic_wpm,
          .acoustic_cadence_confidence =
              track.update.acoustic_cadence_confidence,
          .confidence = track.update.confidence,
          .key_down_probability = track.matched && recently_matched
                                      ? track.update.key_down_probability
                                      : 0.0F,
          .key_down =
              track.matched && recently_matched && track.update.key_down,
          .active = track.matched && recently_matched,
          .verified_cw = false,
          .operator_selected = true,
          .verification_state = track.verification_state,
          .verification_reason = track.verification_reason,
          .verification_confidence = track.verification_confidence,
          .verification_cadence_quality = track.verification_cadence_quality,
          .verification_timing_quality = track.verification_timing_quality,
          .verification_character_confidence =
              track.verification_character_confidence,
          .cadence_quality = track.update.cadence_quality,
          .mean_character_confidence = track.update.mean_character_confidence,
          .narrowband_coherence = track.narrowband_coherence,
          .key_transitions = track.update.key_transitions,
          .characters = {},
          .text = {},
          .refined_text = {},
          .acoustic_alternatives = {},
          .provisional_text = {},
          .pending_elements = {},
          .transmissions = {},
          .sender_cadences = {},
          .active_transmission_sequence = 0,
          .current_sender_callsign = {},
          .current_sender_wpm = 0.0,
          .contextual_text = {},
          .callsign = {},
          .qso_participants = {},
      });
    }
    if (track.verification_state != CwTrackState::Verified) continue;
    const bool recently_matched =
        timestamp_ns < track.last_candidate_match_ns ||
        static_cast<long double>(timestamp_ns - track.last_candidate_match_ns) /
                1'000'000'000.0L <=
            kCandidateMatchHoldSeconds;
    // What this channel holds is a real signal the operator can see and tune
    // to; what the decoder made of it is the sum of several keyers, which is
    // not Morse. Publish the one and withhold the other. Everything that makes
    // the track visible -- identity, colour, frequency, level, key state --
    // survives untouched, and only the text and the station name it would have
    // carried are held back.
    const bool withhold_text = track.keyer_overlap_unresolved;
    std::string callsign;
    // Reconcile the independent literal and append-only refined paths. Shared
    // evidence settles disagreement; refinement remains usable when the
    // literal path's timing gate fails, but degluing a prosign cannot create a
    // stream label without an exact refined token.
    const std::string_view primary_for_callsign =
        !withhold_text && track.update.timing_quality >=
                              config_.minimum_verification_timing_quality
            ? std::string_view(track.update.text)
            : std::string_view{};
    callsign = withhold_text
                   ? std::string{}
                   : CallsignPolicy::best_complete_in_parallel_texts(
                         primary_for_callsign, track.update.refined_text,
                         operator_role_, config_.own_callsign)
                         .value_or(std::string{});
    // Never label a stream with the operator's own callsign. It appears in
    // received text whenever somebody calls the operator, and a caller that
    // sends it repeatedly without ever completing its own would otherwise take
    // the label. Suppressing it leaves the stream unlabelled until the calling
    // station identifies, which is the honest answer.
    if (!config_.own_callsign.empty() && !callsign.empty()) {
      const auto own = CallsignPolicy::normalize(config_.own_callsign);
      if (own && *own == callsign) callsign.clear();
    }
    // A callsign opens with a prefix the ITU has allocated to some
    // administration. A decoded token whose opening characters fall in no
    // allocation names a country that does not exist, which is far better
    // evidence that the decode is wrong than that a rare station was heard --
    // one missed element turns a real prefix into an unallocated one, and that
    // is the shape of most of the wrong labels an operator sees. Refusing it
    // leaves the stream unlabelled until the station identifies cleanly, the
    // same honest answer as suppressing the operator's own callsign above.
    //
    // The table errs towards admitting: an empty or unreadable dictionary
    // admits everything, because refusing every station on the band would be a
    // far larger fault than the misdecodes this catches.
    if (!callsign.empty() &&
        !cwSharedCallsignPrefixes().isAllocatedPrefix(callsign)) {
      callsign.clear();
    }
    CwChannelSnapshot snapshot{
        .id = track.id,
        .color_index = track.color_index,
        .frequency_hz = track.frequency_hz,
        .presentation_frequency_hz = track.presentation_frequency_hz,
        .drift_hz_per_second = track.drift_hz_per_second,
        .filter_width_hz = kNarrowbandWidthsHz[track.selected_width_index],
        .snr_db = track.keying_envelope_initialized ? track.keying_mark_snr_db
                                                    : track.snr_db,
        .wpm = presentedWpm(track.update),
        .acoustic_wpm = track.update.acoustic_wpm,
        .acoustic_cadence_confidence = track.update.acoustic_cadence_confidence,
        .confidence = track.update.confidence,
        .key_down_probability =
            recently_matched ? track.update.key_down_probability : 0.0F,
        .key_down = recently_matched && track.update.key_down,
        // Retention preserves identity and text, not live carrier state.
        // Residual energy/noise at a remembered frequency must not keep the
        // marker filled or generate CW-symbol rows without a current peak.
        .active = recently_matched,
        .verified_cw = true,
        .operator_selected = track.operator_selected,
        .verification_state = track.verification_state,
        .verification_reason = track.verification_reason,
        .verification_confidence = track.verification_confidence,
        .verification_cadence_quality = track.verification_cadence_quality,
        .verification_timing_quality = track.verification_timing_quality,
        .verification_character_confidence =
            track.verification_character_confidence,
        .cadence_quality = track.update.cadence_quality,
        .mean_character_confidence = track.update.mean_character_confidence,
        .narrowband_coherence = track.narrowband_coherence,
        .key_transitions = track.update.key_transitions,
        .characters = withhold_text ? std::vector<CwCharacterEvidence>{}
                                    : track.update.characters,
        .text = withhold_text ? std::string{}
                              : suppressFragmentRuns(track.update.text),
        .refined_text =
            withhold_text ? std::string{}
                          : suppressFragmentRuns(track.update.refined_text),
        .acoustic_alternatives = withhold_text
                                     ? std::vector<CwAcousticAlternative>{}
                                     : track.update.acoustic_alternatives,
        .provisional_text =
            withhold_text ? std::string{} : track.update.provisional_text,
        .pending_elements =
            withhold_text ? std::string{} : track.update.pending_elements,
        .transmissions = withhold_text ? std::vector<CwTransmissionTurn>{}
                                       : track.update.transmissions,
        .sender_cadences = withhold_text ? std::vector<CwSenderCadence>{}
                                         : track.update.sender_cadences,
        .active_transmission_sequence =
            withhold_text ? 0U : track.update.active_transmission_sequence,
        .current_sender_callsign =
            withhold_text ? std::string{}
                          : track.update.current_sender_callsign,
        .current_sender_wpm =
            withhold_text ? 0.0 : track.update.current_sender_wpm,
        .contextual_text =
            withhold_text ? std::string{} : track.update.contextual_text,
        .callsign = callsign,
        .qso_participants =
            withhold_text
                ? std::vector<std::string>{}
                : allocatedParticipants(
                      CallsignPolicy::qso_participants_in_text(
                          track.update.text)),
    };

    auto retained = std::find_if(
        retained_observations_.begin(), retained_observations_.end(),
        [&](const RetainedObservation& observation) {
          return observation.source_track_id == track.id;
        });
    bool replacement = false;
    if (retained == retained_observations_.end()) {
      double nearest_distance = config_.color_identity_tolerance_hz;
      for (auto candidate = retained_observations_.begin();
           candidate != retained_observations_.end(); ++candidate) {
        const bool predecessor_still_published =
            std::find(verified_track_ids.cbegin(), verified_track_ids.cend(),
                      candidate->source_track_id) != verified_track_ids.cend();
        if (candidate->refreshed || predecessor_still_published ||
            candidate->snapshot.color_index != track.color_index)
          continue;
        const double distance =
            std::abs(candidate->snapshot.presentation_frequency_hz -
                     track.presentation_frequency_hz);
        if (distance <= nearest_distance) {
          nearest_distance = distance;
          retained = candidate;
          replacement = true;
        }
      }
    }
    if (retained == retained_observations_.end()) {
      // One remembered region per addressable track slot. Sized by the track
      // capacity rather than by the color-lease count, which happens to be the
      // same number: what bounds this list is how many distinct streams can
      // have existed, not how many colors there are to draw them in.
      if (retained_observations_.size() >= kCwMaximumTrackCapacity) {
        retained = std::min_element(
            retained_observations_.begin(), retained_observations_.end(),
            [](const RetainedObservation& left,
               const RetainedObservation& right) {
              if (left.refreshed != right.refreshed) return !left.refreshed;
              return left.last_seen_ns < right.last_seen_ns;
            });
      } else {
        retained_observations_.push_back({});
        retained = std::prev(retained_observations_.end());
      }
      retained->source_track_id = track.id;
      retained->inherited_text_prefix.clear();
      retained->label = {};
      retained->confirmed_qso_participants.clear();
    } else if (replacement) {
      // A replacement tracker at the same retained identity starts with a
      // fresh timing decoder, but the operator's session is continuous.
      // Freeze the predecessor once as an explicit prefix. Subsequent
      // refreshes replace only the new source suffix and cannot duplicate it.
      retained->inherited_text_prefix = retained->snapshot.text;
      trimPresentationText(retained->inherited_text_prefix);
      retained->source_track_id = track.id;
      // A color/frequency lease preserves visual session continuity, not
      // acoustic station identity. The replacement decoder must establish a
      // callsign from its own suffix; otherwise a different station appearing
      // on the same frequency inherits the predecessor's label indefinitely.
      // The corroboration behind the old name goes with it: a successor must
      // not have to out-argue evidence that was never about it.
      retained->label = {};
    }
    // Hysteresis, not a fresh verdict every frame. The reading above is
    // recomputed from whatever text the window holds at this instant, so on
    // its own it hands the stream's name to any momentary misdecode and takes
    // it away again a frame later. A real capture read EH3ST eight times in a
    // minute and EH3S, EH3SN, EHSMST and EG7S once each, and the operator saw
    // the name change to each of them in turn. Folding the reading into the
    // name already earned keeps the majority verdict and makes a rival pay the
    // same price the incumbent paid.
    retained->label = cwApplyStreamLabelReading(
        std::move(retained->label), snapshot.callsign, primary_for_callsign,
        track.update.refined_text);
    // A withheld track keeps what it had earned but stops presenting it. The
    // evidence is remembered rather than destroyed, so a station that is
    // buried by a pileup and later heard clear again returns under its own
    // name instead of as a new stream; but while it cannot be resolved, the
    // name and the frozen predecessor text are exactly the parts an operator
    // would act on, and neither is supported by what the channel is carrying
    // now. A contested name is withheld for a separate reason: the channel is
    // resolvable and its text is worth reading, but the two names it has
    // offered disagree and neither is worth acting on.
    snapshot.callsign = withhold_text || retained->label.contested
                            ? std::string{}
                            : retained->label.callsign;
    if (withhold_text) {
      snapshot.qso_participants.clear();
    } else if (snapshot.qso_participants.empty()) {
      snapshot.qso_participants = retained->confirmed_qso_participants;
    } else {
      retained->confirmed_qso_participants = snapshot.qso_participants;
    }
    snapshot.text =
        withhold_text ? std::string{}
                      : composePresentationText(
                            retained->inherited_text_prefix, snapshot.text);
    retained->snapshot = std::move(snapshot);
    retained->last_seen_ns = track.last_detected_ns;
    retained->refreshed = true;
  }

  const auto observation_expired = [&](const RetainedObservation& observation) {
    if (timestamp_ns < observation.last_seen_ns) return false;
    const long double age_seconds =
        static_cast<long double>(timestamp_ns - observation.last_seen_ns) /
        1'000'000'000.0L;
    // A stream that decoded and named its station is worth remembering longer
    // than one that never said who it was: the operator is still looking for
    // it, and the region carries a label they can act on. An anonymous or
    // poorly copied stream leaves nothing to come back to, so it keeps the
    // standard time.
    //
    // The identified test reads the latched callsign rather than the live
    // verification state, because verification decays during the silence this
    // rule exists to survive -- by the time expiry matters no track is still
    // verified, and a present-state rule would grant the longer hold to
    // nobody.
    const double retention =
        observation.label.callsign.empty()
            ? config_.decoded_track_retention_seconds
            : config_.decoded_track_retention_seconds *
                  config_.identified_stream_retention_multiplier;
    return age_seconds > static_cast<long double>(retention);
  };
  std::erase_if(retained_observations_, observation_expired);

  snapshots_.clear();
  snapshots_.reserve(retained_observations_.size() +
                     selected_unverified.size());
  for (RetainedObservation& observation : retained_observations_) {
    if (!observation.refreshed) {
      observation.snapshot.active = false;
      observation.snapshot.key_down = false;
      observation.snapshot.key_down_probability = 0.0F;
      observation.snapshot.verification_state = CwTrackState::Lost;
      observation.snapshot.verification_reason =
          CwVerificationReason::SignalLost;
    }
    snapshots_.push_back(observation.snapshot);
  }
  snapshots_.insert(snapshots_.end(), selected_unverified.cbegin(),
                    selected_unverified.cend());
  std::sort(snapshots_.begin(), snapshots_.end(),
            [](const CwChannelSnapshot& left, const CwChannelSnapshot& right) {
              return left.frequency_hz < right.frequency_hz;
            });
}

}  // namespace cwassistant::core
