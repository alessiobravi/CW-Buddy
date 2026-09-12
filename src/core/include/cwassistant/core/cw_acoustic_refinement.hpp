#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cwassistant/core/cw_event_lattice.hpp"

namespace cwassistant::core {

// Provenance for one bounded re-decode of the same receiver interval. A caller
// may vary its narrowband filter, timing seed, or spacing model, but no
// language/callsign/context score belongs at this boundary.
struct CwAcousticPassProvenance {
  double filter_width_hz{0.0};
  double wpm{0.0};
  double character_gap_dots{0.0};
  double word_gap_dots{0.0};
};

// Summary of a complete acoustic pass. acoustic_signature must encode decoded
// elements, unknown spans, boundaries, and their observation alignment; plain
// display text is intentionally insufficient because two different physical
// explanations can render the same string.
struct CwAcousticPassCandidate {
  CwAcousticPassProvenance provenance;
  std::string acoustic_signature;
  double acoustic_cost{0.0};
  float evidence_confidence{0.0F};
  std::size_t observation_count{0};
  std::size_t symbol_count{0};
  std::uint64_t evidence_started_ns{0};
  std::uint64_t evidence_ended_ns{0};
};

enum class CwAcousticRefinementRejection : std::uint8_t {
  None,
  MissingBaseline,
  InvalidBaseline,
  NoEligibleAlternative,
  InsufficientEvidence,
  InsufficientImprovement,
  AmbiguousImprovement,
};

struct CwAcousticRefinementConfig {
  std::size_t maximum_passes{12};
  float minimum_evidence_confidence{0.40F};
  double minimum_interval_overlap{0.95};
  double minimum_observation_ratio{0.50};
  double maximum_observation_ratio{2.00};
  double minimum_normalized_cost_improvement{0.02};
  double minimum_relative_cost_improvement{0.08};
  // Contradictory passes inside this normalized-cost distance are unresolved.
  double contradictory_pass_margin{0.015};
  std::size_t maximum_symbol_count_delta{2};
};

struct CwAcousticRefinementSelection {
  std::size_t selected_index{0};
  bool accepted{false};
  double normalized_cost_improvement{0.0};
  double relative_cost_improvement{0.0};
  CwAcousticRefinementRejection rejection{
      CwAcousticRefinementRejection::MissingBaseline};
};

// Candidate zero is always the causal/live baseline. At most maximum_passes
// candidates are examined, making refinement independently load-sheddable.
// A different pass is accepted only when it covers substantially the same
// physical interval, retains evidence confidence, and improves normalized
// acoustic cost by both absolute and relative margins. Close contradictory
// winners abstain. No text, callsign, database, or conversation prior enters
// this decision.
[[nodiscard]] CwAcousticRefinementSelection selectCwAcousticRefinement(
    std::span<const CwAcousticPassCandidate> candidates,
    CwAcousticRefinementConfig config = {}) noexcept;

struct CwLatticeTimingPass {
  double wpm{0.0};
  double dot_ms{0.0};
};

struct CwLatticeRefinementResult {
  CwEventLatticeResult decoded;
  // Pass zero's own decode, retained only when a later pass was selected over
  // it. A caller can reject the selection on evidence this function cannot
  // see -- an append-only commit boundary, for one -- and would then have to
  // decode the baseline all over again, which is a full pass over every
  // observation of the transmission. Empty whenever `decoded` already is pass
  // zero, so nothing is copied when nothing was displaced.
  CwEventLatticeResult baseline;
  double selected_wpm{0.0};
  CwAcousticRefinementSelection selection;
  std::size_t evaluated_passes{0};
};

// Re-decodes one immutable bounded run lattice with nearby timing hypotheses.
// Pass zero is the live baseline. This is suitable only at a coarse checkpoint
// or completed-turn boundary; it does not retain or reconstruct filter audio.
[[nodiscard]] CwLatticeRefinementResult refineCwEventLattice(
    const CwEventLattice& lattice,
    std::span<const CwLatticeTimingPass> passes,
    CwLatticeDecodeMode mode,
    CwAcousticRefinementConfig config = {});

// A selected completed-turn pass may append only symbols whose entire
// observation span lies to the right of the last committed symbol. Returning
// false tells the caller to retain its baseline pass instead.
[[nodiscard]] bool cwLatticeCleanlyExtendsCommit(
    const CwEventLatticeResult& decoded,
    std::uint64_t committed_observation_id) noexcept;

}  // namespace cwassistant::core
