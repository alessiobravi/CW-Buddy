add_executable(cwa_core_tests core_tests.cpp)
target_link_libraries(cwa_core_tests PRIVATE cwa_core)
# The shipped dictionaries are data the decoder depends on, so the tests read
# the real files rather than a private copy: a token removed from them, or a
# prefix that stops being an exchange word, fails here.
target_compile_definitions(cwa_core_tests PRIVATE
  CWA_DICTIONARY_DIR="${PROJECT_SOURCE_DIR}/dictionaries")
add_test(NAME cwa_core_tests COMMAND cwa_core_tests)

add_executable(cwa_iq_receive_tests iq_receive_tests.cpp)
target_link_libraries(cwa_iq_receive_tests PRIVATE cwa_core)
add_test(NAME cwa_iq_receive_tests COMMAND cwa_iq_receive_tests)

add_executable(cwa_iq_writer_tests iq_writer_tests.cpp)
target_link_libraries(cwa_iq_writer_tests PRIVATE cwa_core)
add_test(NAME cwa_iq_writer_tests COMMAND cwa_iq_writer_tests)

add_executable(cwa_radio_control_tests radio_control_tests.cpp)
target_link_libraries(cwa_radio_control_tests PRIVATE cwa_core)
add_test(NAME cwa_radio_control_tests COMMAND cwa_radio_control_tests)

add_executable(cwa_transmit_scheduler_tests cw_transmit_scheduler_tests.cpp)
target_link_libraries(cwa_transmit_scheduler_tests PRIVATE cwa_core)
add_test(NAME cwa_transmit_scheduler_tests COMMAND cwa_transmit_scheduler_tests)

add_executable(cwa_hardware_acceptance_tests hardware_acceptance_tests.cpp)
target_link_libraries(cwa_hardware_acceptance_tests PRIVATE cwa_core)
add_test(NAME cwa_hardware_acceptance_tests COMMAND cwa_hardware_acceptance_tests)

add_executable(cwa_character_refinement_tests
  cw_character_refinement_tests.cpp)
target_link_libraries(cwa_character_refinement_tests PRIVATE cwa_core)
add_test(NAME cwa_character_refinement_tests
  COMMAND cwa_character_refinement_tests)

add_executable(cwa_decoder_transcript_qml_test
  desktop/decoder_transcript_qml_test.cpp)
