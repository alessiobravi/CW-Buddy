#include "cwassistant/core/cw_event_lattice.hpp"
#include "cwassistant/core/cw_morse_alphabet.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace cwassistant::core {
namespace {

std::optional<std::string_view> decodeElements(const std::string_view value) {
  // One alphabet, loaded from data; see cw_morse_alphabet.hpp. This used to be
  // a second verbatim copy of the table in cw_decoder.cpp, which is exactly
  // the kind of duplicate that drifts silently.
  const auto symbol = cwSharedMorseAlphabet().symbolFor(value);
  if (symbol.empty()) return std::nullopt;
  return symbol;
}

double squaredResidual(const double value, const double center,
                       const double scale) noexcept {
  const double normalized = (value - center) / scale;
  return std::min(normalized * normalized, 25.0);
}

double finiteOr(const double value, const double fallback) noexcept {
  return std::isfinite(value) ? value : fallback;
}

double confidenceScale(const float confidence) noexcept {
  // Poor envelope confidence flattens acoustic distinctions but cannot make
  // any path cheaper than well-observed evidence.
  return 0.10 + 0.90 * static_cast<double>(
      std::clamp(confidence, 0.0F, 1.0F));
}

double uncertaintyCost(const float confidence) noexcept {
  return 0.15 * -std::log(std::max(
      static_cast<double>(std::clamp(confidence, 0.0F, 1.0F)), 0.05));
}

enum class GapKind : std::uint8_t { Element, Character, Word };

double gapCost(const double ratio, const float confidence,
               const GapKind kind, const CwEventLatticeConfig& config,
               const double tolerance_scale) {
  double residual = 0.0;
  switch (kind) {
    case GapKind::Element:
      residual = squaredResidual(ratio, 1.0,
                                 0.45 * tolerance_scale);
      break;
    case GapKind::Character:
      residual = std::min(
          squaredResidual(ratio, config.character_gap_dots,
                          0.90 * tolerance_scale),
          0.12 + squaredResidual(
                     ratio, config.compressed_character_gap_dots,
                     0.45 * tolerance_scale));
      break;
    case GapKind::Word:
      residual = std::min(
          squaredResidual(ratio, config.word_gap_dots,
                          1.50 * tolerance_scale),
          0.35 + squaredResidual(
                     ratio, config.compressed_word_gap_dots,
                     tolerance_scale));
      break;
  }
  return confidenceScale(confidence) * residual;
}


// Farnsworth sending keeps element timing at the sender's speed while
// stretching the gaps between characters and words by a common factor. The
// gap centres are therefore not at three and seven dots but at three and seven
// times that factor, and scoring against the fixed centres reads every
// stretched character gap as a word gap: on a Farnsworth fixture the literal
// decoder read the message perfectly while this path split every character
// apart, and the card prefers this path.
//
// The factor is recovered from the gaps themselves. Ordinary text contains far
// more character gaps than word gaps, so the median gap that is clearly longer
// than an element gap is a character gap, and the factor is that median over
// three. Nothing is adapted unless enough such gaps exist and the result stays
// within the range real sending occupies.
double estimateFarnsworthFactor(
    const std::vector<CwRunObservation>& observations,
    const std::size_t first_observation, const double dot_ms,
    const CwEventLatticeConfig& config) {
  std::vector<double> long_gaps;
  for (std::size_t index = first_observation; index < observations.size();
       ++index) {
    const auto& observation = observations[index];
    if (observation.keyed) continue;
    if (observation.confidence < config.minimum_tolerance_observation_confidence)
      continue;
    const double ratio = observation.duration_ms / dot_ms;
    // Clearly beyond an element gap, and short of the longest a character gap
    // could plausibly reach even when heavily stretched.
    if (ratio > 1.8 && ratio < 12.0) long_gaps.push_back(ratio);
  }
  if (long_gaps.size() < config.minimum_adaptive_character_gaps) return 1.0;
  std::ranges::sort(long_gaps);
  const double median = long_gaps[long_gaps.size() / 2];
  const double factor = median / config.character_gap_dots;
  // Below one is ordinary compressed sending, which the existing compressed
  // centres already model; above two and a half is beyond what is sent.
  if (!std::isfinite(factor) || factor < 1.15 || factor > 2.5) return 1.0;
  return factor;
}

double estimateTimingTolerance(
    const std::vector<CwRunObservation>& observations,
    const std::size_t first_observation, const double dot_ms,
    const CwEventLatticeConfig& config) {
  std::vector<double> residuals;
  residuals.reserve(observations.size() - first_observation);
  std::size_t mark_count = 0;
  std::size_t gap_count = 0;
  for (std::size_t index = first_observation; index < observations.size();
       ++index) {
    const auto& observation = observations[index];
    if (observation.confidence <
        config.minimum_tolerance_observation_confidence) {
      continue;
    }
    const double ratio = observation.duration_ms / dot_ms;
    double residual = 0.0;
    if (observation.keyed) {
      residual = std::min(std::abs(ratio - 1.0) / 0.45,
                          std::abs(ratio - 3.0) / 0.85);
      ++mark_count;
    } else {
      residual = std::min({
          std::abs(ratio - 1.0) / 0.45,
          std::abs(ratio - config.compressed_character_gap_dots) / 0.45,
          std::abs(ratio - config.character_gap_dots) / 0.90,
          std::abs(ratio - config.compressed_word_gap_dots) / 1.00,
          std::abs(ratio - config.word_gap_dots) / 1.50,
      });
      ++gap_count;
    }
    if (std::isfinite(residual)) residuals.push_back(residual);
  }

  if (residuals.size() < config.minimum_tolerance_observations ||
      mark_count < 4U || gap_count < 4U) {
    return config.timing_tolerance_scale;
  }
  std::ranges::sort(residuals);
  const auto quantile = [&residuals](const double fraction) {
    const auto last = residuals.size() - 1U;
    const auto index = static_cast<std::size_t>(
        std::floor(fraction * static_cast<double>(last)));
    return residuals[index];
  };
  const double median = quantile(0.50);
  const double upper_quartile = quantile(0.75);
  // A segment whose typical run does not resemble any Morse timing center is
  // not hand-sent variance. Refuse to widen around noise or a wrong dot fit.
  if (median > 1.25) return config.timing_tolerance_scale;
  const double estimated = 1.0 +
      1.25 * std::max(0.0, upper_quartile - 0.20);
  return std::clamp(std::max(config.timing_tolerance_scale, estimated),
                    config.timing_tolerance_scale,
                    config.maximum_timing_tolerance_scale);
}

// Every surviving path used to own a private copy of the symbols it had
// already decoded, and the beam copies paths relentlessly: each mark expands
// every path into two successors, each gap into three. The decoded-symbol
// vector therefore travelled along with every one of those copies, so a single
// decode cost time proportional to the square of the observation count. That
// is what made a long transmission expensive to close rather than a short one,
// and it is the growth an operator sees as the application going jerky after
// the station has been running for a while.
//
// A symbol is immutable the moment a path emits it, so the history can be
// shared instead of copied. Paths hold one index into this chain; emitting
// appends at most one node and copying a path copies one integer.
//
// Sharing alone is not enough, because the beam also has to tell two paths
// apart, and it used to do that by rebuilding a description of the whole
// decoded history as a string and comparing it character by character for
// every surviving path at every pruning step -- the same quadratic shape in a
// second guise. The chain therefore merges equal histories as it is built: a
// symbol appended to a given node returns the node that already holds it if
// one exists, so two paths that agree on every decoded symbol end up holding
// the same index, and one integer comparison decides what the two strings used
// to decide.
//
// What counts as the same symbol is exactly what the old description encoded:
// the rendered text for a decoded character, the raw elements for an unknown
// span, the observation the symbol starts and ends on, and whether a word
// boundary follows. The elements behind a decoded character were deliberately
// not part of it, and are still not.
class SymbolChain {
 public:
  // The empty history. Node zero is a sentinel and is never read back.
  static constexpr std::size_t kEmpty = 0U;

