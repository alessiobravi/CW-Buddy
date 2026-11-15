#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "cwassistant/core/callsign_policy.hpp"

namespace cwassistant::core {

enum class TransmitState {
  Disarmed,
  Armed,
  AwaitingConfirmation,
  Confirmed,
  Transmitting,
  Tuning,
  Fault,
};

class TransmitGuard {
 public:
  explicit TransmitGuard(const CallsignPolicy& callsign_policy) noexcept
      : callsign_policy_(callsign_policy) {}

  [[nodiscard]] TransmitState state() const noexcept { return state_; }
  [[nodiscard]] std::string_view state_name() const noexcept;
  [[nodiscard]] std::string_view pending_callsign() const noexcept {
    return pending_callsign_;
  }
  [[nodiscard]] std::string_view pending_message() const noexcept {
    return pending_message_;
  }
  [[nodiscard]] bool message_confirmed() const noexcept {
    return message_confirmed_;
  }
  // Authoritative observed line state, not intent to transmit or tune. Only
  // observe_key_state() moves it, in either direction: a state change is not
  // an observation, so neither a latched fault nor a disarm may claim the
  // line was released when nothing saw it released.
  [[nodiscard]] bool key_down() const noexcept { return key_down_; }

  [[nodiscard]] bool arm() noexcept;
  // Returns to Disarmed from any state except Fault, and clears the staged
  // QSO in every state including Fault. A latched fault is deliberately not
  // an exit this offers: reset_fault() is the only one, so that an
  // application wiring its shutdown -- or a settings change -- to disarm
  // cannot silently discharge a fault on the operator's behalf. Callers that
  // need to know whether the guard is now disarmed read state().
  void disarm() noexcept;
  [[nodiscard]] bool request_qso(std::string callsign);
  [[nodiscard]] bool confirm(std::string_view callsign);
  // Stages operator-authored text for an exact preview confirmation. Decoder
  // output has no route to this API and cannot arm or begin transmission.
  [[nodiscard]] bool stage_message(std::string message);
  [[nodiscard]] bool confirm_message(std::string_view message);
  [[nodiscard]] bool begin_transmission();
  // TUNE is an operator-only, armed action. It bypasses message/callsign
  // preparation but has its own hard, shorter-than-human-intervention
  // watchdog and shares the emergency-release fault path. Starting it voids
  // any exact preview confirmation: TUNE can be started from a confirmed QSO
  // and may well move the rig, so the staged text has to be confirmed again
  // before it can be transmitted.
  [[nodiscard]] bool begin_tune() noexcept;
  // Feeds the independent continuous-KEY watchdog. An asserted line outside a
  // guarded transmission or beyond the fixed deadline trips a latched fault.
  [[nodiscard]] bool observe_key_state(bool key_down,
                                       std::uint64_t timestamp_ns) noexcept;
  [[nodiscard]] bool finish_transmission() noexcept;
  [[nodiscard]] bool finish_tune() noexcept;
  [[nodiscard]] bool end_qso() noexcept;
  void emergency_release() noexcept;
  void trip_fault() noexcept;
  [[nodiscard]] bool reset_fault() noexcept;

  static constexpr std::uint64_t kMaximumContinuousKeyDownNs =
      3'000'000'000ULL;
  static constexpr std::uint64_t kMaximumTuneKeyDownNs = 15'000'000'000ULL;

 private:
  [[nodiscard]] static std::string normalize_message(
      std::string_view message);

  const CallsignPolicy& callsign_policy_;
  TransmitState state_{TransmitState::Disarmed};
  std::string pending_callsign_{};
  std::string pending_message_{};
  bool message_confirmed_{false};
  bool key_down_{false};
  std::uint64_t key_down_since_ns_{0};
  TransmitState tune_return_state_{TransmitState::Armed};
};

}  // namespace cwassistant::core
