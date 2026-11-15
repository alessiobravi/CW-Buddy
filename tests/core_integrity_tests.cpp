// Integrity tests for three core contracts that nothing else pins down: the
// single deliberate exit from a latched transmit fault, the shared
// dictionaries' first-use load, and the ADI length prefix that makes escaping
// unnecessary.
//
// The order of the cases in main() is part of the fixture. Two of them have to
// be the first thing in the process that touches their shared table, because
// what they assert is what happens on first use, and nothing can un-touch a
// shared singleton afterwards.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "cwassistant/core/adif.hpp"
#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/cw_callsign_prefixes.hpp"
#include "cwassistant/core/cw_morse_alphabet.hpp"
#include "cwassistant/core/transmit_guard.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

// setenv and unsetenv are POSIX and do not exist under MSVC, which provides
// _putenv_s instead; there an empty value removes the variable. These tests
// link no Qt, so qputenv is not available here.
void setDictionaryDirectoryEnvironment(const char* value) {
#if defined(_WIN32)
  static_cast<void>(
      _putenv_s("CWA_DICTIONARY_DIR", value == nullptr ? "" : value));
#else
  if (value == nullptr) {
    static_cast<void>(unsetenv("CWA_DICTIONARY_DIR"));
  } else {
    static_cast<void>(setenv("CWA_DICTIONARY_DIR", value, 1));
  }
#endif
}

// ---------------------------------------------------------------------------
// Shared dictionaries: one load, however many threads arrive at once.
// ---------------------------------------------------------------------------