  SymbolChain() { nodes_.emplace_back(); }

  [[nodiscard]] std::size_t append(const std::size_t parent,
                                   CwLatticeSymbol symbol) {
    const std::size_t first_child = nodes_[parent].first_child;
    for (std::size_t child = first_child; child != kEmpty;
         child = nodes_[child].next_sibling) {
      if (sameSymbol(nodes_[child].symbol, symbol)) return child;
    }
    nodes_.push_back({.parent = parent,
                      .next_sibling = first_child,
                      .first_child = kEmpty,
                      .symbol = std::move(symbol)});
    const std::size_t index = nodes_.size() - 1U;
    nodes_[parent].first_child = index;
    return index;
  }

  [[nodiscard]] std::vector<CwLatticeSymbol> collect(
      const std::size_t node) const {
    std::vector<CwLatticeSymbol> symbols;
    for (std::size_t index = node; index != kEmpty;
         index = nodes_[index].parent) {
      symbols.push_back(nodes_[index].symbol);
    }
    std::ranges::reverse(symbols);
    return symbols;
  }

 private:
  struct Node {
    std::size_t parent{kEmpty};
    std::size_t next_sibling{kEmpty};
    std::size_t first_child{kEmpty};
    CwLatticeSymbol symbol;
  };

  static bool sameSymbol(const CwLatticeSymbol& left,
                         const CwLatticeSymbol& right) noexcept {
    return left.known == right.known &&
           left.word_boundary_after == right.word_boundary_after &&
           left.first_observation_id == right.first_observation_id &&
           left.last_observation_id == right.last_observation_id &&
           (left.known ? left.symbol == right.symbol
                       : left.elements == right.elements);
  }