target_compile_definitions(cwa_decoder_transcript_qml_test PRIVATE
  CWA_MAIN_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/Main.qml"
  CWA_DESKTOP_MAIN_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/main.cpp"
  CWA_APP_SETTINGS_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/settings/app_settings.cpp")
add_test(NAME cwa_decoder_transcript_qml_test
  COMMAND cwa_decoder_transcript_qml_test)

add_executable(cwa_action_tooltip_qml_test
  desktop/action_tooltip_qml_test.cpp)
target_compile_definitions(cwa_action_tooltip_qml_test PRIVATE
  CWA_MAIN_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/Main.qml"
  CWA_SETTINGS_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/SettingsPane.qml"
  CWA_SETUP_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/SetupWizard.qml"
  CWA_PROFILE_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/ProfileChooser.qml")
add_test(NAME cwa_action_tooltip_qml_test
  COMMAND cwa_action_tooltip_qml_test)

add_executable(cwa_radio_provider_settings_qml_test
  desktop/radio_provider_settings_qml_test.cpp)
target_compile_definitions(cwa_radio_provider_settings_qml_test PRIVATE
  CWA_SETTINGS_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/SettingsPane.qml"
  CWA_SETUP_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/SetupWizard.qml"
  CWA_APP_SETTINGS_HPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/settings/app_settings.hpp"
  CWA_APP_SETTINGS_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/settings/app_settings.cpp")
add_test(NAME cwa_radio_provider_settings_qml_test
  COMMAND cwa_radio_provider_settings_qml_test)

add_executable(cwa_sdr_settings_qml_test
  desktop/sdr_settings_qml_test.cpp)
target_compile_definitions(cwa_sdr_settings_qml_test PRIVATE
  CWA_MAIN_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/Main.qml"
  CWA_SETTINGS_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/SettingsPane.qml"
  CWA_APP_SETTINGS_HPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/settings/app_settings.hpp"
  CWA_APP_SETTINGS_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/settings/app_settings.cpp"
  CWA_REPLAY_CONTROLLER_HPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/replay/replay_controller.hpp"
  CWA_REPLAY_CONTROLLER_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/replay/replay_controller.cpp")
add_test(NAME cwa_sdr_settings_qml_test
  COMMAND cwa_sdr_settings_qml_test)

add_executable(cwa_linux_sdr_packaging_test
  desktop/linux_sdr_packaging_test.cpp)
target_compile_definitions(cwa_linux_sdr_packaging_test PRIVATE
  CWA_DESKTOP_WORKFLOW_PATH="${PROJECT_SOURCE_DIR}/.github/workflows/desktop-ci.yml"
  CWA_ROOT_CMAKE_PATH="${PROJECT_SOURCE_DIR}/CMakeLists.txt"
  CWA_DESKTOP_CMAKE_PATH="${PROJECT_SOURCE_DIR}/src/desktop/CMakeLists.txt"
  CWA_WINDOWS_SDR_VERIFIER_PATH="${PROJECT_SOURCE_DIR}/src/desktop/packaging/windows/VerifyWindowsInstaller.ps1"
  CWA_LINUX_SDR_LAUNCHER_PATH="${PROJECT_SOURCE_DIR}/src/desktop/packaging/linux/cw-buddy-portable-launcher.sh")
add_test(NAME cwa_linux_sdr_packaging_test
  COMMAND cwa_linux_sdr_packaging_test)

add_executable(cwa_decoder_display_separation_test
  desktop/decoder_display_separation_test.cpp)
target_compile_definitions(cwa_decoder_display_separation_test PRIVATE
  CWA_LIVE_AUDIO_WORKER_PATH="${PROJECT_SOURCE_DIR}/src/desktop/replay/live_audio_worker.cpp"
  CWA_REPLAY_CONTROLLER_PATH="${PROJECT_SOURCE_DIR}/src/desktop/replay/replay_controller.cpp")
add_test(NAME cwa_decoder_display_separation_test
  COMMAND cwa_decoder_display_separation_test)

add_executable(cwa_weak_signal_decoding_settings_test
  desktop/weak_signal_decoding_settings_test.cpp)
target_compile_definitions(cwa_weak_signal_decoding_settings_test PRIVATE
  CWA_APP_SETTINGS_HPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/settings/app_settings.hpp"
  CWA_APP_SETTINGS_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/settings/app_settings.cpp"
  CWA_SETTINGS_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/SettingsPane.qml"
  CWA_REPLAY_CONTROLLER_HPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/replay/replay_controller.hpp"
  CWA_REPLAY_CONTROLLER_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/replay/replay_controller.cpp"
  CWA_LIVE_AUDIO_WORKER_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/replay/live_audio_worker.cpp")
add_test(NAME cwa_weak_signal_decoding_settings_test
  COMMAND cwa_weak_signal_decoding_settings_test)

add_executable(cwa_turn_timing_diagnostics_contract_test
  desktop/turn_timing_diagnostics_contract_test.cpp)
target_compile_definitions(cwa_turn_timing_diagnostics_contract_test PRIVATE
  CWA_LIVE_AUDIO_WORKER_PATH="${PROJECT_SOURCE_DIR}/src/desktop/replay/live_audio_worker.cpp"
  CWA_CAPTURE_REPLAY_PATH="${PROJECT_SOURCE_DIR}/tests/cw_capture_replay.cpp")
add_test(NAME cwa_turn_timing_diagnostics_contract_test
  COMMAND cwa_turn_timing_diagnostics_contract_test)

add_executable(cwa_decoder_surface_benchmark cw_decoder_surface_benchmark.cpp)
target_link_libraries(cwa_decoder_surface_benchmark PRIVATE cwa_core)
add_test(NAME cwa_decoder_surface_benchmark
  COMMAND cwa_decoder_surface_benchmark)

add_executable(cwa_audio_stream_consent_qml_test
  desktop/audio_stream_consent_qml_test.cpp)
target_compile_definitions(cwa_audio_stream_consent_qml_test PRIVATE
  CWA_MAIN_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/Main.qml"
  CWA_DESKTOP_MAIN_CPP_PATH="${PROJECT_SOURCE_DIR}/src/desktop/main.cpp")
add_test(NAME cwa_audio_stream_consent_qml_test
  COMMAND cwa_audio_stream_consent_qml_test)

add_executable(cwa_update_notice_qml_test
  desktop/update_notice_qml_test.cpp)
target_compile_definitions(cwa_update_notice_qml_test PRIVATE
  CWA_MAIN_QML_PATH="${PROJECT_SOURCE_DIR}/src/desktop/qml/Main.qml")
add_test(NAME cwa_update_notice_qml_test COMMAND cwa_update_notice_qml_test)

add_executable(cwa_spot_registry_tests cw_spot_registry_tests.cpp)
target_link_libraries(cwa_spot_registry_tests PRIVATE cwa_core)
add_test(NAME cwa_spot_registry_tests COMMAND cwa_spot_registry_tests)

add_executable(cwa_callsign_evidence_tests callsign_evidence_tests.cpp)
target_link_libraries(cwa_callsign_evidence_tests PRIVATE cwa_core)
add_test(NAME cwa_callsign_evidence_tests COMMAND cwa_callsign_evidence_tests)

add_executable(cwa_offline_callsign_database_tests
  offline_callsign_database_tests.cpp)
target_link_libraries(cwa_offline_callsign_database_tests PRIVATE cwa_core)
add_test(NAME cwa_offline_callsign_database_tests
  COMMAND cwa_offline_callsign_database_tests)

add_executable(cwa_conversation_profile_tests conversation_profile_tests.cpp)
target_link_libraries(cwa_conversation_profile_tests PRIVATE cwa_core)
target_compile_definitions(cwa_conversation_profile_tests PRIVATE
  CWA_DICTIONARY_DIR="${PROJECT_SOURCE_DIR}/dictionaries")
add_test(NAME cwa_conversation_profile_tests COMMAND cwa_conversation_profile_tests)

add_executable(cwa_event_lattice_tests cw_event_lattice_tests.cpp)
target_link_libraries(cwa_event_lattice_tests PRIVATE cwa_core)
add_test(NAME cwa_event_lattice_tests COMMAND cwa_event_lattice_tests)

add_executable(cwa_probabilistic_decoder_tests
  cw_probabilistic_decoder_tests.cpp)
target_link_libraries(cwa_probabilistic_decoder_tests PRIVATE cwa_core)
add_test(NAME cwa_probabilistic_decoder_tests
  COMMAND cwa_probabilistic_decoder_tests)

add_executable(cwa_probabilistic_decoder_benchmark
  cw_probabilistic_decoder_benchmark.cpp)
target_link_libraries(cwa_probabilistic_decoder_benchmark PRIVATE cwa_core)
add_test(NAME cwa_probabilistic_decoder_benchmark
  COMMAND cwa_probabilistic_decoder_benchmark)

add_executable(cwa_decoder_benchmark cw_decoder_benchmark.cpp)
target_link_libraries(cwa_decoder_benchmark PRIVATE cwa_core)
add_test(NAME cwa_decoder_benchmark COMMAND cwa_decoder_benchmark)

add_executable(cwa_manual_cadence_tests cw_manual_cadence_tests.cpp)
target_link_libraries(cwa_manual_cadence_tests PRIVATE cwa_core)
add_test(NAME cwa_manual_cadence_tests COMMAND cwa_manual_cadence_tests)

add_executable(cwa_decoder_snapshot_cache_tests
  cw_decoder_snapshot_cache_tests.cpp)
target_link_libraries(cwa_decoder_snapshot_cache_tests PRIVATE cwa_core)
add_test(NAME cwa_decoder_snapshot_cache_tests
  COMMAND cwa_decoder_snapshot_cache_tests)

add_executable(cwa_fixed_lag_refinement_tests
  cw_fixed_lag_refinement_tests.cpp)
target_link_libraries(cwa_fixed_lag_refinement_tests PRIVATE cwa_core)
add_test(NAME cwa_fixed_lag_refinement_tests
  COMMAND cwa_fixed_lag_refinement_tests)

add_executable(cwa_acoustic_refinement_tests
  cw_acoustic_refinement_tests.cpp)
target_link_libraries(cwa_acoustic_refinement_tests PRIVATE cwa_core)
add_test(NAME cwa_acoustic_refinement_tests
  COMMAND cwa_acoustic_refinement_tests)

add_executable(cwa_turn_timing_fingerprint_tests
  cw_turn_timing_fingerprint_tests.cpp)
target_link_libraries(cwa_turn_timing_fingerprint_tests PRIVATE cwa_core)
add_test(NAME cwa_turn_timing_fingerprint_tests
  COMMAND cwa_turn_timing_fingerprint_tests)

add_executable(cwa_decoder_evaluation_metrics_tests
  decoder_evaluation_metrics_tests.cpp)
add_test(NAME cwa_decoder_evaluation_metrics_tests
  COMMAND cwa_decoder_evaluation_metrics_tests)

add_executable(cwa_decoder_quality_gate cw_decoder_quality_gate.cpp)
target_link_libraries(cwa_decoder_quality_gate PRIVATE cwa_core)
add_test(NAME cwa_decoder_quality_gate COMMAND cwa_decoder_quality_gate)

add_executable(cwa_receiver_annotations_tests receiver_annotations_tests.cpp)
target_compile_definitions(cwa_receiver_annotations_tests PRIVATE
  CWA_RECEIVER_ANNOTATION_FIXTURE="${PROJECT_SOURCE_DIR}/tests/fixtures/receiver_annotations_v1.tsv")
add_test(NAME cwa_receiver_annotations_tests
  COMMAND cwa_receiver_annotations_tests)

add_executable(cwa_receiver_holdout_benchmark
  cw_receiver_holdout_benchmark.cpp)
target_link_libraries(cwa_receiver_holdout_benchmark PRIVATE cwa_core)
add_test(NAME cwa_receiver_holdout_benchmark
  COMMAND cwa_receiver_holdout_benchmark)

# Word-boundary placement is the only thing the context vocabulary can change,
# and neither the synthetic surface nor the capture corpus can see it: the
# surface never reaches the rescorer and the corpus scores callsign recovery.
# Per-stage wall time for the live receive path, as a function of tracked
# signal count. PERF-002 accepts an optimisation only after a measurement, and
# this is the instrument that provides one. Not a gate: it reports, and the
# absolute figures depend on the machine.
add_executable(cwa_receive_profile cw_receive_profile.cpp)
target_link_libraries(cwa_receive_profile PRIVATE cwa_core)

add_executable(cwa_spacing_benchmark cw_spacing_benchmark.cpp)
target_link_libraries(cwa_spacing_benchmark PRIVATE cwa_core)
target_compile_definitions(cwa_spacing_benchmark PRIVATE
  CWA_DICTIONARY_DIR="${PROJECT_SOURCE_DIR}/dictionaries")
add_test(NAME cwa_spacing_benchmark COMMAND cwa_spacing_benchmark)
set_tests_properties(cwa_spacing_benchmark PROPERTIES TIMEOUT 600)

add_executable(cwa_verification_benchmark cw_verification_benchmark.cpp)
target_link_libraries(cwa_verification_benchmark PRIVATE cwa_core)
add_test(NAME cwa_verification_benchmark COMMAND cwa_verification_benchmark)

# The replay harness's SigMF path, on a fixture this test writes itself. The
# operator captures it exists for are private, so the two-stage receive chain
# they are decoded through would otherwise have no test at all.
add_executable(cwa_iq_capture_replay_tests iq_capture_replay_tests.cpp)
target_link_libraries(cwa_iq_capture_replay_tests PRIVATE cwa_core)
target_compile_definitions(cwa_iq_capture_replay_tests PRIVATE
  CWA_CAPTURE_REPLAY_PATH="${PROJECT_SOURCE_DIR}/tests/cw_capture_replay.cpp")
add_test(NAME cwa_iq_capture_replay_tests
  COMMAND cwa_iq_capture_replay_tests)

# Operator captures are private and are therefore not CI fixtures. This
# target provides the same native replay path for local before/after audits
# without making external capture files a test-suite dependency.
add_executable(cwa_capture_replay cw_capture_replay.cpp)
target_link_libraries(cwa_capture_replay PRIVATE cwa_core)
target_compile_definitions(cwa_capture_replay PRIVATE
  CWA_DICTIONARY_DIR="${PROJECT_SOURCE_DIR}/dictionaries")

# The Morse alphabet is data. A target that decodes without loading it would
# fall back to the compiled-in copy, which is correct but hides whether the
# shipped files actually work. Point every test in this directory at them so
# the files themselves are what is exercised.
get_property(cwa_directory_tests DIRECTORY PROPERTY TESTS)
foreach(cwa_test IN LISTS cwa_directory_tests)
  set_property(TEST ${cwa_test} APPEND PROPERTY ENVIRONMENT
    "CWA_DICTIONARY_DIR=${PROJECT_SOURCE_DIR}/dictionaries")
endforeach()