void test_shared_prefix_table_first_use_is_thread_safe() {
  // First use of the shared prefix table in this process, from several
  // threads released together. The table is lazily loaded, so every one of
  // them finds it empty and the unguarded version of this code had all of
  // them building the same std::vector at the same time. A host that loads
  // the dictionaries eagerly hides this; a headless tool or a test does not.
  constexpr unsigned kThreads = 16;
  std::atomic<unsigned> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::size_t> observed_sizes(kThreads, 0);
  std::vector<char> admitted_real(kThreads, 0);
  std::vector<char> admitted_invented(kThreads, 1);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  try {
    for (unsigned index = 0; index < kThreads; ++index) {
      threads.emplace_back([&, index]() {
        ready.fetch_add(1U, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto& table = cwassistant::core::cwSharedCallsignPrefixes();
        observed_sizes[index] = table.size();
        admitted_real[index] =
            table.isAllocatedPrefix("IU0LFQ") ? char{1} : char{0};
        admitted_invented[index] =
            table.isAllocatedPrefix("QQ1ABC") ? char{1} : char{0};
      });
    }
  } catch (const std::system_error& error) {
    // A host that cannot start threads cannot exercise a concurrency
    // contract. Say so loudly rather than reporting a pass this run did not
    // earn, and leave the remaining cases to carry the verdict.
    for (auto& thread : threads) {
      if (thread.joinable()) thread.join();
    }
    std::cerr << "NOTE: concurrent first use was not exercised on this host: "
              << error.what() << '\n';
    return;
  }
  while (ready.load(std::memory_order_acquire) < kThreads)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();

  const auto& table = cwassistant::core::cwSharedCallsignPrefixes();
  bool sizes_agree = true;
  bool lookups_agree = true;
  for (unsigned index = 0; index < kThreads; ++index) {
    sizes_agree = sizes_agree && observed_sizes[index] == table.size();
    lookups_agree = lookups_agree && admitted_real[index] == char{1} &&
        admitted_invented[index] == char{0};
  }
  expect(table.size() >= 308U,
         "the shared prefix table loads its blocks on first use");
  expect(sizes_agree,
         "every thread that first used the shared prefix table together saw "
         "the same finished table");
  expect(lookups_agree,
         "a prefix lookup during concurrent first use still separates a real "
         "prefix from an invented one");
}

void test_morse_alphabet_origin_is_answered_for_the_alphabet_in_use() {
  // First use of the shared alphabet in this process, and deliberately
  // through the diagnostic rather than through the alphabet itself. The
  // question is where the alphabet the next decode will use came from, so
  // asking it before anything has loaded one must not answer "a file loaded
  // fine" -- which is exactly what "not the compiled-in copy" means to the
  // operator reading it.
  const bool reported_before_use =
      cwassistant::core::cwMorseAlphabetLoadedFromBuiltin();
  const auto& alphabet = cwassistant::core::cwSharedMorseAlphabet();
  expect(alphabet.size() >= 56U,
         "the shared alphabet recovers its symbols with no dictionary "
         "directory at all");
  expect(reported_before_use,
         "asking where the alphabet came from before anything used it reports "
         "the compiled-in copy the next decode will read, not a file load that "
         "never happened");
  expect(reported_before_use ==
             cwassistant::core::cwMorseAlphabetLoadedFromBuiltin(),
         "the reported origin of the alphabet does not change merely because "
         "the alphabet was then used");
}

void test_prefix_origin_is_answered_for_the_table_in_use() {
  // The same question on the other table, which the concurrency case above
  // has already loaded, so the fixture has to clear it first. A cleared table
  // is the state the application itself passes through at startup.
  auto& table = cwassistant::core::cwMutableSharedCallsignPrefixes();
  table.clear();
  const bool reported_before_use =
      cwassistant::core::cwCallsignPrefixesLoadedFromBuiltin();
  const auto& recovered = cwassistant::core::cwSharedCallsignPrefixes();
  expect(recovered.size() >= 308U,
         "the shared prefix table recovers after a host clears it");
  expect(reported_before_use,
         "asking where the prefix table came from reports the compiled-in copy "
         "the next lookup will read, rather than reporting a file load against "
         "an empty table");
  expect(reported_before_use ==
             cwassistant::core::cwCallsignPrefixesLoadedFromBuiltin(),
         "the reported origin of the prefix table does not change merely "
         "because the table was then used");
}

// ---------------------------------------------------------------------------
// Transmit guard: a latched fault has exactly one exit.
// ---------------------------------------------------------------------------

using cwassistant::core::CallsignPolicy;
using cwassistant::core::TransmitGuard;
using cwassistant::core::TransmitState;

// Drives a fresh guard to a confirmed QSO with an exactly confirmed message,
// which is the state every case below starts from.
[[nodiscard]] bool reach_confirmed_message(TransmitGuard& guard) {
  return guard.arm() && guard.request_qso("i1abc") && guard.confirm("I1ABC") &&
      guard.stage_message("tu <sk>") && guard.confirm_message("TU <SK>") &&
      guard.state() == TransmitState::Confirmed && guard.message_confirmed();
}

void test_disarm_cannot_clear_a_latched_fault() {
  CallsignPolicy policy;
  TransmitGuard guard(policy);
  expect(reach_confirmed_message(guard),
         "fixture reaches a confirmed QSO with an exactly confirmed message");
  guard.trip_fault();
  expect(guard.state() == TransmitState::Fault,
         "fixture latches a fault");
  guard.disarm();
  expect(guard.state() == TransmitState::Fault,
         "disarm does not clear a latched fault");
  expect(!guard.arm(),
         "a fault cannot be armed out of through disarm, which is what makes "
         "reset_fault the single deliberate exit");
  expect(guard.pending_callsign().empty() && guard.pending_message().empty() &&
             !guard.message_confirmed(),
         "no confirmed message survives a disarm taken in a latched fault");
  expect(guard.reset_fault() && guard.state() == TransmitState::Disarmed,
         "reset_fault remains the one way out of a latched fault");

  // The ordinary exits are unaffected: disarm is still an unconditional way
  // back to Disarmed from every state that is not a fault.
  expect(guard.arm() && guard.state() == TransmitState::Armed,
         "the guard arms again after an explicit fault reset");
  guard.disarm();
  expect(guard.state() == TransmitState::Disarmed,
         "disarm still disarms an armed guard");
  expect(reach_confirmed_message(guard),
         "the guard reaches a confirmed message again");
  guard.disarm();
  expect(guard.state() == TransmitState::Disarmed &&
             guard.pending_message().empty() && !guard.message_confirmed(),
         "disarm still disarms a confirmed QSO and clears what was staged");
}

void test_latched_fault_keeps_the_observed_key_state() {
  CallsignPolicy policy;
  TransmitGuard guard(policy);
  expect(reach_confirmed_message(guard) && guard.begin_transmission(),
         "fixture reaches a guarded transmission");
  expect(guard.observe_key_state(true, 1'000'000'000ULL) && guard.key_down(),
         "the guard observes the KEY assertion");
  expect(!guard.observe_key_state(
             true, 1'000'000'000ULL +
                       TransmitGuard::kMaximumContinuousKeyDownNs + 1ULL) &&
             guard.state() == TransmitState::Fault,
         "continuous KEY beyond the deadline latches a fault");
  expect(guard.key_down(),
         "a watchdog trip does not report an unkeyed line: the fault exists "
         "because the line was last seen asserted and never seen released");
  expect(guard.observe_key_state(false, 9'000'000'000ULL),
         "a KEY release is still observed while the fault is latched");
  expect(!guard.key_down(),
         "an observed release, and only an observed release, clears the "
         "reported line state");

  // A disarm is not an observation either.
  expect(guard.reset_fault() && reach_confirmed_message(guard) &&
             guard.begin_transmission(),
         "fixture reaches a guarded transmission again");
  expect(guard.observe_key_state(true, 20'000'000'000ULL) && guard.key_down(),
         "the guard observes the second KEY assertion");
  guard.disarm();
  expect(guard.state() == TransmitState::Disarmed && guard.key_down(),
         "disarming does not claim the line was released");
  expect(guard.observe_key_state(false, 20'100'000'000ULL) &&
             !guard.key_down(),
         "the release observed after a disarm clears the reported line state");
}

void test_tune_voids_the_exact_preview_confirmation() {
  CallsignPolicy policy;
  TransmitGuard guard(policy);
  expect(reach_confirmed_message(guard),
         "fixture reaches a confirmed QSO with an exactly confirmed message");
  expect(guard.begin_tune() && guard.state() == TransmitState::Tuning,
         "TUNE can be started from a confirmed QSO");
  expect(!guard.message_confirmed(),
         "entering TUNE voids the exact preview confirmation");
  expect(guard.finish_tune() && guard.state() == TransmitState::Confirmed,
         "finishing TUNE returns to the confirmed QSO");
  expect(!guard.message_confirmed(),
         "the confirmation is still void once TUNE has ended, because the rig "
         "the operator confirmed a transmission on may have been moved");
  expect(!guard.begin_transmission(),
         "a message cannot be transmitted on a confirmation TUNE voided");
  expect(guard.pending_message() == "TU <SK>",
         "the staged text survives TUNE, so the operator confirms the preview "
         "again rather than retyping it");
  expect(guard.confirm_message("tu <sk>") && guard.begin_transmission(),
         "a fresh exact confirmation restores guarded transmission");
}

// ---------------------------------------------------------------------------
// ADIF: the length prefix is the structure, so nothing in a value is.
// ---------------------------------------------------------------------------

struct AdifField {
  std::string name;
  std::string value;
};

// An independent ADI reader that honours only the length prefixes, which is
// what the format says a reader must do and the whole reason ADI needs no
// escape mechanism. It deliberately shares no code with the writer.
[[nodiscard]] bool parse_adi_record(const std::string_view record,
                                    std::vector<AdifField>& fields) {
  std::size_t offset = 0;
  while (offset < record.size()) {
    while (offset < record.size() && record[offset] == ' ') ++offset;
    if (offset >= record.size()) break;
    if (record[offset] != '<') return false;
    const auto close = record.find('>', offset + 1U);
    if (close == std::string_view::npos) return false;
    const auto header = record.substr(offset + 1U, close - offset - 1U);
    const auto colon = header.find(':');
    if (colon == std::string_view::npos) {
      // The record terminator is the only specifier with no length.
      if (header != "EOR") return false;
      fields.push_back({std::string(header), std::string{}});
      offset = close + 1U;
      continue;
    }
    auto digits = header.substr(colon + 1U);
    const auto type_colon = digits.find(':');
    if (type_colon != std::string_view::npos)
      digits = digits.substr(0, type_colon);
    if (digits.empty()) return false;
    std::size_t length = 0;
    for (const char digit : digits) {
      if (digit < '0' || digit > '9') return false;
      length = length * 10U + static_cast<std::size_t>(digit - '0');
    }
    const auto value_start = close + 1U;
    // A length that runs past the end of the record is the one way this
    // writer could corrupt one: the prefix would be describing bytes it did
    // not write.
    if (length > record.size() - value_start) return false;
    fields.push_back({std::string(header.substr(0, colon)),
                      std::string(record.substr(value_start, length))});
    offset = value_start + length;
  }
  return true;
}

[[nodiscard]] std::string field_value(const std::vector<AdifField>& fields,
                                      const std::string_view name) {
  for (const auto& field : fields) {
    if (field.name == name) return field.value;
  }
  return {};
}

void test_adif_length_prefixes_survive_structural_bytes() {
  // ADI is length-prefixed, which is precisely why it has no escape
  // mechanism: a reader takes the stated number of bytes and never looks
  // inside them for structure. That reasoning is what lets the serializer
  // emit an operator-supplied value unaltered, and it was stated in the
  // conformance policy without anything checking it. This is that check, and
  // it also guards the other direction: a future escaping layer that altered
  // a character inside a callsign would fail here.
  cwassistant::core::QsoRecord qso{};
  qso.callsign = "I1<ABC";
  qso.qso_date = "20260830";
  qso.time_on = "143512";
  qso.band = "20M";
  qso.mode = "CW";
  qso.frequency_mhz = "14.025000";
  qso.rst_sent = "599";
  qso.rst_received = "579";
  qso.station_callsign = "IU0XYZ";
  // A NUL is the one byte that could make the emitted length disagree with
  // the bytes written, if the value were ever measured or copied as a C
  // string on the way out.
  qso.station_rig = std::string("IC-7300\0QRP", 11);
  qso.station_antenna = "DIPOLE\n20M AGL";

  const auto record = cwassistant::core::to_adif(qso);
  std::vector<AdifField> fields;
  expect(parse_adi_record(record, fields),
         "a reader that honours the length prefixes parses the whole record");
  expect(!fields.empty() && fields.back().name == "EOR",
         "the record ends with the terminator and nothing after it");
  expect(field_value(fields, "CALL") == "I1<ABC",
         "an angle bracket inside a callsign round-trips byte for byte, "
         "neither escaped nor stripped");
  expect(field_value(fields, "MY_ANTENNA") == "DIPOLE\n20M AGL",
         "a newline inside a value round-trips byte for byte");
  expect(field_value(fields, "MY_RIG") == std::string("IC-7300\0QRP", 11),
         "a NUL inside a value round-trips byte for byte");
  expect(record.find("<CALL:6>I1<ABC") != std::string::npos,
         "the emitted length counts the bytes actually written, including the "
         "angle bracket");
  expect(record.find("<MY_RIG:11>") != std::string::npos,
         "the emitted length counts a NUL as the byte it is");
  expect(record.find('\0') != std::string::npos,
         "the record carries the NUL rather than having been truncated at it");
  expect(field_value(fields, "FREQ") == "14.025000" &&
             field_value(fields, "STATION_CALLSIGN") == "IU0XYZ",
         "the fields after a value holding structural bytes are still read "
         "correctly, which is what a corrupt record would destroy");
}

}  // namespace

int main() {
  // Every case here is about what happens when nothing loaded a dictionary,
  // so the fixture starts from exactly that: no directory to read one from.
  setDictionaryDirectoryEnvironment(nullptr);

  // These two must stay first, and in this order. Each is the process's first
  // touch of its shared table.
  test_shared_prefix_table_first_use_is_thread_safe();
  test_morse_alphabet_origin_is_answered_for_the_alphabet_in_use();
  test_prefix_origin_is_answered_for_the_table_in_use();

  test_disarm_cannot_clear_a_latched_fault();
  test_latched_fault_keeps_the_observed_key_state();
  test_tune_voids_the_exact_preview_confirmation();

  test_adif_length_prefixes_survive_structural_bytes();

  if (failures > 0) {
    std::cerr << failures << " core integrity check(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "core integrity tests passed\n";
  return EXIT_SUCCESS;
}
