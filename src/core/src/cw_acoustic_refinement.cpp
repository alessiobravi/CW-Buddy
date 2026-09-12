#include "cwassistant/core/cw_acoustic_refinement.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace cwassistant::core {
namespace {

bool validCandidate(const CwAcousticPassCandidate& candidate) noexcept {
  return !candidate.acoustic_signature.empty() &&
         std::isfinite(candidate.acoustic_cost) &&
         candidate.acoustic_cost >= 0.0 &&
         std::isfinite(candidate.evidence_confidence) &&
         candidate.evidence_confidence >= 0.0F &&
         candidate.evidence_confidence <= 1.0F &&
         candidate.observation_count > 0U &&
         candidate.symbol_count > 0U &&
         candidate.evidence_started_ns < candidate.evidence_ended_ns;
}

double normalizedCost(const CwAcousticPassCandidate& candidate) noexcept {
  return candidate.acoustic_cost /
         static_cast<double>(candidate.observation_count);
}

double intervalOverlap(const CwAcousticPassCandidate& left,
                       const CwAcousticPassCandidate& right) noexcept {
  const auto intersection_start =
      std::max(left.evidence_started_ns, right.evidence_started_ns);
  const auto intersection_end =
      std::min(left.evidence_ended_ns, right.evidence_ended_ns);
  if (intersection_end <= intersection_start) return 0.0;
  const auto union_start =
      std::min(left.evidence_started_ns, right.evidence_started_ns);
  const auto union_end =
      std::max(left.evidence_ended_ns, right.evidence_ended_ns);
  if (union_end <= union_start) return 0.0;
  return static_cast<double>(intersection_end - intersection_start) /
         static_cast<double>(union_end - union_start);
}

std::size_t absoluteDifference(const std::size_t left,
                               const std::size_t right) noexcept {
  return left > right ? left - right : right - left;
}

}  // namespace

CwAcousticRefinementSelection selectCwAcousticRefinement(
    const std::span<const CwAcousticPassCandidate> candidates,
    CwAcousticRefinementConfig config) noexcept {
  CwAcousticRefinementSelection result;
  if (candidates.empty()) return result;

  config.maximum_passes =
      std::clamp<std::size_t>(config.maximum_passes, 1U, 32U);
  config.minimum_evidence_confidence = std::clamp(
      std::isfinite(config.minimum_evidence_confidence)
          ? config.minimum_evidence_confidence : 0.40F,
      0.0F, 1.0F);
  config.minimum_interval_overlap = std::clamp(
      std::isfinite(config.minimum_interval_overlap)
          ? config.minimum_interval_overlap : 0.95,
      0.50, 1.0);
  config.minimum_observation_ratio = std::clamp(
      std::isfinite(config.minimum_observation_ratio)
          ? config.minimum_observation_ratio : 0.50,
      0.10, 1.0);
  config.maximum_observation_ratio = std::clamp(
      std::isfinite(config.maximum_observation_ratio)
          ? config.maximum_observation_ratio : 2.00,
      1.0, 10.0);
  config.minimum_normalized_cost_improvement = std::max(
      0.0, std::isfinite(config.minimum_normalized_cost_improvement)
          ? config.minimum_normalized_cost_improvement : 0.02);
  config.minimum_relative_cost_improvement = std::clamp(
      std::isfinite(config.minimum_relative_cost_improvement)
          ? config.minimum_relative_cost_improvement : 0.08,
      0.0, 1.0);
  config.contradictory_pass_margin = std::max(
      0.0, std::isfinite(config.contradictory_pass_margin)
          ? config.contradictory_pass_margin : 0.015);

  const auto& baseline = candidates.front();
  if (!validCandidate(baseline)) {
    result.rejection = CwAcousticRefinementRejection::InvalidBaseline;
    return result;
  }
  if (baseline.evidence_confidence < config.minimum_evidence_confidence) {
    result.rejection = CwAcousticRefinementRejection::InsufficientEvidence;
    return result;
  }

  const std::size_t candidate_count =
      std::min(candidates.size(), config.maximum_passes);
  const double baseline_cost = normalizedCost(baseline);
  const auto eligible = [&](const CwAcousticPassCandidate& candidate) {
    if (!validCandidate(candidate) ||
        candidate.evidence_confidence < config.minimum_evidence_confidence ||
        candidate.evidence_confidence < baseline.evidence_confidence ||
        intervalOverlap(baseline, candidate) <
            config.minimum_interval_overlap ||
        absoluteDifference(baseline.symbol_count, candidate.symbol_count) >
            config.maximum_symbol_count_delta) {
      return false;
    }
    const double observation_ratio =
        static_cast<double>(candidate.observation_count) /
        static_cast<double>(baseline.observation_count);
    return observation_ratio >= config.minimum_observation_ratio &&
           observation_ratio <= config.maximum_observation_ratio;
  };
  double best_cost = baseline_cost;
  std::size_t best_index = 0U;
  for (std::size_t index = 1U; index < candidate_count; ++index) {
    const auto& candidate = candidates[index];
    if (!eligible(candidate)) continue;
    const double candidate_cost = normalizedCost(candidate);
    if (candidate_cost < best_cost) {
      best_cost = candidate_cost;
      best_index = index;
    }
  }

  if (best_index == 0U) {
    result.rejection = CwAcousticRefinementRejection::NoEligibleAlternative;
    return result;
  }
  result.normalized_cost_improvement = baseline_cost - best_cost;
  result.relative_cost_improvement = baseline_cost > 0.0
      ? result.normalized_cost_improvement / baseline_cost
      : 0.0;
  if (result.normalized_cost_improvement <
          config.minimum_normalized_cost_improvement ||
      result.relative_cost_improvement <
          config.minimum_relative_cost_improvement) {
    result.rejection =
        CwAcousticRefinementRejection::InsufficientImprovement;
    return result;
  }

  for (std::size_t index = 1U; index < candidate_count; ++index) {
    if (index == best_index) continue;
    const auto& candidate = candidates[index];
    if (!eligible(candidate) ||
        candidate.acoustic_signature ==
            candidates[best_index].acoustic_signature) {
      continue;
    }
    const double candidate_cost = normalizedCost(candidate);
    if (candidate_cost >= best_cost &&
        candidate_cost - best_cost <= config.contradictory_pass_margin) {
      result.rejection =
          CwAcousticRefinementRejection::AmbiguousImprovement;
      return result;
    }
  }

  result.selected_index = best_index;
  result.accepted = true;
  result.rejection = CwAcousticRefinementRejection::None;
  return result;
}

