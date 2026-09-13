#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace cwassistant::core {

// What the operator is doing, which decides whose callsign a monitored stream
// is expected to carry.
enum class CwOperatorRole : std::uint8_t {
  // No assumption. Exchange context alone ranks the candidates, as it always
  // has; the only role knowledge applied is that the operator's own call
  // cannot label another station's stream.
  Monitor,
  // The operator hunts stations that are calling. The monitored stream is a
  // runner, so the call in runner-identifying context is the one to show.
  SearchAndPounce,
  // The operator calls and others answer. The monitored stream is a caller,
  // whose own call is typically sent bare and repeated rather than introduced.
  Runner,
};

[[nodiscard]] constexpr std::string_view cwOperatorRoleName(
    const CwOperatorRole role) noexcept {
  switch (role) {
    case CwOperatorRole::SearchAndPounce: return "search-and-pounce";
    case CwOperatorRole::Runner: return "runner";
    case CwOperatorRole::Monitor: break;
  }
  return "monitor";
}

[[nodiscard]] constexpr CwOperatorRole cwOperatorRoleFromName(
    const std::string_view name,
    const CwOperatorRole fallback = CwOperatorRole::Monitor) noexcept {
  if (name == cwOperatorRoleName(CwOperatorRole::SearchAndPounce))
    return CwOperatorRole::SearchAndPounce;
  if (name == cwOperatorRoleName(CwOperatorRole::Runner))
    return CwOperatorRole::Runner;
  if (name == cwOperatorRoleName(CwOperatorRole::Monitor))
    return CwOperatorRole::Monitor;
  return fallback;
}

class CallsignPolicy {
 public:
  // Initial policy deliberately supports exact callsigns only. Pattern and
  // prefix rules can hide unintended stations and require a separate design.
  [[nodiscard]] bool add_ignored(std::string_view callsign);
  [[nodiscard]] bool remove_ignored(std::string_view callsign);
  [[nodiscard]] bool is_ignored(std::string_view callsign) const;
  [[nodiscard]] std::vector<std::string> ignored_callsigns() const;

  [[nodiscard]] static std::optional<std::string> normalize(
      std::string_view callsign);
  [[nodiscard]] static std::optional<std::string> latest_in_text(
      std::string_view decoded_text);
  [[nodiscard]] static std::optional<std::string> latest_complete_in_text(
      std::string_view stable_text);
  // Recognizes the high-confidence ordinary-QSO handover CALL1 DE CALL2.
  // Both tokens must already be complete, structurally plausible callsigns;
  // this identifies participants but does not choose which one is transmitting.
  [[nodiscard]] static std::vector<std::string> qso_participants_in_text(
      std::string_view stable_text);
  // Identifies who is sending only when the decoded words contain an
  // explicit handover. In ordinary CW, "CALL1 DE CALL2" is transmitted by
  // CALL2 to CALL1. A bare or repeated call is useful identification evidence
  // but is not enough to attribute the current turn.
  [[nodiscard]] static std::optional<std::string> strong_sender_in_text(
      std::string_view stable_text);
  // Returns a completed, structurally plausible callsign only when decoded
  // word context or exact repetition makes a random call-shaped token
  // unlikely. Intended for automatic stream labels, not operator input.
  [[nodiscard]] static std::optional<std::string> best_complete_in_text(
      std::string_view stable_text);
  // The same, told what the operator is doing and what their own call is.
  //
  // Exchange context alone cannot always say which station a call belongs to.
  // TU precedes a runner identifying itself ("TU IU0LFQ") and equally the
  // station it has just worked ("TU DL1NKB"), and both score the same, so a
  // run can label the worked station instead of the one being listened to.
  // The operator's role settles it: searching and pouncing, the monitored
  // stream is a runner, so the call in runner-identifying context is the one
  // to show; running, the monitored stream is someone answering, so a call
  // that simply repeats itself is.
  //
  // A call the operator's own station sends is never a label for somebody
  // else's stream, whatever the role.
  [[nodiscard]] static std::optional<std::string> best_complete_in_text(
      std::string_view stable_text, CwOperatorRole role,
      std::string_view own_callsign);
  // Reconciles the independent literal and lattice-refined paths. A call seen
  // only in refinement must occur as an exact decoded token or have explicit
  // sender evidence; splitting a glued prosign is not enough by itself.
  //
  // A complete callsign standing as a whole token in both texts is credited as
  // evidence in its own right, above any positional rule short of an explicit
  // DE handover. The two paths are independent readings of one transmission,
  // so both producing the same call says something about the signal, while
  // standing next to a CQ only says where a call was expected to sit. On a
  // recorded pileup the calling station's EH3ST stood whole in both texts and
  // scored nothing, while EH4ST -- one element wrong, and third after a CQ --
  // scored enough to name the stream.
  [[nodiscard]] static std::optional<std::string>
  best_complete_in_parallel_texts(std::string_view primary_text,
                                  std::string_view refined_text,
                                  CwOperatorRole role,
                                  std::string_view own_callsign);

 private:
  std::unordered_set<std::string> ignored_;
};

}  // namespace cwassistant::core
