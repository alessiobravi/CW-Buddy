#include "cwassistant/core/cw_event_lattice.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

namespace {

using cwassistant::core::CwEventLattice;
using cwassistant::core::CwEventLatticeConfig;
using cwassistant::core::CwRunObservation;

void expect(const bool condition, const std::string_view message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  std::exit(1);
}

std::string_view elementsFor(const char value) {
  struct Entry { char value; std::string_view elements; };
  static constexpr Entry entries[]{
      {'A', ".-"}, {'C', "-.-."}, {'D', "-.."}, {'E', "."},
      {'L', ".-.."}, {'Q', "--.-"}, {'Y', "-.--"},
      {'0', "-----"}, {'1', ".----"}, {'2', "..---"},
      {'3', "...--"}, {'4', "....-"}, {'5', "....."},
      {'6', "-...."}, {'7', "--..."}, {'8', "---.."},
      {'9', "----."},
  };
  for (const auto& entry : entries) {
    if (entry.value == value) return entry.elements;
  }
  return {};
}

void appendRun(CwEventLattice& lattice, const bool keyed,
               const double duration_ms) {
  static_cast<void>(lattice.append({.keyed = keyed,
                                    .duration_ms = duration_ms,
                                    .confidence = 1.0F}));
}

void appendText(CwEventLattice& lattice, const std::string_view text,
                const double dot_ms, const double character_gap_dots) {
  for (std::size_t character = 0; character < text.size(); ++character) {
    const auto elements = elementsFor(text[character]);
    for (std::size_t element = 0; element < elements.size(); ++element) {
      appendRun(lattice, true,
                (elements[element] == '-' ? 3.0 : 1.0) * dot_ms);
      const bool final_element = element + 1U == elements.size();
      if (!final_element) appendRun(lattice, false, dot_ms);
    }
    if (character + 1U < text.size()) {
      appendRun(lattice, false, character_gap_dots * dot_ms);
    }
  }
}

void testCompressedCallsignAndDigit() {
  CwEventLattice lattice;
  appendText(lattice, "EA1EYL", 55.0, 2.0);
  const auto result = lattice.decode(
      55.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(!result.alternatives.empty(), "compressed call produces candidates");
  expect(result.alternatives.front().text() == "EA1EYL",
         "two-dot character gaps preserve the five-element digit");
  const auto& one = result.alternatives.front().symbols[2];
  expect(one.known && one.symbol == "1" && one.elements == ".----",
         "digit retains its raw Morse elements");
}

void testAllDigits() {
  CwEventLattice lattice({.maximum_observations = 256,
                          .beam_width = 32,
                          .maximum_alternatives = 4});
  appendText(lattice, "0123456789", 60.0, 3.0);
  const auto result = lattice.decode(
      60.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(!result.alternatives.empty() &&
             result.alternatives.front().text() == "0123456789",
         "all five-element digits survive acoustic segmentation");
}

void testUnknownRemainsAvailable() {
  CwEventLattice lattice;
  // .-.- is not an assigned Morse character. Exact one-dot internal gaps and
  // the three-dot final gap make UNKNOWN acoustically preferable to inventing
  // several characters at element gaps.
  appendRun(lattice, true, 60.0);
  appendRun(lattice, false, 60.0);
  appendRun(lattice, true, 180.0);
  appendRun(lattice, false, 60.0);
  appendRun(lattice, true, 60.0);
  appendRun(lattice, false, 60.0);
  appendRun(lattice, true, 180.0);
  appendRun(lattice, false, 180.0);
  const auto result = lattice.decode(60.0);
  expect(!result.alternatives.empty(), "invalid Morse produces a candidate");
  expect(result.alternatives.front().symbols.size() == 1U &&
             !result.alternatives.front().symbols.front().known &&
             result.alternatives.front().text() == "?",
         "acoustically preferred invalid sequence stays UNKNOWN");
  expect(result.alternatives.front().symbols.front().elements == ".-.-",
         "UNKNOWN retains all observed elements without suffix truncation");
}

void testUnknownCompetesWithLegalSymbol() {
  CwEventLattice lattice({.maximum_observations = 32,
                          .beam_width = 16,
                          .maximum_alternatives = 8,
                          .unknown_symbol_cost = 1.0});
  appendRun(lattice, true, 60.0);
  appendRun(lattice, false, 180.0);
  const auto result = lattice.decode(60.0);
  bool has_e = false;
  bool has_unknown = false;
  for (const auto& alternative : result.alternatives) {
    has_e = has_e || (alternative.symbols.size() == 1U &&
                      alternative.symbols.front().known &&
                      alternative.text() == "E");
    has_unknown = has_unknown || (alternative.symbols.size() == 1U &&
                                  !alternative.symbols.front().known);
  }
  expect(has_e && has_unknown,
         "UNKNOWN competes with a legal symbol at a calibrated cost");
}

void testAcousticWordAmbiguity() {
  CwEventLattice lattice({.maximum_observations = 32,
                          .beam_width = 16,
                          .maximum_alternatives = 8});
  appendRun(lattice, true, 55.0);
  appendRun(lattice, false, 220.0);
  appendRun(lattice, true, 55.0);
  const auto result = lattice.decode(
      55.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  bool has_character = false;
  bool has_word = false;
  for (const auto& alternative : result.alternatives) {
    has_character = has_character || alternative.text() == "EE";
    has_word = has_word || alternative.text() == "E E";
  }
  expect(has_character && has_word,
         "overlapping character/word timing remains an N-best ambiguity");
}

void testBoundedState() {
  CwEventLattice lattice({.maximum_observations = 32,
                          .beam_width = 8,
                          .maximum_alternatives = 3});
  const std::size_t initial_bytes = lattice.stateBytes();
  for (std::size_t index = 0; index < 2'000U; ++index) {
    appendRun(lattice, index % 2U == 0U, index % 2U == 0U ? 60.0 : 180.0);
  }
  const auto result = lattice.decode(60.0);
  expect(lattice.observationCount() == 32U && result.input_truncated,
         "raw observation history is bounded and reports truncation");
  expect(result.observations.size() == 32U &&
             result.alternatives.size() <= 3U,
         "result sizes obey configured bounds");
  expect(lattice.stateBytes() == initial_bytes,
         "steady-state retained memory does not grow with stream duration");
}

void testProvisionalTailAndFlushContract() {
  using cwassistant::core::CwLatticeDecodeMode;
  CwEventLattice lattice;
  appendRun(lattice, true, 60.0);
  const auto provisional = lattice.decode(60.0);
  expect(!provisional.alternatives.empty() &&
             provisional.alternatives.front().text().empty() &&
             provisional.alternatives.front().provisional_elements == ".",
         "a trailing mark remains explicitly provisional");
  const auto flushed = lattice.decode(60.0, CwLatticeDecodeMode::Flush);
  expect(!flushed.alternatives.empty() &&
             flushed.alternatives.front().text() == "E" &&
             flushed.alternatives.front().provisional_elements.empty(),
         "only an explicit flush finalizes an unbounded trailing character");

  appendRun(lattice, false, 180.0);
  const auto bounded = lattice.decode(60.0);
  expect(!bounded.alternatives.empty() &&
             bounded.alternatives.front().text() == "E",
         "a completed character gap finalizes the tail without flush");
}

void testObservationContractAndStableIds() {
  CwEventLattice lattice({.maximum_observations = 16});
  const auto first = lattice.append({.keyed = true,
                                     .duration_ms = 30.0,
                                     .confidence = 0.8F,
                                     .started_ns = 100U,
                                     .ended_ns = 30'000'100U});
  const auto coalesced = lattice.append({.keyed = true,
                                         .duration_ms = 30.0,
                                         .confidence = 0.6F,
                                         .started_ns = 30'000'100U,
                                         .ended_ns = 60'000'100U});
  expect(first && coalesced && *first == *coalesced &&
             lattice.observationCount() == 1U,
         "adjacent equal key states coalesce under one stable observation ID");
  const auto discontinuous_same_state = lattice.append({
      .keyed = true,
      .duration_ms = 30.0,
      .confidence = 1.0F,
      .started_ns = 70'000'100U,
      .ended_ns = 100'000'100U});
  expect(!discontinuous_same_state,
         "separated equal key states are rejected rather than bridging missing evidence");
  const auto overlap = lattice.append({.keyed = false,
                                       .duration_ms = 10.0,
                                       .confidence = 1.0F,
                                       .started_ns = 50'000'100U,
                                       .ended_ns = 108'000'100U});
  expect(!overlap && lattice.observationCount() == 1U,
         "overlapping timestamps are rejected instead of reordered");

  std::uint64_t final_id = *first;
  std::uint64_t timestamp = 60'000'100U;
  for (std::size_t index = 0; index < 24U; ++index) {
    const bool keyed = index % 2U != 0U;
    const std::uint64_t duration = keyed ? 60'000'000U : 180'000'000U;
    const auto id = lattice.append({
        .keyed = keyed,
        .duration_ms = static_cast<double>(duration) / 1'000'000.0,
        .confidence = 1.0F,
        .started_ns = timestamp,
        .ended_ns = timestamp + duration});
    expect(id && *id > final_id, "new physical runs receive monotonic IDs");
    if (id) final_id = *id;
    timestamp += duration;
  }
  const auto truncated = lattice.decode(
      60.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(truncated.input_truncated && truncated.left_prefix_discarded &&
             !truncated.observations.empty() &&
             truncated.observations.front().observation_id > *first,
         "left truncation preserves IDs and reports discarded prefix evidence");
  if (!truncated.alternatives.empty() &&
      !truncated.alternatives.front().symbols.empty()) {
    expect(truncated.alternatives.front().symbols.front().first_observation_id >
               truncated.observations.front().observation_id,
           "decode resumes only after a retained safe boundary");
  }
  expect(truncated.rejected_observations == 2U &&
             truncated.coalesced_observations == 1U,
         "contract repairs and rejections remain observable");

  lattice.reset();
  const auto after_reset = lattice.append({.keyed = true,
                                           .duration_ms = 60.0,
                                           .confidence = 1.0F});
  expect(after_reset && *after_reset > final_id,
         "reset does not recycle observation identities");
}

void testFloatingConfigurationIsSanitized() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  CwEventLattice lattice({.compressed_character_gap_dots = nan,
                          .character_gap_dots = infinity,
                          .compressed_word_gap_dots = -infinity,
                          .word_gap_dots = nan,
                          .unknown_symbol_cost = infinity,
                          .timing_tolerance_scale = nan,
                          .maximum_timing_tolerance_scale = infinity,
                          .minimum_tolerance_observations = 12,
                          .minimum_tolerance_observation_confidence =
                              static_cast<float>(nan)});
  static_cast<void>(lattice.append({.keyed = true,
                                    .duration_ms = 60.0,
                                    .confidence = static_cast<float>(nan)}));
  appendRun(lattice, false, 180.0);
  const auto result = lattice.decode(
      60.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(!result.alternatives.empty() &&
             std::isfinite(result.alternatives.front().acoustic_cost) &&
             result.alternatives.front().evidence_confidence < 0.6F,
         "non-finite config and confidence values degrade safely");
}

void testDistinctUnknownEvidenceSurvivesDeduplication() {
  CwEventLattice lattice({.maximum_observations = 16,
                          .beam_width = 16,
                          .maximum_alternatives = 16,
                          .unknown_symbol_cost = 0.5});
  static_cast<void>(lattice.append({.keyed = true,
                                    .duration_ms = 120.0,
                                    .confidence = 0.2F}));
  const auto result = lattice.decode(
      60.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  bool unknown_dot = false;
  bool unknown_dash = false;
  for (const auto& alternative : result.alternatives) {
    if (alternative.symbols.size() != 1U ||
        alternative.symbols.front().known) {
      continue;
    }
    unknown_dot = unknown_dot ||
                  alternative.symbols.front().elements == ".";
    unknown_dash = unknown_dash ||
                   alternative.symbols.front().elements == "-";
  }
  expect(unknown_dot && unknown_dash,
         "UNKNOWN paths with different acoustic elements are not deduplicated");
}

void testIncrementalPrefixAndJitter() {
  CwEventLattice incremental;
  appendRun(incremental, true, 60.0);
  expect(incremental.decode(60.0).alternatives.front().text().empty(),
         "incremental mark does not invent a stable character");
  appendRun(incremental, false, 180.0);
  const std::string first = incremental.decode(60.0).alternatives.front().text();
  appendRun(incremental, true, 60.0);
  appendRun(incremental, false, 60.0);
  appendRun(incremental, true, 180.0);
  appendRun(incremental, false, 180.0);
  const std::string second = incremental.decode(60.0).alternatives.front().text();
  expect(first == "E" && second == "EA" &&
             second.starts_with(first),
         "successive completed gaps extend the stable acoustic prefix");

  CwEventLattice jittered({.maximum_observations = 128,
                           .beam_width = 24});
  const std::string_view message = "EA1EYL";
  std::size_t run_index = 0;
  for (std::size_t character = 0; character < message.size(); ++character) {
    const auto elements = elementsFor(message[character]);
    for (std::size_t element = 0; element < elements.size(); ++element) {
      const double jitter = run_index++ % 2U == 0U ? 0.88 : 1.12;
      appendRun(jittered, true,
                (elements[element] == '-' ? 3.0 : 1.0) * 55.0 * jitter);
      if (element + 1U < elements.size()) {
        appendRun(jittered, false, 55.0 *
                   (run_index++ % 2U == 0U ? 0.88 : 1.12));
      }
    }
    if (character + 1U < message.size()) {
      appendRun(jittered, false, 110.0 *
                 (run_index++ % 2U == 0U ? 0.90 : 1.10));
    }
  }
  const auto jittered_result = jittered.decode(
      55.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(!jittered_result.alternatives.empty() &&
             jittered_result.alternatives.front().text() == "EA1EYL",
         "compressed callsign segmentation survives bounded timing jitter");
}

void testLongHardNegativeIsNotDecodedFromSuffix() {
  CwEventLattice lattice;
  for (std::size_t element = 0; element < 12U; ++element) {
    appendRun(lattice, true, 60.0);
    if (element + 1U < 12U) appendRun(lattice, false, 60.0);
  }
  const auto result = lattice.decode(
      60.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(!result.alternatives.empty() &&
             result.alternatives.front().symbols.size() == 1U &&
             !result.alternatives.front().symbols.front().known &&
             result.alternatives.front().symbols.front().elements.size() == 12U,
         "long invalid keying remains one complete UNKNOWN evidence span");
}

void testLowConfidenceIsExplicitAndFlattensRanking() {
  const auto decode_mark = [](const float confidence) {
    CwEventLattice lattice({.maximum_observations = 16,
                            .beam_width = 8,
                            .maximum_alternatives = 8,
                            .unknown_symbol_cost = 12.0});
    static_cast<void>(lattice.append({.keyed = true,
                                      .duration_ms = 90.0,
                                      .confidence = confidence}));
    return lattice.decode(
        60.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  };
  const auto strong = decode_mark(1.0F);
  const auto weak = decode_mark(0.01F);
  expect(strong.alternatives.size() >= 2U && weak.alternatives.size() >= 2U,
         "ambiguous duration retains alternatives at both confidence levels");
  const double strong_margin = strong.alternatives[1].acoustic_cost -
                               strong.alternatives[0].acoustic_cost;
  const double weak_margin = weak.alternatives[1].acoustic_cost -
                             weak.alternatives[0].acoustic_cost;
  expect(weak_margin < strong_margin &&
             weak.alternatives.front().evidence_confidence < 0.02F,
         "weak observations explicitly reduce confidence and flatten ranking");
}

void testHandSentVarianceIsBoundedAndVisible() {
  CwEventLattice clean({.maximum_observations = 128,
                        .beam_width = 24});
  appendText(clean, "EA1EYL", 55.0, 2.0);
  const auto clean_result = clean.decode(
      55.0, cwassistant::core::CwLatticeDecodeMode::Flush);

  CwEventLattice hand_sent({.maximum_observations = 128,
                            .beam_width = 24});
  const std::string_view message = "EA1EYL";
  constexpr double mark_scales[]{0.74, 1.22, 0.91, 1.28, 0.82, 1.13};
  constexpr double gap_dots[]{1.75, 2.45, 3.20, 1.90, 2.80};
  std::size_t mark_index = 0;
  std::size_t gap_index = 0;
  for (std::size_t character = 0; character < message.size(); ++character) {
    const auto elements = elementsFor(message[character]);
    for (std::size_t element = 0; element < elements.size(); ++element) {
      const double duration =
          (elements[element] == '-' ? 3.0 : 1.0) * 55.0 *
          mark_scales[mark_index++ % std::size(mark_scales)];
      appendRun(hand_sent, true, duration);
      if (element + 1U < elements.size()) {
        const double internal_gap = 55.0 *
            mark_scales[mark_index++ % std::size(mark_scales)];
        appendRun(hand_sent, false, internal_gap);
      }
    }
    if (character + 1U < message.size()) {
      const double character_gap = 55.0 *
          gap_dots[gap_index++ % std::size(gap_dots)];
      appendRun(hand_sent, false, character_gap);
    }
  }
  const auto hand_result = hand_sent.decode(
      55.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(!clean_result.alternatives.empty() &&
             clean_result.effective_timing_tolerance_scale >= 0.99 &&
             clean_result.effective_timing_tolerance_scale <= 1.05,
         "clean machine timing keeps automatic tolerance near one");
  expect(!hand_result.alternatives.empty() &&
             hand_result.alternatives.front().text() == "EA1EYL" &&
             hand_result.effective_timing_tolerance_scale > 1.10 &&
             hand_result.effective_timing_tolerance_scale <= 2.5,
         "bounded tolerance handles realistic uneven hand-sent marks and gaps");
  expect(hand_result.alternatives.front().evidence_confidence <
             clean_result.alternatives.front().evidence_confidence,
         "higher timing variance lowers reported evidence confidence");
}

void testIsolatedOutlierDoesNotInflateTolerance() {
  CwEventLattice lattice({.maximum_observations = 128,
                          .beam_width = 16});
  appendText(lattice, "EA1EYLEA1EYL", 60.0, 3.0);
  // Complete the preceding mark with one grossly long gap, then provide
  // another clean character. A trimmed upper quartile must ignore this single
  // click/dropout-like outlier rather than widening the whole segment.
  appendRun(lattice, false, 900.0);
  appendRun(lattice, true, 60.0);
  const auto result = lattice.decode(
      60.0, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(result.effective_timing_tolerance_scale <= 1.05,
         "one timing outlier cannot inflate the segment tolerance");
}

void testDecodeCostFollowsObservationsNotTheirSquare() {
  // A decode used to carry each surviving path's whole decoded history along
  // with every copy the beam makes of it, and to rebuild a text description of
  // that history for every path at every pruning step. Both made one decode
  // cost time proportional to the square of the observation count, so closing
  // a long transmission cost far more than twice closing one half its length.
  // That is the shape an operator experiences as the application stalling once
  // a station has been transmitting for a while, and it is the shape this test
  // guards. It deliberately measures the ratio between two sizes rather than
  // any absolute duration, so it means the same thing on a slow machine as on
  // a fast one.
  const auto measure = [](const std::size_t target_observations) {
    CwEventLattice lattice({.maximum_observations = 512, .beam_width = 16});
    while (lattice.observationCount() < target_observations) {
      appendText(lattice, "EA1EYL", 60.0, 3.0);
      appendRun(lattice, false, 7.0 * 60.0);
    }
    double best_seconds = std::numeric_limits<double>::infinity();
    for (int attempt = 0; attempt < 7; ++attempt) {
      const auto started = std::chrono::steady_clock::now();
      for (int repeat = 0; repeat < 4; ++repeat) {
        const auto result = lattice.decode(
            60.0, cwassistant::core::CwLatticeDecodeMode::Flush);
        expect(!result.alternatives.empty(),
               "the timed decode produces candidates");
      }
      const std::chrono::duration<double> elapsed =
          std::chrono::steady_clock::now() - started;
      best_seconds = std::min(best_seconds, elapsed.count());
    }
    // The fastest attempt is the one least disturbed by whatever else the
    // machine was doing, which is what keeps this usable under a loaded
    // continuous-integration runner.
    return best_seconds;
  };

  const double small = measure(64U);
  const double large = measure(256U);
  expect(small > 0.0, "the small decode is measurable");
  // Four times the observations. Cost proportional to the observations lands
  // near four, and this implementation measures about four and a half; the
  // implementation it replaced measured close to nine. The bound sits between
  // the two with room on both sides, and taking the fastest of several
  // attempts at each size is what keeps a busy machine from moving the ratio
  // far enough to matter.
  const double ratio = large / small;
  if (ratio >= 6.0) {
    std::cerr << "decode cost ratio for four times the observations: "
              << ratio << '\n';
  }
  expect(ratio < 6.0,
         "decode cost follows the observation count, not its square");
}

}  // namespace

int main() {
  testCompressedCallsignAndDigit();
  testAllDigits();
  testUnknownRemainsAvailable();
  testUnknownCompetesWithLegalSymbol();
  testAcousticWordAmbiguity();
  testBoundedState();
  testProvisionalTailAndFlushContract();
  testObservationContractAndStableIds();
  testFloatingConfigurationIsSanitized();
  testDistinctUnknownEvidenceSurvivesDeduplication();
  testIncrementalPrefixAndJitter();
  testLongHardNegativeIsNotDecodedFromSuffix();
  testLowConfidenceIsExplicitAndFlattensRanking();
  testHandSentVarianceIsBoundedAndVisible();
  testIsolatedOutlierDoesNotInflateTolerance();
  testDecodeCostFollowsObservationsNotTheirSquare();
  std::cout << "cw_event_lattice_tests: PASS\n";
}