  // Siblings are threaded through the nodes themselves, so growing the chain
  // never allocates anything beyond this one vector.
  std::vector<Node> nodes_;
};

struct Path {
  std::size_t symbols_node{SymbolChain::kEmpty};
  std::string pending_elements;
  std::uint64_t pending_first_observation_id{0};
  std::uint64_t pending_last_observation_id{0};
  double cost{0.0};
};

void emitSymbol(Path& path, SymbolChain& chain, const bool word_boundary,
                const std::uint64_t last_observation_id,
                const bool force_unknown = false) {
  if (path.pending_elements.empty()) return;
  const auto decoded = decodeElements(path.pending_elements);
  path.symbols_node = chain.append(path.symbols_node, {
      .symbol = decoded && !force_unknown ? std::string(*decoded)
                                           : std::string{},
      .elements = path.pending_elements,
      .first_observation_id = path.pending_first_observation_id,
      .last_observation_id = last_observation_id,
      .known = decoded.has_value() && !force_unknown,
      .word_boundary_after = word_boundary,
  });
  path.pending_elements.clear();
}

void prune(std::vector<Path>& paths, const std::size_t beam_width) {
  std::stable_sort(paths.begin(), paths.end(),
                   [](const Path& left, const Path& right) {
                     return left.cost < right.cost;
                   });
  std::vector<Path> unique;
  unique.reserve(std::min(paths.size(), beam_width));
  for (auto& path : paths) {
    // Identity is the decoded history plus the elements not yet committed to a
    // symbol, exactly as before; only its representation changed.
    const bool duplicate = std::ranges::any_of(
        unique, [&path](const Path& kept) {
          return kept.symbols_node == path.symbols_node &&
                 kept.pending_elements == path.pending_elements;
        });
    if (duplicate) continue;
    unique.push_back(std::move(path));
    if (unique.size() >= beam_width) break;
  }
  paths = std::move(unique);
}

}  // namespace

std::string CwLatticeAlternative::text(const char unknown_marker) const {
  std::string result;
  for (const auto& item : symbols) {
    if (item.known) {
      result += item.symbol;
    } else {
      result.push_back(unknown_marker);
    }
    if (item.word_boundary_after &&
        (result.empty() || result.back() != ' ')) {
      result.push_back(' ');
    }
  }
  while (!result.empty() && result.back() == ' ') result.pop_back();
  return result;
}

std::uint64_t cwFixedLagCommitObservationId(
    const std::span<const CwRunObservation> observations,
    const double lag_ms,
    const std::size_t minimum_later_observations) noexcept {
  if (observations.empty() || !std::isfinite(lag_ms) || lag_ms < 0.0 ||
      minimum_later_observations >= observations.size()) {
    return 0U;
  }
  const auto lag_ns = static_cast<std::uint64_t>(std::min<long double>(
      std::ceil(static_cast<long double>(lag_ms) * 1'000'000.0L),
      static_cast<long double>(std::numeric_limits<std::uint64_t>::max())));
  std::uint64_t previous_end_ns = 0U;
  std::uint64_t previous_id = 0U;
  for (const auto& observation : observations) {
    if (observation.observation_id == 0U ||
        observation.observation_id <= previous_id ||
        observation.ended_ns <= observation.started_ns ||
        (previous_end_ns != 0U &&
         observation.started_ns < previous_end_ns)) {
      return 0U;
    }
    previous_id = observation.observation_id;
    previous_end_ns = observation.ended_ns;
  }
  const std::uint64_t latest_end_ns = observations.back().ended_ns;
  std::uint64_t safe_id = 0U;
  for (std::size_t index = 0;
       index + minimum_later_observations < observations.size(); ++index) {
    const auto& observation = observations[index];
    if (observation.ended_ns > latest_end_ns ||
        latest_end_ns - observation.ended_ns < lag_ns) {
      break;
    }
    safe_id = observation.observation_id;
  }
  return safe_id;
}

