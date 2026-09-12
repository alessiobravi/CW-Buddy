add_library(cwa_core
  src/adif.cpp
  src/callsign_evidence.cpp
  src/callsign_policy.cpp
  src/cw_callsign_prefixes.cpp
  src/cat4om_protocol.cpp
  src/channel_scheduler.cpp
  src/cw_acoustic_refinement.cpp
  src/cw_character_decoder.cpp
  src/cw_character_lane_frontend.cpp
  src/cw_channel_bank.cpp
  src/cw_context_rescorer.cpp
  src/cw_spot_registry.cpp
  src/cw_vocabulary.cpp
  src/cw_morse_alphabet.cpp
  src/conversation_profile_file.cpp
  src/cw_decoder.cpp
  src/cw_event_lattice.cpp
  src/cw_probabilistic_decoder.cpp
  src/cw_semi_markov_segmenter.cpp
  src/cw_turn_timing_fingerprint.cpp
  src/cw_transmit_encoder.cpp
  src/cw_transmit_scheduler.cpp
  src/conversation_profile.cpp
  src/frequency_plan.cpp
  src/hardware_acceptance.cpp
  src/iq_receive.cpp
  src/iq_replay_source.cpp
  src/iq_writer.cpp
  src/offline_callsign_database.cpp
  src/reference_rig_profiles.cpp
  src/radio_control.cpp
  src/remote_control.cpp
  src/station_equipment.cpp
  src/spectrum_analyzer.cpp
  src/transmit_guard.cpp
  src/wav_replay_source.cpp
  src/wav_writer.cpp
)

target_include_directories(cwa_core PUBLIC include)
target_compile_features(cwa_core PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(cwa_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(cwa_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The Morse alphabet is data, but a decoder with no alphabet decodes nothing at
# all, so the shipped file is also compiled in as a last-resort fallback. The
# file remains the single source: this header is generated from it.
include("${PROJECT_SOURCE_DIR}/cmake/GenerateBuiltinDictionary.cmake")
cwa_generate_builtin_dictionary(
  "${PROJECT_SOURCE_DIR}/dictionaries/morse-alphabet.txt"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/cwassistant/core/cw_builtin_morse_alphabet.hpp"
  kCwBuiltinMorseAlphabet)
# The allocated callsign prefixes are data for the same reason and carry the
# same risk in reverse: a table that failed to load would answer "no such
# country" for every callsign at once, refusing every station on the band
# rather than the handful of misdecodes it exists to catch. The shipped file
# stays the single source; this header is generated from it.
cwa_generate_builtin_dictionary(
  "${PROJECT_SOURCE_DIR}/dictionaries/callsign-prefixes.txt"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/cwassistant/core/cw_builtin_callsign_prefixes.hpp"
  kCwBuiltinCallsignPrefixes)
target_include_directories(cwa_core PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
