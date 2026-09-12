#include "cwassistant/core/cw_decoder.hpp"
#include "cwassistant/core/cw_morse_alphabet.hpp"
#include "cwassistant/core/cw_speed_anchors.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/cw_acoustic_refinement.hpp"
#include "cwassistant/core/cw_context_rescorer.hpp"

namespace cwassistant::core {
namespace {

std::string_view decode_elements(const std::string_view value) {
  // The alphabet is data, loaded once at startup; see cw_morse_alphabet.hpp.
  // An unknown pattern is retained uncertainty, not a guess.
  const auto symbol = cwSharedMorseAlphabet().symbolFor(value);
  return symbol.empty() ? std::string_view{"?"} : symbol;
}

double milliseconds(const std::uint64_t value) {
  return static_cast<double>(value) / 1'000'000.0;
}

std::size_t characterEvidenceBytes(
    const std::vector<CwCharacterEvidence>& characters) noexcept {
  std::size_t result = characters.capacity() * sizeof(CwCharacterEvidence);
  for (const auto& character : characters)
    result += character.symbol.capacity();
  return result;
}

std::size_t updateDynamicBytes(const CwDecoderUpdate& update) noexcept {
  std::size_t result = update.text.capacity() +
                       update.provisional_text.capacity() +
                       update.pending_elements.capacity() +
                       update.refined_text.capacity() +
                       update.current_sender_callsign.capacity() +
                       update.contextual_text.capacity() +
                       characterEvidenceBytes(update.characters) +
                       update.acoustic_alternatives.capacity() *
                           sizeof(CwAcousticAlternative);
  for (const auto& alternative : update.acoustic_alternatives) {
    result += alternative.text.capacity() +
              alternative.provisional_elements.capacity();
  }
  result += update.transmissions.capacity() * sizeof(CwTransmissionTurn);
  for (const auto& transmission : update.transmissions)
    result += transmission.text.capacity() +
              transmission.sender_callsign.capacity();
  result += update.sender_cadences.capacity() * sizeof(CwSenderCadence);
  for (const auto& cadence : update.sender_cadences)
    result += cadence.callsign.capacity();
  return result;
}

constexpr std::size_t kRecentCharacterWindow = 32;
constexpr std::size_t kRecentCadenceWindow = 64;

}  // namespace

CwTimingDecoder::CwTimingDecoder(CwDecoderConfig config) : config_(config) {
  config_.initial_wpm = std::clamp(config_.initial_wpm, 5.0, 80.0);
  config_.key_off_snr_db = std::min(config_.key_off_snr_db,
                                    config_.key_on_snr_db);
  config_.key_on_probability =
      std::clamp(config_.key_on_probability, 0.51F, 0.95F);
  config_.key_off_probability = std::clamp(
      config_.key_off_probability, 0.05F, config_.key_on_probability - 0.05F);
  config_.evidence_time_constant_ms =
      std::clamp(config_.evidence_time_constant_ms, 1.0, 100.0);
  config_.character_gap_dots =
      std::clamp(config_.character_gap_dots, 1.6, 2.8);
  config_.stable_gap_dots = std::clamp(
      config_.stable_gap_dots, config_.character_gap_dots + 0.2, 4.5);
  config_.word_gap_dots =
      std::clamp(config_.word_gap_dots, config_.stable_gap_dots + 0.5, 9.0);
  anchor_dot_ms_ = 1'200.0 / config_.initial_wpm;
  // Each speed hypothesis segments at its own element length, so each sizes
  // its own search window: this is what keeps nine anchors per track from
  // costing nine times the slowest one. A model that is not selected is never
  // constructed, so it costs nothing.
  switch (config_.keying_model) {
    case CwKeyingModel::SemiMarkov: {
      CwSemiMarkovConfig segment_config;
      segment_config.initial_wpm = config_.initial_wpm;
      segmenter_ = std::make_unique<CwSemiMarkovSegmenter>(segment_config);
      break;
    }
    case CwKeyingModel::AdaptiveThreshold:
      break;
  }
  reset();
}

void CwTimingDecoder::reset() noexcept {
  stable_text_.clear(); provisional_text_.clear(); elements_.clear();
  dot_ms_ = 1'200.0 / config_.initial_wpm;
  state_started_ns_ = 0; last_timestamp_ns_ = 0;
  previous_state_started_ns_ = 0; has_previous_state_ = false;
  pending_pair_mark_dots_ = 0.0; pending_pair_mark_duration_ms_ = 0.0;
  pending_pair_confidence_ = 0.0F; pending_pair_valid_ = false;
  last_snr_db_ = 0.0F; key_down_probability_ = 0.0F;
  confidence_ = 0.0F; element_confidence_sum_ = 0.0F;
  timing_confidence_sum_ = 0.0F;
  mark_probability_sum_ = 0.0F; mark_probability_duration_ms_ = 0.0;
  decoded_symbol_count_ = 0;
  unknown_symbol_count_ = 0;
  key_transition_count_ = 0;
  cadence_observation_count_ = 0;
  element_count_ = 0;
  characters_.clear();
  recent_cadence_quality_.clear();
  provisional_character_ = {};
  if (segmenter_ != nullptr) {
    segmenter_->reset();
    segmenter_->setElementLengthMs(dot_ms_);
  }
  committed_segments_.clear();
  cached_update_ = {};
  initialized_ = false; key_down_ = false; character_finished_ = false;
  word_space_emitted_ = false;
}

float CwTimingDecoder::snrForSegment(const CwSegment& segment) const noexcept {
  const float midpoint =
      0.5F * (config_.key_on_snr_db + config_.key_off_snr_db);
  const float scale = std::max(
      0.75F, 0.5F * (config_.key_on_snr_db - config_.key_off_snr_db));
  // Honest evidence, with no floor under it. The key state is carried by the
  // sign alone, so a run is never lost for being weak -- which frees this
  // magnitude to say how weak it actually was. Flooring it instead, so that it
  // also had to clear the old hysteresis band, told the verification gate that
  // every run was a confident one: a duration-explicit search fits legal Morse
  // to noise perfectly well, and confidence in the evidence is the thing that
  // separates a signal from a channel that merely has structure imposed on it.
  const float magnitude = static_cast<float>(
      std::clamp(std::abs(segment.element_evidence_nats), 0.05, 3.0));
  return midpoint + (isMarkSegment(segment.kind) ? magnitude : -magnitude) *
                        scale;
}

const CwDecoderUpdate& CwTimingDecoder::process(
    const std::uint64_t timestamp_ns, const float snr_db) {
  // The threshold model has no segmenter in front of it: the frame goes
  // straight into the per-frame path, exactly as it always has.
  if (segmenter_ == nullptr) return processFrame(timestamp_ns, snr_db);
  last_snr_db_ = snr_db;
  // Bounded to the neighbourhood of this hypothesis's own anchor. The
  // downstream speed estimate is derived from this segmentation, so feeding it
  // back unbounded closes a loop on itself: one long run biases the estimate
  // slow, a slower model then prefers longer runs still, and the hypothesis
  // walks away from the signal and stops segmenting it at all. Measured, that
  // cost an entire transmission on one seed in five -- 132 key transitions
  // became 18. The bank already covers speed with nine anchors; each one only
  // has to cover the ground between itself and its neighbours.
  segmenter_->setElementLengthMs(
      std::clamp(dot_ms_, anchor_dot_ms_ * 0.80, anchor_dot_ms_ * 1.25));
  const float midpoint =
      0.5F * (config_.key_on_snr_db + config_.key_off_snr_db);
  const float scale = std::max(
      0.75F, 0.5F * (config_.key_on_snr_db - config_.key_off_snr_db));
  // Exact inverse of probabilityForSnr: the channel bank already encoded a
  // calibrated log-likelihood ratio into this figure, and the segmenter wants
  // it back in nats rather than a second squash of an already shaped ramp.
  const double log_likelihood_nats =
      static_cast<double>((snr_db - midpoint) / scale);
  bool changed = false;
  if (last_input_timestamp_ns_ != 0 &&
      timestamp_ns > last_input_timestamp_ns_) {
    const double interval_ms =
        milliseconds(timestamp_ns - last_input_timestamp_ns_);
    if (interval_ms > 0.0 && interval_ms < 200.0) {
      input_frame_ms_ = input_frame_ms_ <= 0.0
          ? interval_ms
          : input_frame_ms_ + 0.05 * (interval_ms - input_frame_ms_);
    }
  }
  last_input_timestamp_ns_ = timestamp_ns;
  committed_segments_ = segmenter_->process(timestamp_ns,
                                            log_likelihood_nats);
  changed = replayCommittedSegments();
  return snapshot(changed);
}

bool CwTimingDecoder::replayCommittedSegments() {
  // Replayed at the rate the frames arrived on. Handing a run over as a single
  // step would leave every measure that integrates across it -- the mark
  // confidence the verification gate reads above all -- accumulating once at
  // the instant the state had already moved on to the next run. The end of a
  // transmission goes through here too: the last character of a call is the
  // one an operator most needs, and it is the one a shortcut here loses.
  const auto step_ns = static_cast<std::uint64_t>(
      std::max(1.0, input_frame_ms_ * 1.0e6));
  bool changed = false;
  for (const CwSegment& segment : committed_segments_) {
    const float segment_snr_db = snrForSegment(segment);
    for (std::uint64_t at = segment.started_ns; at < segment.ended_ns;
         at += step_ns) {
      const auto& update = processFrame(at, segment_snr_db);
      changed = changed || update.changed;
    }
  }
  return changed;
}

const CwDecoderUpdate& CwTimingDecoder::processFrame(
    const std::uint64_t timestamp_ns, const float snr_db) {
  last_snr_db_ = snr_db;
  const float instantaneous_probability = probabilityForSnr(snr_db);
  if (!initialized_) {
    initialized_ = true;
    key_down_probability_ = instantaneous_probability;
    key_down_ = key_down_probability_ >= config_.key_on_probability;
    state_started_ns_ = timestamp_ns;
    last_timestamp_ns_ = timestamp_ns;
    return snapshot(false);
  }
  if (timestamp_ns < state_started_ns_ || timestamp_ns < last_timestamp_ns_) {
    reset();
    initialized_ = true;
    key_down_probability_ = instantaneous_probability;
    key_down_ = key_down_probability_ >= config_.key_on_probability;
    state_started_ns_ = timestamp_ns;
    last_timestamp_ns_ = timestamp_ns;
    return snapshot(true);
  }

  const double elapsed_ms = milliseconds(timestamp_ns - last_timestamp_ns_);
  // Scale evidence smoothing to the element length this hypothesis is
  // tracking. A single fixed constant cannot serve a 7.5:1 speed range: at
  // 12 ms it is 8% of a dit at 8 WPM (so receiver noise passes into the
  // decision) and 60% of a dit at 60 WPM (so short marks are destroyed).
  // A quarter of the element length measured best across the WPM x SNR
  // surface: wide enough to integrate receiver noise, narrow enough to keep
  // a 60 WPM dit resolvable.
  const double evidence_time_constant_ms = std::clamp(
      dot_ms_ / 4.0, 2.0, config_.evidence_time_constant_ms * 2.0);
  // Nothing undecided reaches here any more. The segmenter has already
  // integrated this run's evidence over its whole length, so re-applying an
  // evidence smoother would only re-introduce the lag it exists to fight --
  // and on a run near the minimum length it would delay the crossing past the
  // end of the run and lose it, which is precisely the weak-signal failure
  // this decoder replaced.
  // A segmenter has already weighed this run's evidence over its whole length,
  // so re-applying an evidence smoother would only re-introduce the lag it
  // exists to fight, and on a run near the minimum length would delay the
  // crossing past the end of the run and lose it. Undecided input still needs
  // it.
  const float smoothing = segmenter_ != nullptr
      ? 1.0F
      : static_cast<float>(std::clamp(
            elapsed_ms / evidence_time_constant_ms, 0.0, 1.0));
  key_down_probability_ +=
      smoothing * (instantaneous_probability - key_down_probability_);
  if (key_down_ && elapsed_ms > 0.0) {
    mark_probability_sum_ +=
        key_down_probability_ * static_cast<float>(elapsed_ms);
    mark_probability_duration_ms_ += elapsed_ms;
  }
  last_timestamp_ns_ = timestamp_ns;

  // The segmenter decided this, having weighed the whole run; the sign is that
  // decision. A hysteresis band re-applied here would only be a second, worse
  // decision taken on one frame at a time -- and it is exactly what used to
  // drop weak marks.
  const bool observed_down = segmenter_ != nullptr
      ? snr_db >= 0.5F * (config_.key_on_snr_db + config_.key_off_snr_db)
      : (key_down_ ? key_down_probability_ >= config_.key_off_probability
                   : key_down_probability_ >= config_.key_on_probability);
  // The smoothed probability crosses its threshold roughly one time constant
  // after the signal actually changed. Both edges of a completed run shift by
  // the same amount, so measured run lengths are already correct and must not
  // be adjusted: the time constant tracks dot_ms_, so compensating a run's two
  // ends with two different delays would feed the estimate back into itself.
  // Only a still-open gap is affected, because it is timed against the wall
  // clock while its start edge was detected late.
  // With no smoother in front of the decision there is no crossing delay to
  // compensate for; a gap still open is timed exactly from where it began.
  const double detection_delay_ms =
      segmenter_ != nullptr ? 0.0 : 1.1 * evidence_time_constant_ms;
  bool changed = false;
  if (observed_down != key_down_) {
    const double duration_ms = milliseconds(timestamp_ns - state_started_ns_);
    // Impulsive noise produces very short excursions. Rejecting them by
    // duration keeps the amplitude decision free to sit exactly at the edge,
    // instead of using a wide hysteresis band that mistimes every real
    // element in order to survive impulses.
    // A quarter of an element measured best across the accuracy surface and
    // the hard-negative corpus together: it raises timing quality on real CW
    // (0.98) while lowering it on irregularly keyed noise (0.44), which is
    // exactly the separation the verification gate needs.
    // On a weak signal the envelope does not glitch once, it chatters: a
    // measured 17% of marks fragment at 12 dB and 90% at 6 dB. Retaining the
    // preceding run's start lets a whole burst collapse back into it, instead
    // of only the first excursion being absorbed and every later one being
    // published as a spurious element.
    if (has_previous_state_ && duration_ms < 0.25 * dot_ms_) {
      key_down_ = observed_down;
      state_started_ns_ = previous_state_started_ns_;
      return snapshot(false);
    }
    if (key_transition_count_ < std::numeric_limits<std::uint32_t>::max())
      ++key_transition_count_;
    if (key_down_) {
      finishElement(duration_ms); changed = true;
    } else {
      if (pending_pair_valid_) {
        // Only an intra-character gap pairs with the preceding mark; a
        // character or word gap is not part of the weighted element pair.
        if (duration_ms < config_.character_gap_dots * dot_ms_) {
          const double estimate =
              (pending_pair_mark_duration_ms_ + duration_ms) /
              (pending_pair_mark_dots_ + 1.0);
          if (estimate >= 15.0 && estimate <= 240.0 &&
              pending_pair_confidence_ >= 0.35F) {
            // Damped relative to the mark-only estimate it replaces: the
            // pair sums two noisy measurements, so it trades a little
            // convergence speed for the variance that addition introduces.
            const double adaptation =
                0.7 * (0.08 + 0.14 * pending_pair_confidence_);
            dot_ms_ += adaptation * (estimate - dot_ms_);
          }
        }
        pending_pair_valid_ = false;
      }
      if (!elements_.empty() || character_finished_ ||
          decoded_symbol_count_ > 0) {
        const double ratio = duration_ms / dot_ms_;
        const double distance = !elements_.empty()
            ? std::abs(ratio - 1.0)
            : std::min(std::abs(ratio - 3.0) / 1.5,
                       std::abs(ratio - 7.0) / 3.0);
        const float cadence_quality = static_cast<float>(
            std::exp(-1.2 * std::min(distance, 8.0)));
        if (recent_cadence_quality_.size() >= kRecentCadenceWindow) {
          recent_cadence_quality_.erase(recent_cadence_quality_.begin());
        }
        recent_cadence_quality_.push_back(cadence_quality);
        if (cadence_observation_count_ <
            std::numeric_limits<std::uint32_t>::max()) {
          ++cadence_observation_count_;
        }
      }
      if (!character_finished_ &&
          duration_ms >= config_.character_gap_dots * dot_ms_) {
        finishCharacter(); changed = true;
      }
      if (!provisional_text_.empty()) {
        promoteProvisional();
        changed = true;
      }
      if (duration_ms >= config_.word_gap_dots * dot_ms_ &&
          !word_space_emitted_ && !stable_text_.empty()) {
        if (stable_text_.back() != ' ') stable_text_.push_back(' ');
        word_space_emitted_ = true; changed = true;
      }
    }
    previous_state_started_ns_ = state_started_ns_;
    has_previous_state_ = true;
    key_down_ = observed_down;
    state_started_ns_ = timestamp_ns;
    if (key_down_) {
      mark_probability_sum_ = 0.0F;
      mark_probability_duration_ms_ = 0.0;
      character_finished_ = false;
      word_space_emitted_ = false;
    }
  } else if (!key_down_) {
    // The gap actually began one detection delay before it was observed.
    const double duration_ms =
        milliseconds(timestamp_ns - state_started_ns_) + detection_delay_ms;
    if (!character_finished_ && !elements_.empty() &&
        duration_ms >= config_.character_gap_dots * dot_ms_) {
      finishCharacter(); changed = true;
    }
    if (character_finished_ && !provisional_text_.empty() &&
        duration_ms >= config_.stable_gap_dots * dot_ms_) {
      promoteProvisional(); changed = true;
    }
    if (character_finished_ && !word_space_emitted_ && !stable_text_.empty() &&
        duration_ms >= config_.word_gap_dots * dot_ms_) {
      if (stable_text_.back() != ' ') stable_text_.push_back(' ');
      word_space_emitted_ = true; changed = true;
    }
  }
  return snapshot(changed);
}

const CwDecoderUpdate& CwTimingDecoder::flush(
    const std::uint64_t timestamp_ns) {
  if (segmenter_ != nullptr) {
    committed_segments_ = segmenter_->flush();
    static_cast<void>(replayCommittedSegments());
  }
  bool changed = processFrame(timestamp_ns, -100.0F).changed;
  if (key_down_) {
    // Flush is an explicit end-of-input boundary, so it must release a keyed
    // state even when called only one evidence interval after the last update
    // and the probability smoother has not naturally crossed key-off yet.
    const double duration_ms = timestamp_ns >= state_started_ns_
        ? milliseconds(timestamp_ns - state_started_ns_)
        : 0.0;
    if (key_transition_count_ < std::numeric_limits<std::uint32_t>::max())
      ++key_transition_count_;
    finishElement(duration_ms);
    key_down_ = false;
    key_down_probability_ = 0.0F;
    state_started_ns_ = timestamp_ns;
    last_timestamp_ns_ = timestamp_ns;
    changed = true;
  }
  if (!elements_.empty()) { finishCharacter(); changed = true; }
  if (!provisional_text_.empty()) {
    promoteProvisional();
    changed = true;
  }
  return snapshot(changed);
}

std::size_t CwTimingDecoder::stateBytes() const noexcept {
  return sizeof(*this) +
         (segmenter_ != nullptr ? segmenter_->stateBytes() : 0) +
         stable_text_.capacity() +
         provisional_text_.capacity() + elements_.capacity() +
         characterEvidenceBytes(characters_) +
         recent_cadence_quality_.capacity() * sizeof(float) +
         provisional_character_.symbol.capacity() +
         updateDynamicBytes(cached_update_);
}

void CwTimingDecoder::finishElement(const double duration_ms) {
  const double ratio = duration_ms / dot_ms_;
  const double dot_distance = std::abs(ratio - 1.0);
  const double dash_distance = std::abs(ratio - 3.0) / 1.5;
  const bool dash = dash_distance < dot_distance;
  elements_.push_back(dash ? '-' : '.');
  if (elements_.size() > 9) elements_.erase(0, elements_.size() - 9);

  const float mark_confidence = mark_probability_duration_ms_ > 0.0
      ? std::clamp(mark_probability_sum_ /
                       static_cast<float>(mark_probability_duration_ms_),
                   0.0F, 1.0F)
      : key_down_probability_;
  const float timing_confidence = static_cast<float>(std::exp(
      -1.25 * std::min(dot_distance, dash_distance)));
  element_confidence_sum_ += mark_confidence * timing_confidence;
  timing_confidence_sum_ += timing_confidence;
  if (element_count_ < 255) ++element_count_;

  // Defer the element-length adaptation until the following gap closes. A
  // mark measured alone carries the operator's keying weight, which is why
  // lightly weighted and bug-style sending previously decoded far worse than
  // machine timing; the mark plus its following element gap is independent of
  // that weight.
  pending_pair_mark_dots_ = dash ? 3.0 : 1.0;
  pending_pair_mark_duration_ms_ = duration_ms;
  pending_pair_confidence_ = mark_confidence * timing_confidence;
  pending_pair_valid_ = true;
  character_finished_ = false;
}

void CwTimingDecoder::finishCharacter() {
  if (elements_.empty()) return;
  provisional_text_ = std::string(decode_elements(elements_));
  confidence_ = element_count_ == 0
      ? 0.0F
      : std::clamp(element_confidence_sum_ /
                       static_cast<float>(element_count_),
                   0.0F, 1.0F);
  // Pure duration-ratio precision, independent of confidence_'s blended
  // amplitude/keying-probability component: two tracks with identical
  // cadence precision but different SNR must not report identical
  // "timing quality" just because they report identical character
  // confidence -- that collapses two lines of verification evidence into
  // one (see BACKLOG.md CW-001's known-defect note).
  float character_timing_quality = element_count_ == 0
      ? 0.0F
      : std::clamp(timing_confidence_sum_ /
                       static_cast<float>(element_count_),
                   0.0F, 1.0F);
  if (provisional_text_ == "?") {
    confidence_ *= 0.35F;
    character_timing_quality *= 0.35F;
  }
  if (decoded_symbol_count_ < std::numeric_limits<std::uint32_t>::max())
    ++decoded_symbol_count_;
  if (provisional_text_ == "?" &&
      unknown_symbol_count_ < std::numeric_limits<std::uint32_t>::max())
    ++unknown_symbol_count_;
  provisional_character_ = {
      .symbol = provisional_text_,
      .confidence = confidence_,
      .timing_quality = character_timing_quality,
      .known = provisional_text_ != "?",
  };
  elements_.clear(); character_finished_ = true;
  element_confidence_sum_ = 0.0F;
  timing_confidence_sum_ = 0.0F;
  element_count_ = 0;
}


void CwTimingDecoder::promoteProvisional() {
  if (provisional_text_.empty()) return;
  stable_text_ += provisional_text_;
  if (characters_.size() >= 256) characters_.erase(characters_.begin());
  characters_.push_back(provisional_character_);
  if (stable_text_.size() > 4'096)
    stable_text_.erase(0, stable_text_.size() - 4'096);
  provisional_text_.clear();
  provisional_character_ = {};
}

namespace {

float evidenceForRatio(const CwDecoderConfig& config,
                       const float log_likelihood_ratio) noexcept {
  const float bound = cwEvidenceBoundNats(config.keying_model);
  const float midpoint = 0.5F * (config.key_on_snr_db + config.key_off_snr_db);
  const float scale = std::max(
      0.75F, 0.5F * (config.key_on_snr_db - config.key_off_snr_db));
  // Saturate symmetrically about the midpoint, but far enough out to leave a
  // strong sample its strength. The old bound sat at three nats, where a
  // per-frame posterior is already within a few percent of certain, because a
  // decoder thresholding one frame at a time gains nothing past that and is
  // only made unstable by it. A decoder that integrates evidence across a
  // whole run does gain: the difference between a frame that is merely
  // probably keyed and one that certainly is, is most of what tells a real
  // element from a noise excursion once thirty of them are added up.
  const float bounded = std::clamp(log_likelihood_ratio, -bound, bound);
  return midpoint + scale * bounded;
}

}  // namespace

float CwTimingDecoder::evidenceForLogLikelihoodRatio(
    const float log_likelihood_ratio) const noexcept {
  return evidenceForRatio(config_, log_likelihood_ratio);
}

float CwMultiSpeedDecoder::evidenceForLogLikelihoodRatio(
    const float log_likelihood_ratio) const noexcept {
  return evidenceForRatio(decoder_config_, log_likelihood_ratio);
}

float CwTimingDecoder::probabilityForSnr(const float snr_db) const noexcept {
  const float midpoint =
      0.5F * (config_.key_on_snr_db + config_.key_off_snr_db);
  const float scale = std::max(
      0.75F, 0.5F * (config_.key_on_snr_db - config_.key_off_snr_db));
  const float normalized = std::clamp((snr_db - midpoint) / scale,
                                      -20.0F, 20.0F);
  return 1.0F / (1.0F + std::exp(-normalized));
}

const CwDecoderUpdate& CwTimingDecoder::snapshot(const bool changed) {
  const std::size_t character_count = std::min(
      characters_.size(), kRecentCharacterWindow);
  const auto character_begin = characters_.end() -
      static_cast<std::ptrdiff_t>(character_count);
  float recent_timing_sum = 0.0F;
  float recent_confidence_sum = 0.0F;
  std::uint32_t recent_unknown = 0;
  for (auto character = character_begin; character != characters_.end();
       ++character) {
    recent_timing_sum += character->timing_quality;
    recent_confidence_sum += character->confidence;
    if (!character->known) ++recent_unknown;
  }
  // Include the current provisional character in the live quality metrics.
  // It is not counted as recent decoded evidence until it is promoted.
  const bool has_provisional = !provisional_character_.symbol.empty();
  const float quality_count = static_cast<float>(
      character_count + (has_provisional ? 1U : 0U));
  if (has_provisional) {
    recent_timing_sum += provisional_character_.timing_quality;
    recent_confidence_sum += provisional_character_.confidence;
  }
  const float timing_quality = quality_count == 0.0F
      ? 0.0F : recent_timing_sum / quality_count;
  const float mean_character_confidence = quality_count == 0.0F
      ? 0.0F : recent_confidence_sum / quality_count;
  float recent_cadence_sum = 0.0F;
  for (const float quality : recent_cadence_quality_)
    recent_cadence_sum += quality;
  const float cadence_quality = recent_cadence_quality_.empty()
      ? 0.0F
      : recent_cadence_sum /
            static_cast<float>(recent_cadence_quality_.size());
  cached_update_.changed = changed;
  cached_update_.key_down = key_down_;
  cached_update_.key_down_probability = key_down_probability_;
  cached_update_.wpm = 1'200.0 / dot_ms_;
  cached_update_.confidence = confidence_;
  cached_update_.timing_quality = timing_quality;
  cached_update_.cadence_quality = cadence_quality;
  cached_update_.mean_character_confidence = mean_character_confidence;
  cached_update_.decoded_symbols = decoded_symbol_count_;
  cached_update_.unknown_symbols = unknown_symbol_count_;
  cached_update_.key_transitions = key_transition_count_;
  cached_update_.cadence_observations = cadence_observation_count_;
  cached_update_.recent_decoded_symbols =
      static_cast<std::uint32_t>(character_count);
  cached_update_.recent_unknown_symbols = recent_unknown;
  cached_update_.recent_cadence_observations =
      static_cast<std::uint32_t>(recent_cadence_quality_.size());

  // These fields can only change at a decoded element/character/gap boundary,
  // all of which set `changed`. The common frame therefore updates only the
  // scalar evidence above and performs no dynamic allocation or deep copy.
  if (changed) {
    cached_update_.text = stable_text_;
    cached_update_.provisional_text = provisional_text_;
    cached_update_.pending_elements = elements_;
    cached_update_.characters = characters_;
  }
  return cached_update_;
}

CwMultiSpeedDecoder::Hypothesis::Hypothesis(
    const double speed_wpm, CwDecoderConfig config)
    : seed_wpm(speed_wpm),
      decoder([&] {
        config.initial_wpm = speed_wpm;
        return config;
      }()) {}

CwMultiSpeedDecoder::CwMultiSpeedDecoder(
    CwDecoderConfig decoder_config, CwMultiSpeedConfig config)
    : decoder_config_(decoder_config), config_(config) {
  config_.preferred_wpm = std::clamp(config_.preferred_wpm, 5.0, 80.0);
  config_.minimum_acquisition_ms =
      std::clamp(config_.minimum_acquisition_ms, 100.0, 5'000.0);
  config_.reacquire_after_silence_ms =
      std::clamp(config_.reacquire_after_silence_ms, 500.0, 15'000.0);
  config_.lock_after_symbols =
      std::clamp<std::uint8_t>(config_.lock_after_symbols, 1, 8);
  config_.lock_score_margin =
      std::clamp(config_.lock_score_margin, 0.0F, 1.0F);
  config_.lattice_checkpoint_ms = std::clamp(
      std::isfinite(config_.lattice_checkpoint_ms)
          ? config_.lattice_checkpoint_ms : 500.0,
      100.0, 2'000.0);
  config_.lattice_fixed_lag_ms = std::clamp(
      std::isfinite(config_.lattice_fixed_lag_ms)
          ? config_.lattice_fixed_lag_ms : 1'000.0,
      250.0, 5'000.0);
  config_.lattice_fixed_lag_observations = std::clamp<std::size_t>(
      config_.lattice_fixed_lag_observations, 2U, 32U);
  config_.lattice_competitive_cost_margin = std::clamp(
      std::isfinite(config_.lattice_competitive_cost_margin)
          ? config_.lattice_competitive_cost_margin : 1.0,
      0.10, 5.0);
  config_.minimum_lattice_evidence_confidence = std::clamp(
      std::isfinite(config_.minimum_lattice_evidence_confidence)
          ? config_.minimum_lattice_evidence_confidence : 0.40F,
      0.20F, 0.90F);
  reset();
}

void CwMultiSpeedDecoder::setKeyingModel(const CwKeyingModel model) {
  if (decoder_config_.keying_model == model) return;
  decoder_config_.keying_model = model;
  reset();
}

void CwMultiSpeedDecoder::reset() {
  committed_prefix_.clear();
  refined_text_.clear();
  transmissions_.clear();
  sender_cadences_.clear();
  next_transmission_sequence_ = 1;
  active_transmission_sequence_ = 1;
  transmission_primary_starts_.fill(0U);
  transmission_refined_start_ = 0;
  current_sender_callsign_.clear();
  current_sender_wpm_ = 0.0;
  active_transmission_completed_ = false;
  pending_turn_timing_fingerprint_.reset();
  lattice_committed_observation_id_ = 0;
  resetLatticeSegment();
  resetHypotheses();
}

void CwMultiSpeedDecoder::resetHypotheses() {
  hypotheses_.clear();
  hypotheses_.reserve(kCwSeedWpmAnchors.size());
  for (const double speed : kCwSeedWpmAnchors)
    hypotheses_.emplace_back(speed, decoder_config_);
  leader_index_ = cwSeedWpmAnchorIndex(kCwInitialLeaderWpm);
  locked_index_ = 0;
  first_timestamp_ns_ = 0;
  last_signal_timestamp_ns_ = 0;
  locked_ = false;
  initialized_ = false;
  signal_seen_ = false;
  recent_mark_count_ = 0;
  recent_gap_count_ = 0;
  recent_mark_index_ = 0;
  recent_gap_index_ = 0;
  recent_mark_gap_count_ = 0;
  recent_mark_gap_index_ = 0;
  pending_cadence_mark_ms_ = 0.0;
  pending_cadence_mark_ = false;
  cadence_state_started_ns_ = 0;
  cadence_dot_ms_ = 0.0;
  cadence_confidence_ = 0.0F;
  lattice_cadence_dot_ms_ = 0.0;
  lattice_cadence_confidence_ = 0.0F;
  cadence_initialized_ = false;
  last_observed_segment_ns_ = 0;
  cadence_key_down_ = false;
  transmission_primary_starts_.fill(0U);
  current_sender_callsign_.clear();
  current_sender_wpm_ = 0.0;
  active_transmission_completed_ = false;
  pending_turn_timing_fingerprint_.reset();
}

CwDecoderUpdate CwMultiSpeedDecoder::process(
    const std::uint64_t timestamp_ns, const float snr_db) {
  if (snr_db >= decoder_config_.key_off_snr_db) {
    last_signal_timestamp_ns_ = timestamp_ns;
    signal_seen_ = true;
  }
  if (!initialized_ && snr_db >= decoder_config_.key_on_snr_db) {
    first_timestamp_ns_ = timestamp_ns;
    initialized_ = true;
  }
  if (locked_) {
    bool changed = false;
    for (auto& hypothesis : hypotheses_) {
      const auto& update = hypothesis.decoder.process(timestamp_ns, snr_db);
      changed = changed || update.changed;
    }
    // Each hypothesis now smooths evidence against its own element length,
    // so their key decisions genuinely differ. Cadence and lattice evidence
    // must follow the current presentation leader rather than the first
    // seeded hypothesis, which is the slowest one in the bank.
    observeLeaderEvidence(timestamp_ns);
    const auto& selected = hypotheses_[locked_index_];
    leader_index_ = locked_index_;
    if (changed) updateCurrentSender();
    const double silence_ms = signal_seen_ &&
            timestamp_ns >= last_signal_timestamp_ns_
        ? milliseconds(timestamp_ns - last_signal_timestamp_ns_)
        : 0.0;
    const auto& selected_update = selected.decoder.currentUpdate();
    if (!selected_update.key_down && selected_update.decoded_symbols > 0 &&
        silence_ms >= config_.reacquire_after_silence_ms) {
      // All fixed speed anchors continue observing after the initial
      // selection. At a safe segment boundary, commit the best complete
      // hypothesis rather than making the early WPM choice irreversible.
      const std::size_t final_leader = selectLeader();
      refreshLattice(CwLatticeDecodeMode::Flush);
      commitCompletedTransmission(final_leader);
      return snapshot(true);
    }
    return snapshot(changed);
  }

  bool changed = false;
  for (auto& hypothesis : hypotheses_) {
    const auto& update = hypothesis.decoder.process(timestamp_ns, snr_db);
    changed = changed || update.changed;
  }
  // Each hypothesis now smooths evidence against its own element length,
  // so their key decisions genuinely differ. Cadence and lattice evidence
  // must follow the current presentation leader rather than the first
  // seeded hypothesis, which is the slowest one in the bank.
  observeLeaderEvidence(timestamp_ns);
  const std::size_t previous_leader = leader_index_;
  float margin = 0.0F;
  leader_index_ = selectLeader(&margin);
  if (changed || leader_index_ != previous_leader) updateCurrentSender();
  changed = changed || leader_index_ != previous_leader;
  const double observed_ms = timestamp_ns >= first_timestamp_ns_
      ? milliseconds(timestamp_ns - first_timestamp_ns_)
      : 0.0;
  if (observed_ms >= config_.minimum_acquisition_ms) considerLock(margin);
  return snapshot(changed || locked_);
}

CwDecoderUpdate CwMultiSpeedDecoder::suspendInput(
    const std::uint64_t timestamp_ns) {
  for (auto& hypothesis : hypotheses_)
    static_cast<void>(hypothesis.decoder.flush(timestamp_ns));
  leader_index_ = selectLeader();
  locked_index_ = leader_index_;
  locked_ = true;
  observeLattice(false, 0.0F, timestamp_ns);
  // Association can disappear during an ordinary word gap. Keep the bounded
  // lattice and its ambiguous suffix provisional until reacquisition proves a
  // semantic turn boundary; forcing MAP here made a brief detector dropout an
  // irreversible decoding decision.
  refreshLattice(CwLatticeDecodeMode::Provisional);
  updateCurrentSender();
  return snapshot(true);
}

CwDecoderUpdate CwMultiSpeedDecoder::resumeInput(
    const std::uint64_t timestamp_ns) {
  const double silence_ms = signal_seen_ &&
          timestamp_ns >= last_signal_timestamp_ns_
      ? milliseconds(timestamp_ns - last_signal_timestamp_ns_)
      : 0.0;
  const std::size_t final_leader = selectLeader();
  if (signal_seen_ &&
      hypotheses_[final_leader].decoder.currentUpdate().decoded_symbols > 0U &&
      silence_ms >= config_.reacquire_after_silence_ms) {
    // The acoustic state was already drained when association disappeared.
    // Only the independently measured sustained absence makes it a semantic
    // operator-turn boundary.
    refreshLattice(CwLatticeDecodeMode::Flush);
    if (!refined_text_.empty() && refined_text_.back() != ' ')
      refined_text_.push_back(' ');
    completeTransmission(final_leader);
    resetLatticeSegment();
    beginNextTransmissionWithoutAcousticReset();
    return snapshot(true);
  }
  updateCurrentSender();
  return snapshot(false);
}

CwDecoderUpdate CwMultiSpeedDecoder::flush(
    const std::uint64_t timestamp_ns) {
  static_cast<void>(suspendInput(timestamp_ns));
  const std::size_t final_leader = selectLeader();
  refreshLattice(CwLatticeDecodeMode::Flush);
  if (!refined_text_.empty() && refined_text_.back() != ' ')
    refined_text_.push_back(' ');
  completeTransmission(final_leader);
  return snapshot(true);
}

std::size_t CwMultiSpeedDecoder::hypothesisCount() const noexcept {
  return hypotheses_.size();
}

std::size_t CwMultiSpeedDecoder::stateBytes() const noexcept {
  std::size_t result = sizeof(*this) +
      hypotheses_.capacity() * sizeof(Hypothesis) +
      committed_prefix_.capacity() + refined_text_.capacity() +
      current_sender_callsign_.capacity() +
      contextual_lattice_text_.capacity() +
      acoustic_alternatives_.capacity() * sizeof(CwAcousticAlternative) +
      transmissions_.capacity() * sizeof(CwTransmissionTurn) +
      sender_cadences_.capacity() * sizeof(CwSenderCadence) +
      event_lattice_.stateBytes() - sizeof(CwEventLattice);
  for (const auto& alternative : acoustic_alternatives_) {
    result += alternative.text.capacity() +
              alternative.provisional_elements.capacity();
  }
  for (const auto& transmission : transmissions_)
    result += transmission.text.capacity() +
              transmission.sender_callsign.capacity();
  for (const auto& cadence : sender_cadences_)
    result += cadence.callsign.capacity();
  for (const auto& hypothesis : hypotheses_) {
    result += hypothesis.decoder.stateBytes() - sizeof(CwTimingDecoder);
  }
  return result;
}

float CwMultiSpeedDecoder::score(
    const Hypothesis& hypothesis) const noexcept {
  const auto& update = hypothesis.decoder.currentUpdate();
  const float prior_distance = static_cast<float>(std::abs(
      std::log2(hypothesis.seed_wpm / config_.preferred_wpm)));
  if (update.decoded_symbols == 0)
    return -0.20F * prior_distance;
  const std::uint32_t recent_symbols = update.recent_decoded_symbols > 0
      ? update.recent_decoded_symbols : update.decoded_symbols;
  const std::uint32_t recent_unknown = update.recent_decoded_symbols > 0
      ? update.recent_unknown_symbols : update.unknown_symbols;
  const float known_fraction = 1.0F -
      static_cast<float>(std::min(recent_unknown, recent_symbols)) /
          static_cast<float>(std::max<std::uint32_t>(recent_symbols, 1U));
  // Hypothesis selection deliberately scores on mean_character_confidence
  // (the blended amplitude/keying-probability and timing signal), not the
  // now-independent pure-cadence timing_quality: this preserves the
  // original, already-tuned hypothesis-selection behavior, since
  // timing_quality's separation (see BACKLOG.md CW-001) was made only to
  // stop it duplicating mean_character_confidence for the verification
  // gate, not to change which WPM hypothesis wins here.
  // Both terms below exist only because a duration-explicit model can be
  // confidently wrong about speed in a way a threshold cannot, so they are
  // applied only to a hypothesis that uses one. Adding them to the threshold
  // model perturbs a speed acquisition that was already correct.
  float segmentation_evidence = 0.0F;
  if (hypothesis.decoder.usesSegmenter()) {
    // What it cost this hypothesis to describe the signal at its own element
    // length. Text plausibility cannot separate speed hypotheses when each
    // segments explicitly -- every one of them yields legal Morse -- so the
    // likelihood of the segmentation itself is evidence about the speed.
    const float duration_fit = static_cast<float>(
        std::clamp(hypothesis.decoder.segmentationScore(), -12.0, 0.0));
    // Morse timing is self-similar at a factor of three: read three times too
    // fast, every dash becomes a dot and every character gap becomes a gap
    // between elements. The result is legal, entirely composed of known
    // symbols, and fits its own duration model perfectly, so neither the text
    // nor the fit can reject it -- a hypothesis three times too fast produced
    // "E ?TMT MMTM MTMT" from a clean CQ and was preferred to the truth. What
    // gives it away is that every element became a character of its own. Real
    // Morse averages around three elements per character, and no natural text
    // approaches one.
    const float elements_per_symbol =
        0.5F * static_cast<float>(update.key_transitions) /
        static_cast<float>(
            std::max<std::uint32_t>(update.decoded_symbols, 1U));
    const float fragmentation =
        std::clamp(2.2F - elements_per_symbol, 0.0F, 2.0F);
    segmentation_evidence = duration_fit - 0.60F * fragmentation;
  }
  return 2.5F * update.mean_character_confidence + 0.4F * known_fraction -
         0.15F * (1.0F - update.confidence) -
         0.20F * prior_distance + segmentation_evidence;
}

void CwMultiSpeedDecoder::observeLeaderEvidence(
    const std::uint64_t timestamp_ns) {
  const auto& leader = hypotheses_[leader_index_];
  if (!leader.decoder.usesSegmenter()) {
    // The threshold model publishes its key state per frame and nothing else,
    // so cadence and the lattice read it the way they always have.
    const auto& update = leader.decoder.currentUpdate();
    observeCadence(update.key_down, timestamp_ns);
    observeLattice(update.key_down, update.key_down_probability,
                   timestamp_ns);
    return;
  }
  // A segmenter dates each run to when it happened rather than when it was
  // noticed, and may commit several at once. Sampling its key state once per
  // incoming frame would mis-time every run and drop all but the last. The
  // nine anchors also do not reach the same instant together, so a change of
  // leader can offer a run older than the one already seen: observations are
  // taken strictly forwards, and a new leader resumes where the stream had
  // reached.
  for (const CwSegment& segment : leader.decoder.committedSegments()) {
    if (segment.started_ns <= last_observed_segment_ns_) continue;
    last_observed_segment_ns_ = segment.started_ns;
    const bool keyed = isMarkSegment(segment.kind);
    observeCadence(keyed, segment.started_ns);
    observeLattice(keyed, segment.confidence, segment.started_ns);
  }
}

void CwMultiSpeedDecoder::observeCadence(const bool key_down,
                                         const std::uint64_t timestamp_ns) {
  if (!cadence_initialized_) {
    cadence_initialized_ = true;
    cadence_key_down_ = key_down;
    cadence_state_started_ns_ = timestamp_ns;
    return;
  }
  if (timestamp_ns < cadence_state_started_ns_) {
    cadence_initialized_ = false;
    cadence_confidence_ = 0.0F;
    return;
  }
  if (key_down == cadence_key_down_) return;

  const double duration_ms = milliseconds(timestamp_ns -
                                           cadence_state_started_ns_);
  if (duration_ms >= 10.0 && duration_ms <= 2'000.0) {
    if (cadence_key_down_) {
      recent_mark_ms_[recent_mark_index_] = duration_ms;
      recent_mark_index_ =
          (recent_mark_index_ + 1U) % kCadenceDurationWindow;
      recent_mark_count_ = std::min(recent_mark_count_ + 1U,
                                    kCadenceDurationWindow);
      pending_cadence_mark_ms_ = duration_ms;
      pending_cadence_mark_ = true;
    } else {
      recent_gap_ms_[recent_gap_index_] = duration_ms;
      recent_gap_index_ =
          (recent_gap_index_ + 1U) % kCadenceDurationWindow;
      recent_gap_count_ = std::min(recent_gap_count_ + 1U,
                                   kCadenceDurationWindow);
      if (config_.paired_cadence_fit && pending_cadence_mark_) {
        recent_mark_gap_ms_[recent_mark_gap_index_] =
            pending_cadence_mark_ms_ + duration_ms;
        recent_mark_gap_index_ =
            (recent_mark_gap_index_ + 1U) % kCadenceDurationWindow;
        recent_mark_gap_count_ = std::min(recent_mark_gap_count_ + 1U,
                                          kCadenceDurationWindow);
      }
      pending_cadence_mark_ = false;
    }
    recomputeCadenceEstimate();
  } else {
    // A dropped/implausibly long run breaks adjacency. Never let the next
    // valid gap combine with a mark from the other side of that missing edge.
    pending_cadence_mark_ms_ = 0.0;
    pending_cadence_mark_ = false;
  }
  cadence_key_down_ = key_down;
  cadence_state_started_ns_ = timestamp_ns;
}

void CwMultiSpeedDecoder::recomputeCadenceEstimate() {
  const std::size_t observation_count = recent_mark_count_ +
                                        recent_gap_count_;
  if (recent_mark_count_ < 3U || observation_count < 6U) {
    cadence_confidence_ = 0.0F;
    return;
  }

  std::array<double, kCadenceDurationWindow * 5U> candidates{};
  std::size_t candidate_count = 0;
  const auto add_candidate = [&](const double value) {
    if (value >= 15.0 && value <= 240.0 &&
        candidate_count < candidates.size()) {
      candidates[candidate_count++] = value;
    }
  };
  for (std::size_t index = 0; index < recent_mark_count_; ++index) {
    add_candidate(recent_mark_ms_[index]);
    add_candidate(recent_mark_ms_[index] / 3.0);
  }
  for (std::size_t index = 0; index < recent_gap_count_; ++index) {
    add_candidate(recent_gap_ms_[index]);
    add_candidate(recent_gap_ms_[index] / 3.0);
    add_candidate(recent_gap_ms_[index] / 7.0);
  }
  if (candidate_count == 0) return;

  const auto mark_residual = [](const double duration,
                                const double dot) {
    const double ratio = duration / dot;
    return std::min(std::abs(ratio - 1.0) / 0.40,
                    std::abs(ratio - 3.0) / 0.85);
  };
  const auto gap_residual = [](const double duration,
                               const double dot) {
    const double ratio = duration / dot;
    return std::min({std::abs(ratio - 1.0) / 0.45,
                     std::abs(ratio - 3.0) / 1.0,
                     std::abs(ratio - 7.0) / 2.2});
  };
  const auto pair_residual = [](const double duration,
                                const double dot) {
    const double ratio = duration / dot;
    // Unique totals for mark {1,3} followed by gap {1,3,7}. Tolerance grows
    // mildly with duration because manual timing variance is proportional,
    // while the clipped contribution keeps one long pause bounded.
    static constexpr double totals[]{2.0, 4.0, 6.0, 8.0, 10.0};
    double residual = std::numeric_limits<double>::max();
    for (const double total : totals) {
      residual = std::min(
          residual, std::abs(ratio - total) / (0.35 + 0.12 * total));
    }
    return residual;
  };

  double best_dot = candidates[0];
  double best_cost = std::numeric_limits<double>::max();
  double best_individual_dot = candidates[0];
  double best_individual_cost = std::numeric_limits<double>::max();
  for (std::size_t candidate = 0; candidate < candidate_count; ++candidate) {
    const double dot = candidates[candidate];
    double cost = 0.0;
    for (std::size_t index = 0; index < recent_mark_count_; ++index) {
      cost += std::min(mark_residual(recent_mark_ms_[index], dot), 1.0);
    }
    for (std::size_t index = 0; index < recent_gap_count_; ++index) {
      cost += std::min(gap_residual(recent_gap_ms_[index], dot), 1.0);
    }
    // Clipping every observation bounds the influence of key clicks and
    // missed edges without sorting inside the per-track real-time path.
    cost /= static_cast<double>(observation_count);
    const double prior_cost = 0.015 * std::abs(std::log2(dot / 60.0));
    const double individual_cost = cost + prior_cost;
    if (individual_cost < best_individual_cost) {
      best_individual_cost = individual_cost;
      best_individual_dot = dot;
    }
    if (config_.paired_cadence_fit && recent_mark_gap_count_ >= 3U) {
      double paired_cost = 0.0;
      for (std::size_t index = 0; index < recent_mark_gap_count_; ++index) {
        paired_cost += std::min(
            pair_residual(recent_mark_gap_ms_[index], dot), 1.0);
      }
      paired_cost /= static_cast<double>(recent_mark_gap_count_);
      // Keep independent mark/gap evidence dominant. Pair evidence corrects
      // the known manual-weighting bias but cannot create a confident cadence
      // when the individual runs do not resemble Morse at all.
      cost = 0.65 * cost + 0.35 * paired_cost;
    }
    // Only resolve otherwise-near ties toward the normal operating range.
    // The measured ratios, not this weak prior, remain decisive.
    cost += prior_cost;
    if (cost < best_cost) {
      best_cost = cost;
      best_dot = dot;
    }
  }

  const float coverage = std::min(
      1.0F, static_cast<float>(observation_count) / 18.0F);
  const float fit = static_cast<float>(std::clamp(1.0 - best_cost,
                                                  0.0, 1.0));
  const float confidence = coverage * fit;
  const float individual_fit = static_cast<float>(std::clamp(
      1.0 - best_individual_cost, 0.0, 1.0));
  const float individual_confidence = coverage * individual_fit;
  const auto update_estimate = [](const double measured_dot,
                                  const float measured_confidence,
                                  double* dot, float* confidence_value) {
    if (*dot <= 0.0 || *confidence_value < 0.25F) {
      *dot = measured_dot;
    } else if (measured_dot / *dot >= 0.55 &&
               measured_dot / *dot <= 1.8) {
      *dot += 0.25 * (measured_dot - *dot);
    } else if (measured_confidence >= 0.75F) {
      *dot = measured_dot;
    }
    *confidence_value = measured_confidence;
  };
  update_estimate(best_dot, confidence, &cadence_dot_ms_,
                  &cadence_confidence_);
  update_estimate(best_individual_dot, individual_confidence,
                  &lattice_cadence_dot_ms_, &lattice_cadence_confidence_);
}

std::size_t CwMultiSpeedDecoder::selectLeader(float* margin) const {
  std::size_t best_index = 0;
  float best_score = score(hypotheses_.front());
  float second_score = -1'000.0F;
  for (std::size_t index = 1; index < hypotheses_.size(); ++index) {
    const float candidate_score = score(hypotheses_[index]);
    if (candidate_score > best_score) {
      second_score = best_score;
      best_score = candidate_score;
      best_index = index;
    } else {
      second_score = std::max(second_score, candidate_score);
    }
  }
  if (margin != nullptr) *margin = best_score - second_score;
  return best_index;
}

CwDecoderUpdate CwMultiSpeedDecoder::snapshot(const bool changed) const {
  CwDecoderUpdate result = hypotheses_[leader_index_].decoder.currentUpdate();
  result.changed = changed;
  // The independent cadence estimate is withheld until its own confidence
  // supports it, for the same reason the timing bank's speed is: on thin
  // evidence it reaches values far from the truth. Measured on a receiver
  // capture, it read 40.9 WPM for a 27 WPM station while fewer than twenty key
  // transitions had been seen. It is an independent check on the speed, so a
  // wrong value is worse than none.
  constexpr float kMinimumCadenceConfidence = 0.45F;
  result.acoustic_wpm =
      cadence_dot_ms_ > 0.0 && cadence_confidence_ >= kMinimumCadenceConfidence
          ? 1'200.0 / cadence_dot_ms_ : 0.0;
  result.acoustic_cadence_confidence = cadence_confidence_;
  result.transmissions = transmissions_;
  result.sender_cadences = sender_cadences_;
  result.active_transmission_sequence = active_transmission_sequence_;
  result.current_sender_callsign = current_sender_callsign_;
  result.current_sender_wpm = current_sender_wpm_;
  bool active_text_present = false;
  for (const auto& transmission : transmissions_) {
    if (!result.contextual_text.empty()) result.contextual_text += '\n';
    result.contextual_text += transmission.text;
  }
  // Before lock, provisional_text owns the leader's complete uncommitted
  // candidate. Publishing that same candidate here as contextual stable text
  // would make a two-stage presentation append it twice. Once locked, only
  // the genuinely stable active prefix belongs in contextual_text.
  if (locked_ && !active_transmission_completed_ && !hypotheses_.empty()) {
    const auto& primary = hypotheses_[leader_index_].decoder.currentUpdate().text;
    const std::size_t primary_start = std::min(
        transmission_primary_starts_[leader_index_], primary.size());
    std::string active = transmission_refined_start_ < refined_text_.size()
        ? refined_text_.substr(transmission_refined_start_)
        : primary.substr(primary_start);
    if (active != word_gap_cache_input_) {
      word_gap_cache_input_ = active;
      word_gap_cache_output_ = reconstructCwWordGaps(active);
    }
    active = word_gap_cache_output_;
    if (!active.empty()) {
      if (!result.contextual_text.empty()) result.contextual_text += '\n';
      result.contextual_text += active;
      active_text_present = true;
    }
  }
  result.refined_text = refined_text_;
  result.acoustic_alternatives = acoustic_alternatives_;
  if (!locked_) {
    result.provisional_text = result.text + result.provisional_text;
    result.text = committed_prefix_;
  } else {
    result.text = committed_prefix_ + result.text;
  }
  // A newly keyed turn first exists only in provisional_text. Reserve its
  // line at that same snapshot so a presentation can append the provisional
  // suffix without momentarily joining it to the preceding transmission.
  // Once the character becomes stable, the active text replaces this empty
  // line with the same visible content instead of forcing a corrective reflow.
  if (!active_transmission_completed_ && !active_text_present &&
      !transmissions_.empty() && !result.provisional_text.empty() &&
      !result.contextual_text.empty() && result.contextual_text.back() != '\n') {
    result.contextual_text.push_back('\n');
  }
  return result;
}

const CwSenderCadence* CwMultiSpeedDecoder::senderCadence(
    const std::string_view sender) const noexcept {
  const auto found = std::find_if(
      sender_cadences_.begin(), sender_cadences_.end(),
      [sender](const CwSenderCadence& cadence) {
        return cadence.callsign == sender;
      });
  return found == sender_cadences_.end() ? nullptr : &*found;
}

void CwMultiSpeedDecoder::rememberSenderCadence(
    const std::string_view sender, const double wpm, const float confidence) {
  if (sender.empty() || !std::isfinite(wpm) || wpm < 5.0 || wpm > 80.0 ||
      confidence < 0.35F) {
    return;
  }
  auto found = std::find_if(
      sender_cadences_.begin(), sender_cadences_.end(),
      [sender](const CwSenderCadence& cadence) {
        return cadence.callsign == sender;
      });
  if (found == sender_cadences_.end()) {
    if (sender_cadences_.size() >= kMaximumSenderCadences)
      sender_cadences_.erase(sender_cadences_.begin());
    sender_cadences_.push_back({.callsign = std::string(sender),
                                .wpm = wpm,
                                .confidence = confidence,
                                .observed_turns = 1});
    return;
  }
  const double retained_weight = std::min(found->observed_turns, 4U) *
                                 std::max(0.35F, found->confidence);
  const double new_weight = std::max(0.35F, confidence);
  found->wpm = (found->wpm * retained_weight + wpm * new_weight) /
               (retained_weight + new_weight);
  found->confidence = static_cast<float>(std::clamp(
      (found->confidence * retained_weight + confidence * new_weight) /
          (retained_weight + new_weight),
      0.0, 1.0));
  if (found->observed_turns < std::numeric_limits<std::uint32_t>::max())
    ++found->observed_turns;
}

void CwMultiSpeedDecoder::updateCurrentSender() {
  if (hypotheses_.empty()) return;
  // Stable decoder text already carries a trailing delimiter when a word is
  // complete. Never manufacture one here: doing so could promote a partial
  // callsign and leave stale provisional attribution latched.
  const auto& evidence = hypotheses_[leader_index_].decoder.currentUpdate().text;
  const std::size_t start = std::min(
      transmission_primary_starts_[leader_index_], evidence.size());
  const auto sender = CallsignPolicy::strong_sender_in_text(
      std::string_view(evidence).substr(start));
  current_sender_callsign_ = sender.value_or(std::string{});

  current_sender_wpm_ = 0.0;
  if (current_sender_callsign_.empty()) return;
  if (cadence_dot_ms_ > 0.0 && cadence_confidence_ >= 0.45F) {
    current_sender_wpm_ = 1'200.0 / cadence_dot_ms_;
  } else if (const auto* remembered = senderCadence(
                 current_sender_callsign_)) {
    current_sender_wpm_ = remembered->wpm;
  }
}

void CwMultiSpeedDecoder::commitCompletedTransmission(
    const std::size_t final_leader) {
  completeTransmission(final_leader);
  committed_prefix_ += hypotheses_[final_leader].decoder.currentUpdate().text;
  if (!committed_prefix_.empty() && committed_prefix_.back() != ' ')
    committed_prefix_.push_back(' ');
  if (committed_prefix_.size() > 4'096)
    committed_prefix_.erase(0, committed_prefix_.size() - 4'096);
  if (!refined_text_.empty() && refined_text_.back() != ' ')
    refined_text_.push_back(' ');
  transmission_refined_start_ = refined_text_.size();
  resetLatticeSegment();
  resetHypotheses();
}

void CwMultiSpeedDecoder::beginNextTransmissionWithoutAcousticReset() {
  for (std::size_t index = 0; index < hypotheses_.size() &&
       index < transmission_primary_starts_.size(); ++index) {
    transmission_primary_starts_[index] =
        hypotheses_[index].decoder.currentUpdate().text.size();
  }
  transmission_refined_start_ = refined_text_.size();
  contextual_lattice_text_.clear();
  current_sender_callsign_.clear();
  current_sender_wpm_ = 0.0;
  active_transmission_completed_ = false;
  pending_turn_timing_fingerprint_.reset();

  // A semantic sender turn gets a fresh cadence observation window without
  // rewriting the continuously accumulated acoustic transcript or timing
  // hypotheses. This preserves evidence while preventing one operator's
  // manual weighting from becoming the next operator's measured cadence.
  recent_mark_count_ = 0;
  recent_gap_count_ = 0;
  recent_mark_index_ = 0;
  recent_gap_index_ = 0;
  recent_mark_gap_count_ = 0;
  recent_mark_gap_index_ = 0;
  pending_cadence_mark_ms_ = 0.0;
  pending_cadence_mark_ = false;
  cadence_state_started_ns_ = 0;
  cadence_dot_ms_ = 0.0;
  cadence_confidence_ = 0.0F;
  lattice_cadence_dot_ms_ = 0.0;
  lattice_cadence_confidence_ = 0.0F;
  cadence_initialized_ = false;
  last_observed_segment_ns_ = 0;
  cadence_key_down_ = false;
}

void CwMultiSpeedDecoder::completeTransmission(
    const std::size_t final_leader) {
  if (active_transmission_completed_ || final_leader >= hypotheses_.size())
    return;
  const auto& complete_primary =
      hypotheses_[final_leader].decoder.currentUpdate().text;
  const std::size_t primary_start = std::min(
      transmission_primary_starts_[final_leader], complete_primary.size());
  std::string primary = complete_primary.substr(primary_start);
  std::string refined = transmission_refined_start_ < refined_text_.size()
      ? refined_text_.substr(transmission_refined_start_)
      : std::string{};
  const auto trim = [](std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string{};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
  };
  primary = trim(std::move(primary));
  refined = trim(std::move(refined));
  const std::string acoustic_text = !refined.empty() ? refined : primary;
  if (acoustic_text.empty()) return;
  const auto same_decoded_characters = [](const std::string_view left,
                                           const std::string_view right) {
    const auto compact = [](const std::string_view value) {
      std::string result;
      result.reserve(value.size());
      for (const unsigned char character : value) {
        if (std::isspace(character) == 0)
          result.push_back(static_cast<char>(character));
      }
      return result;
    };
    return compact(left) == compact(right);
  };
  std::string text = !contextual_lattice_text_.empty() &&
          same_decoded_characters(contextual_lattice_text_, acoustic_text)
      ? contextual_lattice_text_ : reconstructCwWordGaps(acoustic_text);

  const auto completed_sender = [](std::string text) {
    if (text.empty() || text.back() != ' ') text.push_back(' ');
    return CallsignPolicy::strong_sender_in_text(text);
  };
  const auto primary_sender = completed_sender(primary);
  const auto refined_sender = completed_sender(refined);
  // Completion recomputes attribution from the final selected evidence.
  // Conflicting final paths are uncertainty, never permission to retain a
  // provisional sender seen earlier in the turn.
  std::string sender;
  if (primary_sender && refined_sender &&
      *primary_sender != *refined_sender) {
    sender.clear();
  } else if (refined_sender) {
    sender = *refined_sender;
  } else if (primary_sender) {
    sender = *primary_sender;
  }
  const auto& update = hypotheses_[final_leader].decoder.currentUpdate();
  const bool cadence_supported = cadence_dot_ms_ > 0.0 &&
                                 cadence_confidence_ >= 0.45F;
  const double turn_wpm = cadence_supported
      ? 1'200.0 / cadence_dot_ms_ : update.wpm;
  const float turn_confidence = cadence_supported ? cadence_confidence_ : 0.0F;
  if (!sender.empty() && cadence_supported)
    rememberSenderCadence(sender, turn_wpm, turn_confidence);
  if (transmissions_.size() >= kMaximumTransmissionTurns)
    transmissions_.erase(transmissions_.begin());
  transmissions_.push_back({.sequence = active_transmission_sequence_,
                            .text = std::move(text),
                            .sender_callsign = sender,
                            .wpm = turn_wpm,
                            .cadence_confidence = turn_confidence,
                            .timing_fingerprint =
                                pending_turn_timing_fingerprint_});
  pending_turn_timing_fingerprint_.reset();
  active_transmission_completed_ = true;
  active_transmission_sequence_ = ++next_transmission_sequence_;
}

void CwMultiSpeedDecoder::observeLattice(
    const bool key_down, const float key_down_probability,
    const std::uint64_t timestamp_ns) {
  const float probability = std::clamp(key_down_probability, 0.0F, 1.0F);
  if (!lattice_initialized_) {
    lattice_initialized_ = true;
    lattice_key_down_ = key_down;
    lattice_state_started_ns_ = timestamp_ns;
    lattice_last_timestamp_ns_ = timestamp_ns;
    return;
  }
  if (timestamp_ns < lattice_last_timestamp_ns_ ||
      timestamp_ns < lattice_state_started_ns_) {
    resetLatticeSegment();
    lattice_initialized_ = true;
    lattice_key_down_ = key_down;
    lattice_state_started_ns_ = timestamp_ns;
    lattice_last_timestamp_ns_ = timestamp_ns;
    return;
  }

  const double elapsed_ms = milliseconds(timestamp_ns -
                                          lattice_last_timestamp_ns_);
  const float state_confidence = lattice_key_down_ ? probability
                                                    : 1.0F - probability;
  lattice_confidence_sum_ += static_cast<double>(state_confidence) *
                             elapsed_ms;
  lattice_confidence_duration_ms_ += elapsed_ms;
  lattice_last_timestamp_ns_ = timestamp_ns;
  if (key_down == lattice_key_down_) return;

  const double duration_ms = milliseconds(timestamp_ns -
                                           lattice_state_started_ns_);
  const float confidence = lattice_confidence_duration_ms_ > 0.0
      ? static_cast<float>(std::clamp(
            lattice_confidence_sum_ / lattice_confidence_duration_ms_,
            0.0, 1.0))
      : state_confidence;
  const bool completed_gap = !lattice_key_down_;
  // Ignore leading silence. The lattice starts with the first physical mark,
  // so a recording or newly acquired track does not treat arbitrary pre-key
  // time as Morse spacing evidence.
  if (event_lattice_.observationCount() > 0U || lattice_key_down_) {
    static_cast<void>(event_lattice_.append({
        .keyed = lattice_key_down_,
        .duration_ms = duration_ms,
        .confidence = confidence,
        .started_ns = lattice_state_started_ns_,
        .ended_ns = timestamp_ns,
    }));
  }
  lattice_key_down_ = key_down;
  lattice_state_started_ns_ = timestamp_ns;
  lattice_confidence_sum_ = 0.0;
  lattice_confidence_duration_ms_ = 0.0;

  // A completed gap is the only point at which another stable character can
  // exist. Keep this bounded batch calculation off the per-sample path.
  if (completed_gap &&
      (lattice_last_decode_ns_ == 0U ||
       milliseconds(timestamp_ns - lattice_last_decode_ns_) >=
           config_.lattice_checkpoint_ms)) {
    refreshLattice(CwLatticeDecodeMode::Provisional);
    lattice_last_decode_ns_ = timestamp_ns;
  }
}

void CwMultiSpeedDecoder::refreshLattice(const CwLatticeDecodeMode mode) {
  if (mode == CwLatticeDecodeMode::Flush)
    pending_turn_timing_fingerprint_.reset();
  if (event_lattice_.observationCount() == 0U || hypotheses_.empty()) return;
  // Every fixed timing anchor continues running after presentation lock. Use
  // the currently strongest complete acoustic path, not the historical lock,
  // so the refinement path can recover from an early cadence choice without
  // rewriting the primary transcript.
  const std::size_t lattice_leader = selectLeader();
  double candidate_wpm =
      hypotheses_[lattice_leader].decoder.currentUpdate().wpm;
  const double cadence_wpm = lattice_cadence_dot_ms_ > 0.0
      ? 1'200.0 / lattice_cadence_dot_ms_ : 0.0;
  const double cadence_ratio = candidate_wpm > 0.0
      ? cadence_wpm / candidate_wpm : 0.0;
  // The independent estimator is deliberately a guard, not a broad override:
  // noisy pileups can produce a confident-looking harmonic fit at roughly
  // twice the selected speed. Use it only when strong evidence agrees with
  // the continuously evaluated timing bank's neighborhood.
  if (lattice_cadence_confidence_ >= 0.65F && cadence_wpm > 0.0 &&
      cadence_ratio >= 0.75 && cadence_ratio <= 1.35) {
    candidate_wpm = cadence_wpm;
  }
  // Once explicit handover text identifies this operator, a cadence learned
  // from that same operator's earlier completed turn is a bounded prior. It
  // may refine only a timing-bank neighborhood that already agrees; it cannot
  // pull an unrelated or ambiguous turn to a memorized speed.
  if (!current_sender_callsign_.empty()) {
    if (const auto* remembered = senderCadence(current_sender_callsign_);
        remembered != nullptr && remembered->confidence >= 0.55F &&
        candidate_wpm > 0.0) {
      const double ratio = remembered->wpm / candidate_wpm;
      if (ratio >= 0.75 && ratio <= 1.35)
        candidate_wpm = 0.70 * candidate_wpm + 0.30 * remembered->wpm;
    }
  }
  if (!std::isfinite(candidate_wpm) || candidate_wpm <= 0.0) return;

  const double baseline_dot_ms = 1'200.0 / candidate_wpm;
  CwEventLatticeResult decoded;
  bool decoded_ready = false;
  // Only a completed semantic turn may re-evaluate timing. The live and
  // fixed-lag paths remain unchanged and append-only. Every alternative uses
  // the same immutable physical runs; filter-width retries require a separate
  // bounded receive-feature store and are not fabricated here.
  if (mode == CwLatticeDecodeMode::Flush) {
    std::array<CwLatticeTimingPass, 9> passes{};
    std::size_t pass_count = 0U;
    const auto add_pass = [&](const double wpm) {
      if (!std::isfinite(wpm) || wpm < 5.0 || wpm > 80.0 ||
          pass_count >= passes.size()) {
        return;
      }
      for (std::size_t index = 0; index < pass_count; ++index) {
        if (std::abs(passes[index].wpm - wpm) < 0.25) return;
      }
      passes[pass_count++] = {.wpm = wpm, .dot_ms = 1'200.0 / wpm};
    };
    add_pass(candidate_wpm);
    for (const auto& hypothesis : hypotheses_)
      add_pass(hypothesis.decoder.currentUpdate().wpm);
    if (pass_count > 1U) {
      // The candidate speed is offered to the refinement first, so unless the
      // speed range rejected it outright, pass zero decodes exactly what this
      // function's own baseline decode would produce: same immutable runs,
      // same dot length, same mode, and decode has no state of its own.
      // Running both cost a second complete walk over every observation of the
      // transmission for an identical answer, so the baseline is taken from
      // the refinement instead, and decoded here only when the speed range did
      // reject the candidate and pass zero is some other hypothesis.
      const bool baseline_is_first_pass = passes.front().wpm == candidate_wpm;
      if (!baseline_is_first_pass) {
        decoded = event_lattice_.decode(baseline_dot_ms, mode);
        decoded_ready = true;
      }
      auto refinement = refineCwEventLattice(
          event_lattice_, std::span(passes).first(pass_count), mode);
      if (refinement.selection.accepted &&
          cwLatticeCleanlyExtendsCommit(
              refinement.decoded, lattice_committed_observation_id_)) {
        decoded = std::move(refinement.decoded);
        decoded_ready = true;
        // This local value labels the selected lattice alternatives below. It
        // does not replace the decoder's live WPM or the completed sender's
        // independently measured cadence.
        candidate_wpm = refinement.selected_wpm;
      } else if (!decoded_ready) {
        // A selection that was made but then refused by the commit boundary
        // leaves pass zero in `baseline`; a selection that was never made
        // leaves it in `decoded`. Either way this is the baseline decode.
        decoded = refinement.selection.accepted
            ? std::move(refinement.baseline) : std::move(refinement.decoded);
        decoded_ready = true;
      }
    }
  }
  if (!decoded_ready) decoded = event_lattice_.decode(baseline_dot_ms, mode);
  if (mode == CwLatticeDecodeMode::Flush &&
      decoded.alternatives.size() > 1U) {
    std::vector<CwContextAlternative> contextual;
    contextual.reserve(decoded.alternatives.size());
    for (const auto& alternative : decoded.alternatives) {
      contextual.push_back({.text = alternative.text(),
                            .acoustic_cost = alternative.acoustic_cost});
    }
    const auto selected = selectCwContextAlternative(
        contextual, config_.lattice_competitive_cost_margin);
    if (selected.index < decoded.alternatives.size())
      contextual_lattice_text_ = reconstructCwWordGaps(
          decoded.alternatives[selected.index].text());
  }
  if (mode == CwLatticeDecodeMode::Flush && !decoded.alternatives.empty()) {
    pending_turn_timing_fingerprint_ = makeCwTurnTimingFingerprint(
        decoded, 1'200.0 / candidate_wpm);
  }
  acoustic_alternatives_.clear();
  if (decoded.alternatives.empty()) return;
  acoustic_alternatives_.reserve(decoded.alternatives.size());
  for (const auto& alternative : decoded.alternatives) {
    std::uint64_t first_id = alternative.provisional_first_observation_id;
    std::uint64_t last_id = alternative.provisional_last_observation_id;
    if (!alternative.symbols.empty()) {
      first_id = alternative.symbols.front().first_observation_id;
      last_id = alternative.symbols.back().last_observation_id;
    }
    last_id = std::max(last_id,
                       alternative.provisional_last_observation_id);
    acoustic_alternatives_.push_back({
        .text = alternative.text(),
        .provisional_elements = alternative.provisional_elements,
        .wpm = candidate_wpm,
        .acoustic_cost = alternative.acoustic_cost,
        .evidence_confidence = alternative.evidence_confidence,
        .first_observation_id = first_id,
        .last_observation_id = last_id,
    });
  }

  const double maximum_cost = decoded.alternatives.front().acoustic_cost +
                              config_.lattice_competitive_cost_margin;
  std::size_t competitive_count = 1U;
  while (competitive_count < decoded.alternatives.size() &&
         decoded.alternatives[competitive_count].acoustic_cost <=
             maximum_cost) {
    ++competitive_count;
  }
  // A caller-requested flush closes this acoustic segment. No later evidence
  // can resolve an N-best disagreement to the left of the boundary, and
  // dropping that suffix would permanently stall the append-only transcript.
  // Finalize the MAP path while retaining every bounded alternative above so
  // residual uncertainty remains observable to diagnostics.
  if (mode == CwLatticeDecodeMode::Flush) competitive_count = 1U;
  // An early preferred-speed path can be internally self-consistent while it
  // is still the wrong cadence (a slow dit resembles a faster dash). Expose
  // its segment-scoped alternatives, but do not make them append-only until
  // the multi-speed acquisition has settled or an explicit flush closes the
  // segment.
  // A completed transmission provides a strong temporal boundary even when
  // hand keying widens the timing distribution. Permit a modestly lower
  // acoustic-confidence floor only at that boundary; provisional output keeps
  // the stricter gate so noise cannot continuously manufacture characters.
  const float evidence_floor = mode == CwLatticeDecodeMode::Flush
      ? std::max(0.30F,
                 config_.minimum_lattice_evidence_confidence - 0.10F)
      : config_.minimum_lattice_evidence_confidence;
  if ((!locked_ && mode == CwLatticeDecodeMode::Provisional) ||
      decoded.alternatives.front().evidence_confidence < evidence_floor) {
    return;
  }

  const std::uint64_t provisional_commit_limit =
      mode == CwLatticeDecodeMode::Flush
          ? std::numeric_limits<std::uint64_t>::max()
          : cwFixedLagCommitObservationId(
                decoded.observations, config_.lattice_fixed_lag_ms,
                config_.lattice_fixed_lag_observations);

  const auto& best_symbols = decoded.alternatives.front().symbols;
  for (std::size_t symbol_index = 0; symbol_index < best_symbols.size();
       ++symbol_index) {
    const auto& candidate = best_symbols[symbol_index];
    if (candidate.last_observation_id <= lattice_committed_observation_id_) {
      continue;
    }
    if (candidate.last_observation_id > provisional_commit_limit) break;
    // Never let a later best path reinterpret a run that has already crossed
    // the append-only boundary. A newly agreed symbol must begin entirely to
    // the right of the last committed physical observation.
    if (candidate.first_observation_id <=
        lattice_committed_observation_id_) {
      break;
    }
    bool agreed = true;
    for (std::size_t path_index = 1U; path_index < competitive_count;
         ++path_index) {
      const auto& symbols = decoded.alternatives[path_index].symbols;
      if (symbol_index >= symbols.size()) {
        agreed = false;
        break;
      }
      const auto& compared = symbols[symbol_index];
      if (candidate.symbol != compared.symbol ||
          candidate.known != compared.known ||
          candidate.word_boundary_after != compared.word_boundary_after ||
          candidate.first_observation_id != compared.first_observation_id ||
          candidate.last_observation_id != compared.last_observation_id) {
        agreed = false;
        break;
      }
    }
    if (!agreed) break;
    refined_text_ += candidate.known ? candidate.symbol : "?";
    if (candidate.word_boundary_after &&
        (refined_text_.empty() || refined_text_.back() != ' ')) {
      refined_text_.push_back(' ');
    }
    lattice_committed_observation_id_ = candidate.last_observation_id;
    if (refined_text_.size() > 4'096) {
      refined_text_.erase(0, refined_text_.size() - 4'096);
    }
  }
}

void CwMultiSpeedDecoder::resetLatticeSegment() noexcept {
  event_lattice_.reset();
  acoustic_alternatives_.clear();
  contextual_lattice_text_.clear();
  lattice_state_started_ns_ = 0;
  lattice_last_timestamp_ns_ = 0;
  lattice_last_decode_ns_ = 0;
  lattice_confidence_sum_ = 0.0;
  lattice_confidence_duration_ms_ = 0.0;
  lattice_initialized_ = false;
  lattice_key_down_ = false;
}

void CwMultiSpeedDecoder::considerLock(const float margin) {
  const auto& leader = hypotheses_[leader_index_].decoder.currentUpdate();
  if (leader.decoded_symbols < config_.lock_after_symbols) return;
  if (margin < config_.lock_score_margin &&
      leader.decoded_symbols <
          static_cast<std::uint32_t>(config_.lock_after_symbols) + 3U) {
    return;
  }
  locked_index_ = leader_index_;
  locked_ = true;
}

}  // namespace cwassistant::core