CwEventLattice::CwEventLattice(CwEventLatticeConfig config)
    : config_(config) {
  config_.maximum_observations =
      std::clamp<std::size_t>(config_.maximum_observations, 16U, 512U);
  config_.beam_width = std::clamp<std::size_t>(config_.beam_width, 2U, 64U);
  config_.maximum_alternatives = std::clamp<std::size_t>(
      config_.maximum_alternatives, 1U, config_.beam_width);
  config_.maximum_elements_per_symbol = std::clamp<std::size_t>(
      config_.maximum_elements_per_symbol, 9U, 32U);
  config_.compressed_character_gap_dots = std::clamp(finiteOr(
      config_.compressed_character_gap_dots, 2.0), 1.4, 2.6);
  config_.character_gap_dots = std::clamp(finiteOr(
      config_.character_gap_dots, 3.0),
      config_.compressed_character_gap_dots + 0.2, 4.0);
  config_.compressed_word_gap_dots = std::clamp(finiteOr(
      config_.compressed_word_gap_dots, 4.5),
      config_.character_gap_dots + 0.3, 6.5);
  config_.word_gap_dots = std::clamp(finiteOr(
      config_.word_gap_dots, 7.0),
      config_.compressed_word_gap_dots + 0.4, 9.0);
  config_.unknown_symbol_cost = std::clamp(finiteOr(
      config_.unknown_symbol_cost, 2.5), 0.5, 12.0);
  config_.timing_tolerance_scale = std::clamp(finiteOr(
      config_.timing_tolerance_scale, 1.0), 0.75, 2.5);
  config_.maximum_timing_tolerance_scale = std::clamp(finiteOr(
      config_.maximum_timing_tolerance_scale, 2.5),
      config_.timing_tolerance_scale, 3.0);
  config_.minimum_tolerance_observations = std::clamp<std::size_t>(
      config_.minimum_tolerance_observations, 8U,
      config_.maximum_observations);
  if (!std::isfinite(config_.minimum_tolerance_observation_confidence)) {
    config_.minimum_tolerance_observation_confidence = 0.55F;
  }
  config_.minimum_tolerance_observation_confidence = std::clamp(
      config_.minimum_tolerance_observation_confidence, 0.25F, 0.95F);
  observations_.reserve(config_.maximum_observations);
}

void CwEventLattice::reset() noexcept {
  observations_.clear();
  input_truncated_ = false;
  rejected_observations_ = 0;
  coalesced_observations_ = 0;
}

std::optional<std::uint64_t> CwEventLattice::append(
    CwRunObservation observation) {
  if (!std::isfinite(observation.duration_ms) ||
      observation.duration_ms <= 0.0) {
    ++rejected_observations_;
    return std::nullopt;
  }
  if (!std::isfinite(observation.confidence)) observation.confidence = 0.0F;
  observation.confidence = std::clamp(observation.confidence, 0.0F, 1.0F);
  const bool has_timestamps = observation.started_ns != 0U ||
                              observation.ended_ns != 0U;
  if (has_timestamps && observation.ended_ns <= observation.started_ns) {
    ++rejected_observations_;
    return std::nullopt;
  }
  if (!observations_.empty()) {
    auto& previous = observations_.back();
    const bool previous_has_timestamps = previous.started_ns != 0U ||
                                         previous.ended_ns != 0U;
    if (has_timestamps != previous_has_timestamps ||
        (has_timestamps && observation.started_ns < previous.ended_ns)) {
      ++rejected_observations_;
      return std::nullopt;
    }
    if (previous.keyed == observation.keyed) {
      if (has_timestamps && observation.started_ns != previous.ended_ns) {
        ++rejected_observations_;
        return std::nullopt;
      }
      const double combined_duration = previous.duration_ms +
                                       observation.duration_ms;
      if (!std::isfinite(combined_duration)) {
        ++rejected_observations_;
        return std::nullopt;
      }
      previous.confidence = static_cast<float>(
          (static_cast<double>(previous.confidence) * previous.duration_ms +
           static_cast<double>(observation.confidence) *
               observation.duration_ms) /
          combined_duration);
      previous.duration_ms = combined_duration;
      if (has_timestamps) previous.ended_ns = observation.ended_ns;
      ++coalesced_observations_;
      return previous.observation_id;
    }
  }
  if (next_observation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    ++rejected_observations_;
    return std::nullopt;
  }
  observation.observation_id = next_observation_id_++;
  if (observations_.size() >= config_.maximum_observations) {
    observations_.erase(observations_.begin());
    input_truncated_ = true;
  }
  observations_.push_back(observation);
  return observation.observation_id;
}

