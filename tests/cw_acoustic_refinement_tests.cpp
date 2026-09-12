#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cwassistant/core/cw_acoustic_refinement.hpp"

namespace {

using cwassistant::core::CwAcousticPassCandidate;
using cwassistant::core::CwAcousticRefinementRejection;

void expect(const bool condition, const char* message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

CwAcousticPassCandidate candidate(std::string signature,
                                  const double normalized_cost,
                                  const float confidence = 0.80F,
                                  const std::size_t observations = 20U,
                                  const std::size_t symbols = 6U,
                                  const std::uint64_t start_ns = 1'000U,
                                  const std::uint64_t end_ns =
                                      5'000'001'000U) {
  return {.provenance = {.filter_width_hz = 120.0,
                         .wpm = 20.0,
                         .character_gap_dots = 3.0,
                         .word_gap_dots = 7.0},
          .acoustic_signature = std::move(signature),
          .acoustic_cost = normalized_cost *
                           static_cast<double>(observations),
          .evidence_confidence = confidence,
          .observation_count = observations,
          .symbol_count = symbols,
          .evidence_started_ns = start_ns,
          .evidence_ended_ns = end_ns};
}

void testClearAcousticImprovementWins() {
  std::vector<CwAcousticPassCandidate> passes;
  passes.push_back(candidate("baseline", 0.50));
  auto narrow = candidate("narrow-filter", 0.38, 0.84F, 22U);
  narrow.provenance.filter_width_hz = 60.0;
  narrow.provenance.wpm = 21.5;
  passes.push_back(std::move(narrow));
  passes.push_back(candidate("wide-filter", 0.49, 0.83F, 18U));
  const auto selected =
      cwassistant::core::selectCwAcousticRefinement(passes);
  expect(selected.accepted && selected.selected_index == 1U &&
             selected.normalized_cost_improvement > 0.11 &&
             selected.relative_cost_improvement > 0.20,
         "a time-aligned lower-cost acoustic pass is selected");
}

void testWeakOrMisalignedPassFailsClosed() {
  std::vector<CwAcousticPassCandidate> passes;
  passes.push_back(candidate("baseline", 0.50));
  passes.push_back(candidate("weak", 0.20, 0.79F));
  passes.push_back(candidate("different-time", 0.10, 0.90F, 20U, 6U,
                             900'001'000U, 5'000'001'000U));
  const auto selected =
      cwassistant::core::selectCwAcousticRefinement(passes);
  expect(!selected.accepted && selected.selected_index == 0U &&
             selected.rejection ==
                 CwAcousticRefinementRejection::NoEligibleAlternative,
         "confidence regression and mismatched intervals cannot revise text");
}

void testSmallGainDoesNotRevise() {
  const std::vector passes{candidate("baseline", 0.50),
                           candidate("nearby", 0.485, 0.82F)};
  const auto selected =
      cwassistant::core::selectCwAcousticRefinement(passes);
  expect(!selected.accepted && selected.rejection ==
             CwAcousticRefinementRejection::InsufficientImprovement,
         "a numerically better but immaterial pass leaves baseline stable");
}

void testContradictoryNearTieAbstains() {
  const std::vector passes{candidate("baseline", 0.60),
                           candidate("dit-dah-a", 0.39, 0.85F),
                           candidate("dit-dah-b", 0.40, 0.86F)};
  const auto selected =
      cwassistant::core::selectCwAcousticRefinement(passes);
  expect(!selected.accepted && selected.rejection ==
             CwAcousticRefinementRejection::AmbiguousImprovement,
         "close contradictory acoustic explanations remain unresolved");
}

void testEquivalentNearTieMayAgree() {
  const std::vector passes{candidate("baseline", 0.60),
                           candidate("same-physical-path", 0.39, 0.85F),
                           candidate("same-physical-path", 0.40, 0.86F)};
  const auto selected =
      cwassistant::core::selectCwAcousticRefinement(passes);
  expect(selected.accepted && selected.selected_index == 1U,
         "near-tied passes agreeing on physical symbols can refine");
}

void testIneligibleContradictionCannotVeto() {
  const std::vector passes{candidate("baseline", 0.60),
                           candidate("supported", 0.39, 0.85F),
                           candidate("weak-contradiction", 0.40, 0.79F)};
  const auto selected =
      cwassistant::core::selectCwAcousticRefinement(passes);
  expect(selected.accepted && selected.selected_index == 1U,
         "an ineligible low-confidence pass cannot manufacture ambiguity");
}

void testPassBudgetIsHard() {
  const std::vector passes{candidate("baseline", 0.60),
                           candidate("inside-budget", 0.57, 0.85F),
                           candidate("outside-budget", 0.10, 0.95F)};
  const auto selected = cwassistant::core::selectCwAcousticRefinement(
      passes, {.maximum_passes = 2U});
  expect(!selected.accepted && selected.selected_index == 0U,
         "passes beyond the hard budget are not inspected");
}

void testInvalidBaselineCannotBeHidden() {
  auto baseline = candidate("baseline", 0.50);
  baseline.acoustic_cost = -1.0;
  const std::vector passes{baseline, candidate("alternative", 0.10)};
  const auto selected =
      cwassistant::core::selectCwAcousticRefinement(passes);
  expect(!selected.accepted && selected.rejection ==
             CwAcousticRefinementRejection::InvalidBaseline,
         "invalid live evidence cannot be replaced by a plausible retry");
}

void appendRun(cwassistant::core::CwEventLattice& lattice,
               const bool keyed, const double duration_ms,
               std::uint64_t& now_ns) {
  const auto started_ns = now_ns;
  now_ns += static_cast<std::uint64_t>(duration_ms * 1'000'000.0);
  static_cast<void>(lattice.append({.keyed = keyed,
                                    .duration_ms = duration_ms,
                                    .confidence = 0.95F,
                                    .started_ns = started_ns,
                                    .ended_ns = now_ns}));
}

void testLatticeRetryImprovesWrongTimingBaseline() {
  cwassistant::core::CwEventLattice lattice;
  std::uint64_t now_ns = 1U;
  // A (.-), compressed character gap, then E (.). The 60 ms timing pass is
  // the physical source; 100 ms deliberately represents a wrong slow lock.
  appendRun(lattice, true, 60.0, now_ns);
  appendRun(lattice, false, 60.0, now_ns);
  appendRun(lattice, true, 180.0, now_ns);
  appendRun(lattice, false, 120.0, now_ns);
  appendRun(lattice, true, 60.0, now_ns);
  const std::array passes{
      cwassistant::core::CwLatticeTimingPass{12.0, 100.0},
      cwassistant::core::CwLatticeTimingPass{20.0, 60.0}};
  const auto baseline = lattice.decode(
      passes.front().dot_ms,
      cwassistant::core::CwLatticeDecodeMode::Flush);
  const auto refined = cwassistant::core::refineCwEventLattice(
      lattice, passes, cwassistant::core::CwLatticeDecodeMode::Flush,
      {.minimum_normalized_cost_improvement = 0.005,
       .minimum_relative_cost_improvement = 0.05});
  expect(!baseline.alternatives.empty() &&
             !refined.decoded.alternatives.empty() &&
             refined.selection.accepted &&
             refined.selected_wpm == 20.0 &&
             refined.decoded.alternatives.front().text() == "AE" &&
             baseline.alternatives.front().text() != "AE",
         "completed-turn timing retry improves a wrong acoustic baseline");
}

void testRejectedSelectionHandsBackThePassZeroDecode() {
  // A caller decides whether it may keep the selected pass on evidence the
  // refinement cannot see, and when it refuses it needs pass zero again. Pass
  // zero has already been decoded here, and one decode is a complete walk over
  // every observation of the transmission, so it is handed back rather than
  // left for the caller to repeat.
  cwassistant::core::CwEventLattice lattice;
  std::uint64_t now_ns = 1U;
  appendRun(lattice, true, 60.0, now_ns);
  appendRun(lattice, false, 60.0, now_ns);
  appendRun(lattice, true, 180.0, now_ns);
  appendRun(lattice, false, 120.0, now_ns);
  appendRun(lattice, true, 60.0, now_ns);
  const std::array passes{
      cwassistant::core::CwLatticeTimingPass{12.0, 100.0},
      cwassistant::core::CwLatticeTimingPass{20.0, 60.0}};
  const auto pass_zero = lattice.decode(
      passes.front().dot_ms,
      cwassistant::core::CwLatticeDecodeMode::Flush);
  const auto refined = cwassistant::core::refineCwEventLattice(
      lattice, passes, cwassistant::core::CwLatticeDecodeMode::Flush,
      {.minimum_normalized_cost_improvement = 0.005,
       .minimum_relative_cost_improvement = 0.05});
  expect(refined.selection.accepted && refined.selection.selected_index != 0U,
         "the fixture still selects a pass other than the baseline");
  expect(!refined.baseline.alternatives.empty(),
         "a displaced baseline decode is returned, not discarded");
  expect(refined.baseline.alternatives.front().text() ==
             pass_zero.alternatives.front().text() &&
         refined.baseline.alternatives.front().acoustic_cost ==
             pass_zero.alternatives.front().acoustic_cost &&
         refined.baseline.observations.size() == pass_zero.observations.size(),
         "the returned baseline is exactly what decoding pass zero produces");

  // Nothing was displaced here, so the selected result already is pass zero
  // and there is no second copy to carry.
  const std::array single_pass{passes.front()};
  const auto unrefined = cwassistant::core::refineCwEventLattice(
      lattice, single_pass, cwassistant::core::CwLatticeDecodeMode::Flush);
  expect(!unrefined.selection.accepted &&
             unrefined.baseline.alternatives.empty() &&
             unrefined.baseline.observations.empty(),
         "an unrefined turn carries no duplicate of its own baseline");
}

void testCommittedBoundaryRejectsCrossingRefinement() {
  using cwassistant::core::CwEventLatticeResult;
  using cwassistant::core::CwLatticeAlternative;
  using cwassistant::core::CwLatticeSymbol;
  const auto decoded_span = [](const std::uint64_t first,
                               const std::uint64_t last) {
    CwEventLatticeResult decoded;
    decoded.alternatives.push_back(CwLatticeAlternative{
        .symbols = {CwLatticeSymbol{.symbol = "A",
                                    .elements = ".-",
                                    .first_observation_id = first,
                                    .last_observation_id = last,
                                    .known = true,
                                    .word_boundary_after = false}},
        .provisional_elements = {},
        .provisional_first_observation_id = 0U,
        .provisional_last_observation_id = 0U,
        .acoustic_cost = 0.0,
        .evidence_confidence = 1.0F});
    return decoded;
  };
  const auto crossing = decoded_span(4U, 7U);
  expect(!cwassistant::core::cwLatticeCleanlyExtendsCommit(crossing, 5U),
         "a resegmented symbol crossing the committed watermark fails closed");

  const auto extension = decoded_span(6U, 6U);
  expect(cwassistant::core::cwLatticeCleanlyExtendsCommit(extension, 5U),
         "a symbol wholly right of the watermark may append");

  const auto no_extension = decoded_span(2U, 3U);
  expect(!cwassistant::core::cwLatticeCleanlyExtendsCommit(no_extension, 5U),
         "a selected pass with no new symbol cannot displace the baseline");
}

}  // namespace

int main() {
  testClearAcousticImprovementWins();
  testWeakOrMisalignedPassFailsClosed();
  testSmallGainDoesNotRevise();
  testContradictoryNearTieAbstains();
  testEquivalentNearTieMayAgree();
  testIneligibleContradictionCannotVeto();
  testPassBudgetIsHard();
  testInvalidBaselineCannotBeHidden();
  testLatticeRetryImprovesWrongTimingBaseline();
  testRejectedSelectionHandsBackThePassZeroDecode();
  testCommittedBoundaryRejectsCrossingRefinement();
  std::cout << "cw_acoustic_refinement_tests: PASS\n";
  return EXIT_SUCCESS;
}