CwLatticeRefinementResult refineCwEventLattice(
    const CwEventLattice& lattice,
    const std::span<const CwLatticeTimingPass> passes,
    const CwLatticeDecodeMode mode,
    CwAcousticRefinementConfig config) {
  CwLatticeRefinementResult result;
  if (passes.empty()) return result;
  config.maximum_passes =
      std::clamp<std::size_t>(config.maximum_passes, 1U, 32U);
  const std::size_t pass_count = std::min(passes.size(),
                                          config.maximum_passes);
  std::vector<CwEventLatticeResult> decoded_passes;
  std::vector<CwAcousticPassCandidate> candidates;
  decoded_passes.reserve(pass_count);
  candidates.reserve(pass_count);
  for (std::size_t index = 0; index < pass_count; ++index) {
    decoded_passes.push_back(lattice.decode(passes[index].dot_ms, mode));
    const auto& decoded = decoded_passes.back();
    CwAcousticPassCandidate candidate{
        .provenance = {.filter_width_hz = 0.0,
                       .wpm = passes[index].wpm,
                       .character_gap_dots = 0.0,
                       .word_gap_dots = 0.0},
        .acoustic_signature = {},
        .acoustic_cost = 0.0,
        .evidence_confidence = 0.0F,
        .observation_count = 0U,
        .symbol_count = 0U,
        .evidence_started_ns = 0U,
        .evidence_ended_ns = 0U};
    if (!decoded.observations.empty() && !decoded.alternatives.empty()) {
      const auto& alternative = decoded.alternatives.front();
      candidate.acoustic_cost = alternative.acoustic_cost;
      candidate.evidence_confidence = alternative.evidence_confidence;
      candidate.observation_count = decoded.observations.size();
      candidate.symbol_count = alternative.symbols.size();
      candidate.evidence_started_ns =
          decoded.observations.front().started_ns;
      candidate.evidence_ended_ns = decoded.observations.back().ended_ns;
      for (const auto& symbol : alternative.symbols) {
        candidate.acoustic_signature += symbol.known ? symbol.symbol : "?";
        candidate.acoustic_signature.push_back('@');
        candidate.acoustic_signature +=
            std::to_string(symbol.first_observation_id);
        candidate.acoustic_signature.push_back(':');
        candidate.acoustic_signature +=
            std::to_string(symbol.last_observation_id);
        candidate.acoustic_signature.push_back(
            symbol.word_boundary_after ? ' ' : '|');
        if (!symbol.known) {
          candidate.acoustic_signature += symbol.elements;
          candidate.acoustic_signature.push_back('|');
        }
      }
    }
    candidates.push_back(std::move(candidate));
  }
  result.evaluated_passes = pass_count;
  result.selection = selectCwAcousticRefinement(candidates, config);
  const std::size_t selected = result.selection.accepted
      ? result.selection.selected_index : 0U;
  if (selected != 0U) result.baseline = std::move(decoded_passes.front());
  result.decoded = std::move(decoded_passes[selected]);
  result.selected_wpm = passes[selected].wpm;
  return result;
}

bool cwLatticeCleanlyExtendsCommit(
    const CwEventLatticeResult& decoded,
    const std::uint64_t committed_observation_id) noexcept {
  if (decoded.alternatives.empty()) return false;
  bool extends = false;
  for (const auto& symbol : decoded.alternatives.front().symbols) {
    if (symbol.last_observation_id <= committed_observation_id) continue;
    if (symbol.first_observation_id <= committed_observation_id) return false;
    extends = true;
  }
  return extends;
}

}  // namespace cwassistant::core