CwEventLatticeResult CwEventLattice::decode(
    const double dot_ms, const CwLatticeDecodeMode mode) const {
  CwEventLatticeResult result{
      .observations = observations_,
      .alternatives = {},
      .input_truncated = input_truncated_,
      .left_prefix_discarded = false,
      .rejected_observations = rejected_observations_,
      .coalesced_observations = coalesced_observations_,
      .effective_timing_tolerance_scale = config_.timing_tolerance_scale,
  };
  if (!std::isfinite(dot_ms) || dot_ms <= 0.0 || observations_.empty()) {
    return result;
  }

  std::size_t first_observation = 0;
  if (input_truncated_) {
    result.left_prefix_discarded = true;
    const double safe_boundary_ratio =
        1.0 + 0.55 * config_.maximum_timing_tolerance_scale;
    bool boundary_found = false;
    for (std::size_t index = 0; index + 1U < observations_.size(); ++index) {
      if (!observations_[index].keyed &&
          observations_[index].duration_ms / dot_ms >= safe_boundary_ratio &&
          observations_[index].confidence >= 0.35F &&
          observations_[index + 1U].keyed) {
        first_observation = index + 1U;
        boundary_found = true;
        break;
      }
    }
    if (!boundary_found) return result;
  }

  const double effective_tolerance = estimateTimingTolerance(
      observations_, first_observation, dot_ms, config_);
  // Scale the character and word centres together, which is exactly how
  // Farnsworth stretches them; element timing is untouched.
  CwEventLatticeConfig spacing = config_;
  const double farnsworth = estimateFarnsworthFactor(
      observations_, first_observation, dot_ms, config_);
  spacing.character_gap_dots *= farnsworth;
  spacing.compressed_character_gap_dots *= farnsworth;
  spacing.word_gap_dots *= farnsworth;
  spacing.compressed_word_gap_dots *= farnsworth;
  result.effective_timing_tolerance_scale = effective_tolerance;
  float confidence_sum = 0.0F;
  std::size_t confidence_count = 0;
  for (std::size_t index = first_observation;
       index < observations_.size(); ++index) {
    confidence_sum += observations_[index].confidence;
    ++confidence_count;
  }
  const float evidence_confidence = confidence_count == 0U
      ? 0.0F
      : std::clamp(
            confidence_sum / static_cast<float>(confidence_count) /
                static_cast<float>(effective_tolerance),
            0.0F, 1.0F);

  SymbolChain chain;
  std::vector<Path> paths(1);
  for (std::size_t observation_index = first_observation;
       observation_index < observations_.size(); ++observation_index) {
    const auto& observation = observations_[observation_index];
    if (!observation.keyed) continue;

    const double ratio = observation.duration_ms / dot_ms;
    const double uncertainty_cost = uncertaintyCost(observation.confidence);
    const double variance_cost =
        0.10 * std::max(0.0, std::log(effective_tolerance));
    const double dot_cost = uncertainty_cost +
                            variance_cost +
                            confidenceScale(observation.confidence) *
                                squaredResidual(
                                    ratio, 1.0,
                                    0.45 * effective_tolerance);
    const double dash_cost = uncertainty_cost +
                             variance_cost +
                             confidenceScale(observation.confidence) *
                                 squaredResidual(
                                     ratio, 3.0,
                                     0.85 * effective_tolerance);
    std::vector<Path> marked;
    marked.reserve(paths.size() * 2U);
    for (const auto& path : paths) {
      if (path.pending_elements.size() >=
          config_.maximum_elements_per_symbol) {
        continue;
      }
      Path dot_path = path;
      if (dot_path.pending_elements.empty()) {
        dot_path.pending_first_observation_id = observation.observation_id;
      }
      dot_path.pending_elements.push_back('.');
      dot_path.pending_last_observation_id = observation.observation_id;
      dot_path.cost += dot_cost;
      marked.push_back(std::move(dot_path));

      Path dash_path = path;
      if (dash_path.pending_elements.empty()) {
        dash_path.pending_first_observation_id = observation.observation_id;
      }
      dash_path.pending_elements.push_back('-');
      dash_path.pending_last_observation_id = observation.observation_id;
      dash_path.cost += dash_cost;
      marked.push_back(std::move(dash_path));
    }
    prune(marked, config_.beam_width);

    std::size_t gap_index = observation_index + 1U;
    while (gap_index < observations_.size() &&
           observations_[gap_index].keyed) {
      ++gap_index;
    }
    if (gap_index >= observations_.size()) {
      paths = std::move(marked);
      continue;
    }
    const auto& gap = observations_[gap_index];
    const double gap_ratio = gap.duration_ms / dot_ms;
    const double gap_uncertainty_cost = uncertaintyCost(gap.confidence) +
        0.10 * std::max(0.0, std::log(effective_tolerance));
    std::vector<Path> expanded;
    expanded.reserve(marked.size() * 3U);
    for (const auto& path : marked) {
      if (path.pending_elements.size() <
          config_.maximum_elements_per_symbol) {
        Path element_path = path;
        element_path.cost += gap_uncertainty_cost + gapCost(
            gap_ratio, gap.confidence, GapKind::Element, spacing,
            effective_tolerance);
        expanded.push_back(std::move(element_path));
      }

      Path character_path = path;
      character_path.cost += gap_uncertainty_cost + gapCost(
          gap_ratio, gap.confidence, GapKind::Character, spacing,
          effective_tolerance);
      const bool character_is_known =
          decodeElements(character_path.pending_elements).has_value();
      if (character_is_known) {
        Path unknown_character_path = character_path;
        unknown_character_path.cost += config_.unknown_symbol_cost;
        emitSymbol(unknown_character_path, chain, false,
                   observation.observation_id, true);
        expanded.push_back(std::move(unknown_character_path));
      }
      emitSymbol(character_path, chain, false, observation.observation_id);
      expanded.push_back(std::move(character_path));

      Path word_path = path;
      word_path.cost += gap_uncertainty_cost + gapCost(
          gap_ratio, gap.confidence, GapKind::Word, spacing,
          effective_tolerance);
      const bool word_is_known =
          decodeElements(word_path.pending_elements).has_value();
      if (word_is_known) {
        Path unknown_word_path = word_path;
        unknown_word_path.cost += config_.unknown_symbol_cost;
        emitSymbol(unknown_word_path, chain, true,
                   observation.observation_id, true);
        expanded.push_back(std::move(unknown_word_path));
      }
      emitSymbol(word_path, chain, true, observation.observation_id);
      expanded.push_back(std::move(word_path));
    }
    prune(expanded, config_.beam_width);
    paths = std::move(expanded);
  }

  std::vector<Path> finalized;
  finalized.reserve(paths.size() * 2U);
  for (auto& path : paths) {
    if (mode == CwLatticeDecodeMode::Flush &&
        !path.pending_elements.empty() &&
        decodeElements(path.pending_elements).has_value()) {
      Path unknown_path = path;
      unknown_path.cost += config_.unknown_symbol_cost;
      emitSymbol(unknown_path, chain, false,
                 unknown_path.pending_last_observation_id, true);
      finalized.push_back(std::move(unknown_path));
    }
    if (mode == CwLatticeDecodeMode::Flush &&
        !path.pending_elements.empty()) {
      emitSymbol(path, chain, false, path.pending_last_observation_id);
    }
    finalized.push_back(std::move(path));
  }
  prune(finalized, config_.beam_width);
  paths = std::move(finalized);
  const std::size_t count = std::min(paths.size(),
                                     config_.maximum_alternatives);
  result.alternatives.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    result.alternatives.push_back({
        .symbols = chain.collect(paths[index].symbols_node),
        .provisional_elements = std::move(paths[index].pending_elements),
        .provisional_first_observation_id =
            paths[index].pending_first_observation_id,
        .provisional_last_observation_id =
            paths[index].pending_last_observation_id,
        .acoustic_cost = paths[index].cost,
        .evidence_confidence = evidence_confidence,
    });
  }
  return result;
}

std::size_t CwEventLattice::observationCount() const noexcept {
  return observations_.size();
}

std::size_t CwEventLattice::stateBytes() const noexcept {
  return sizeof(*this) +
         observations_.capacity() * sizeof(CwRunObservation);
}

}  // namespace cwassistant::core
