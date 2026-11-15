#include "cwassistant/core/transmit_guard.hpp"

#include "cwassistant/core/cw_transmit_encoder.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace cwassistant::core {

std::string TransmitGuard::normalize_message(const std::string_view message) {
  constexpr std::string_view supported_punctuation = ".,?'/!()&:;=+-\"$@";
  constexpr std::size_t maximum_message_characters = 512;
  std::string result;
  result.reserve(std::min(message.size(), maximum_message_characters));
  bool pending_space = false;
  for (std::size_t index = 0; index < message.size(); ++index) {
    const unsigned char raw = static_cast<unsigned char>(message[index]);
    if (std::isspace(raw) != 0) {
      pending_space = !result.empty();
      continue;
    }
    if (raw > 0x7fU) return {};
    const char character = static_cast<char>(std::toupper(raw));
    // A bracketed token is admitted only when it names a prosign this
    // application may transmit. Passing the brackets through and leaving the
    // decision to the encoder would let arbitrary bracketed text survive into
    // a staged message, so the whole token is judged here, where every other
    // character is judged.
    if (character == '<') {
      const auto close = message.find('>', index + 1U);
      if (close == std::string_view::npos) return {};
      std::string name;
      for (std::size_t scan = index + 1U; scan < close; ++scan) {
        const unsigned char inner = static_cast<unsigned char>(message[scan]);
        if (inner > 0x7fU) return {};
        name.push_back(static_cast<char>(std::toupper(inner)));
      }
      if (!CwTransmitEncoder::is_transmittable_prosign(name)) return {};
      if (pending_space) result.push_back(' ');
      pending_space = false;
      result.push_back('<');
      result.append(name);
      result.push_back('>');
      if (result.size() > maximum_message_characters) return {};
      index = close;
      continue;
    }
    const bool supported = (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') ||
        supported_punctuation.find(character) != std::string_view::npos;
    if (!supported) return {};
    if (pending_space) result.push_back(' ');
    pending_space = false;
    result.push_back(character);
    if (result.size() > maximum_message_characters) return {};
  }
  return result;
}

std::string_view TransmitGuard::state_name() const noexcept {
  switch (state_) {
    case TransmitState::Disarmed: return "disarmed";
    case TransmitState::Armed: return "armed";
    case TransmitState::AwaitingConfirmation: return "awaiting-confirmation";
    case TransmitState::Confirmed: return "confirmed";
    case TransmitState::Transmitting: return "transmitting";
    case TransmitState::Tuning: return "tuning";
    case TransmitState::Fault: return "fault";
  }
  return "unknown";
}

bool TransmitGuard::arm() noexcept {
  if (state_ != TransmitState::Disarmed) {
    return false;
  }
  state_ = TransmitState::Armed;
  return true;
}

void TransmitGuard::disarm() noexcept {
  // Nothing staged survives a disarm, in any state, including a latched
  // fault. trip_fault() already clears these, so this is belt and braces:
  // no future path may leave a confirmed message waiting behind a fault for
  // whatever clears it.
  pending_callsign_.clear();
  pending_message_.clear();
  message_confirmed_ = false;
  // A latched fault is left latched. reset_fault() is the single deliberate
  // way out of Fault, and a disarm must not become a second, unannounced one
  // -- least of all the disarm an application wires to its own shutdown, or
  // one a settings change makes on the operator's behalf. Returning here
  // rather than asserting a state keeps that rule in the guard instead of in
  // the discipline of every caller.
  if (state_ == TransmitState::Fault) return;
  // The observed line state is deliberately not touched. key_down_ is what
  // the line was last seen doing, and only observe_key_state() is entitled to
  // say that it changed.
  state_ = TransmitState::Disarmed;
}

bool TransmitGuard::request_qso(std::string callsign) {
  const auto normalized = CallsignPolicy::normalize(callsign);
  if (state_ != TransmitState::Armed || !normalized.has_value() ||
      callsign_policy_.is_ignored(*normalized)) {
    return false;
  }
  pending_callsign_ = *normalized;
  state_ = TransmitState::AwaitingConfirmation;
  return true;
}

bool TransmitGuard::confirm(const std::string_view callsign) {
  const auto normalized = CallsignPolicy::normalize(callsign);
  if (state_ != TransmitState::AwaitingConfirmation || !normalized.has_value() ||
      *normalized != pending_callsign_) {
    return false;
  }
  if (callsign_policy_.is_ignored(pending_callsign_)) {
    pending_callsign_.clear();
    state_ = TransmitState::Armed;
    return false;
  }
  state_ = TransmitState::Confirmed;
  return true;
}

bool TransmitGuard::stage_message(std::string message) {
  if (state_ != TransmitState::Confirmed) return false;
  std::string normalized = normalize_message(message);
  if (normalized.empty()) return false;
  pending_message_ = std::move(normalized);
  message_confirmed_ = false;
  return true;
}

bool TransmitGuard::confirm_message(const std::string_view message) {
  if (state_ != TransmitState::Confirmed || pending_message_.empty())
    return false;
  const std::string normalized = normalize_message(message);
  message_confirmed_ = !normalized.empty() && normalized == pending_message_;
  return message_confirmed_;
}

bool TransmitGuard::begin_transmission() {
  if (state_ != TransmitState::Confirmed || pending_message_.empty() ||
      !message_confirmed_) {
    return false;
  }
  if (callsign_policy_.is_ignored(pending_callsign_)) {
    pending_callsign_.clear();
    pending_message_.clear();
    message_confirmed_ = false;
    state_ = TransmitState::Armed;
    return false;
  }
  key_down_ = false;
  key_down_since_ns_ = 0;
  state_ = TransmitState::Transmitting;
  return true;
}

bool TransmitGuard::begin_tune() noexcept {
  if (state_ != TransmitState::Armed && state_ != TransmitState::Confirmed)
    return false;
  tune_return_state_ = state_;
  // Entering TUNE voids an exact preview confirmation. TUNE is reachable from
  // Confirmed, it keys the transmitter, and an operator reaches for it to
  // work on the antenna system or to move the rig -- so the transmission the
  // confirmation described is no longer the transmission that would go out.
  // The staged text is kept, because retyping it is not what the second look
  // is for: the operator confirms the preview again and transmits.
  message_confirmed_ = false;
  key_down_ = false;
  key_down_since_ns_ = 0;
  state_ = TransmitState::Tuning;
  return true;
}

bool TransmitGuard::observe_key_state(
    const bool key_down, const std::uint64_t timestamp_ns) noexcept {
  if (state_ != TransmitState::Transmitting &&
      state_ != TransmitState::Tuning) {
    // An observation outside a guarded transmission is still an observation,
    // and it is the only thing that may move key_down_. Recording the release
    // here is what lets a latched fault stop reporting a keyed line once the
    // line is actually seen to drop, now that trip_fault() no longer claims
    // that on its own.
    if (!key_down) {
      key_down_ = false;
      key_down_since_ns_ = 0;
      return true;
    }
    if (!key_down_) {
      key_down_ = true;
      key_down_since_ns_ = timestamp_ns;
    }
    trip_fault();
    return false;
  }
  if (!key_down) {
    key_down_ = false;
    key_down_since_ns_ = 0;
    return true;
  }
  if (!key_down_) {
    key_down_ = true;
    key_down_since_ns_ = timestamp_ns;
    return true;
  }
  const std::uint64_t deadline = state_ == TransmitState::Tuning
      ? kMaximumTuneKeyDownNs : kMaximumContinuousKeyDownNs;
  if (timestamp_ns < key_down_since_ns_ ||
      timestamp_ns - key_down_since_ns_ > deadline) {
    trip_fault();
    return false;
  }
  return true;
}

bool TransmitGuard::finish_transmission() noexcept {
  if (state_ != TransmitState::Transmitting) {
    return false;
  }
  key_down_ = false;
  key_down_since_ns_ = 0;
  pending_message_.clear();
  message_confirmed_ = false;
  // Exact callsign confirmation remains valid for this QSO only. A later
  // message still needs its own exact preview confirmation.
  state_ = TransmitState::Confirmed;
  return true;
}

bool TransmitGuard::finish_tune() noexcept {
  if (state_ != TransmitState::Tuning) return false;
  key_down_ = false;
  key_down_since_ns_ = 0;
  state_ = tune_return_state_;
  return true;
}

bool TransmitGuard::end_qso() noexcept {
  if (state_ != TransmitState::Confirmed) return false;
  pending_callsign_.clear();
  pending_message_.clear();
  message_confirmed_ = false;
  state_ = TransmitState::Armed;
  return true;
}

void TransmitGuard::emergency_release() noexcept { trip_fault(); }

void TransmitGuard::trip_fault() noexcept {
  pending_callsign_.clear();
  pending_message_.clear();
  message_confirmed_ = false;
  // The observed line state is left exactly as it was observed. Clearing it
  // here reported an unkeyed line at the one moment the guard has most
  // reason to believe the opposite: the continuous-KEY watchdog trips
  // precisely because the line was seen asserted and never seen released, so
  // "KEY is down" is the fault's most important fact, not something to erase
  // on the way into it. It clears when observe_key_state() sees the line
  // drop, and not before.
  state_ = TransmitState::Fault;
}

bool TransmitGuard::reset_fault() noexcept {
  if (state_ != TransmitState::Fault) {
    return false;
  }
  state_ = TransmitState::Disarmed;
  return true;
}

}  // namespace cwassistant::core
