find_package(Qt6 6.5 REQUIRED COMPONENTS
  Core Gui Multimedia Network Qml Quick QuickControls2 QuickDialogs2 SerialPort WebSockets)

set(CWA_HAVE_SOAPY_SDR 0)
set(CWA_SOAPY_SDR_MODULE "" CACHE FILEPATH
  "Optional SoapySDR receive module to embed in a macOS application bundle")
set(CWA_SOAPY_SDR_LICENSE_DIR "" CACHE PATH
  "Directory containing SDR runtime licenses and build provenance")
if(CWA_ENABLE_SOAPY_SDR)
  find_package(SoapySDR CONFIG REQUIRED)
  set(CWA_HAVE_SOAPY_SDR 1)
  message(STATUS "Direct SoapySDR reception enabled")
endif()

qt_standard_project_setup(REQUIRES 6.5)

function(cwa_target_add_local_character_decoder target_name)
  target_sources(${target_name} PRIVATE
    decoder/local_character_decoder.cpp
    decoder/local_character_decoder.hpp)
  target_include_directories(${target_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  if(CWA_ENABLE_ONNX_CHARACTER_DECODER)
    target_sources(${target_name} PRIVATE
      decoder/onnx_character_decoder_backend.cpp
      decoder/onnx_character_decoder_backend.hpp)
    cwa_target_link_onnx_runtime(${target_name})
    target_compile_definitions(${target_name} PRIVATE
      CWA_HAVE_ONNX_RUNTIME=1)
  else()
    target_compile_definitions(${target_name} PRIVATE
      CWA_HAVE_ONNX_RUNTIME=0)
  endif()
endfunction()

qt_add_executable(cw-buddy-desktop
  main.cpp
  replay/replay_controller.cpp
  replay/replay_controller.hpp
  replay/decoder_channel_model.cpp
  replay/decoder_channel_model.hpp
  replay/live_audio_worker.cpp
  replay/live_audio_worker.hpp
  replay/live_audio_pipe.hpp
  sdr/sdr_receiver.cpp
  sdr/sdr_receiver.hpp
  sdr/sdr_capture_worker.cpp
  sdr/sdr_capture_worker.hpp
  sdr/sdr_runtime_environment.cpp
  sdr/sdr_runtime_environment.hpp
  radio/cat4om_client.cpp
  radio/cat4om_client.hpp
  dxcluster/dx_cluster_servers.cpp
  dxcluster/dx_cluster_servers.hpp
  dxcluster/dx_cluster_client.cpp
  dxcluster/dx_cluster_client.hpp
  diagnostics/diagnostics_server.cpp
  diagnostics/diagnostics_server.hpp
  diagnostics/audio_stream_server.cpp
  diagnostics/audio_stream_server.hpp
  radio/hamlib_rigctld_client.cpp
  radio/hamlib_rigctld_client.hpp
  transmit/transmit_controller.cpp
  transmit/transmit_controller.hpp
  transmit/direct_keying_adapter.cpp
  transmit/direct_keying_adapter.hpp
  transmit/direct_keying_acceptance_probe.cpp
  transmit/direct_keying_acceptance_probe.hpp
  transmit/direct_transmit_engine.cpp
  transmit/direct_transmit_engine.hpp
  settings/app_settings.cpp
  settings/app_settings.hpp
  settings/product_migration.cpp
  settings/product_migration.hpp
  updates/update_checker.cpp
  updates/update_checker.hpp
  updates/callsign_database_updater.cpp
  updates/callsign_database_updater.hpp
  visualization/spectrum_frame.hpp
  visualization/waterfall_conditioner.cpp
  visualization/waterfall_conditioner.hpp
  visualization/spectrum_waterfall_item.cpp
  visualization/spectrum_waterfall_item.hpp
)

if(CWA_HAVE_SOAPY_SDR)
  target_sources(cw-buddy-desktop PRIVATE sdr/soapy_sdr_backend.cpp)
  if(TARGET SoapySDR::SoapySDR)
    target_link_libraries(cw-buddy-desktop PRIVATE SoapySDR::SoapySDR)
  else()
    target_include_directories(cw-buddy-desktop PRIVATE ${SoapySDR_INCLUDE_DIRS})
    target_link_libraries(cw-buddy-desktop PRIVATE ${SoapySDR_LIBRARIES})
  endif()
endif()

set(CWA_MACOS_BUNDLE_NAME "CW Buddy")
set(CWA_MACOS_BUNDLE_EXECUTABLE "cw-buddy-desktop")
# Preserve the original bundle identifier so macOS recognizes CW Buddy as an
# upgrade of installed legacy builds instead of a second application.
set(CWA_MACOS_BUNDLE_IDENTIFIER "it.iu0lfq.CWAssistant")
set(CWA_MACOS_BUNDLE_ICON "cw-buddy.icns")

set_target_properties(cw-buddy-desktop PROPERTIES
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_BUNDLE_NAME "${CWA_MACOS_BUNDLE_NAME}"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${CWA_MACOS_BUNDLE_IDENTIFIER}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${CWA_BUILD_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${CWA_BUILD_VERSION}"
  MACOSX_BUNDLE_ICON_FILE "${CWA_MACOS_BUNDLE_ICON}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in"
  WIN32_EXECUTABLE TRUE
)

set_source_files_properties(packaging/cw-buddy.png PROPERTIES
  QT_RESOURCE_ALIAS cw-buddy.png)
set_source_files_properties(packaging/on-air-active.png PROPERTIES
  QT_RESOURCE_ALIAS on-air-active.png)
qt_add_resources(cw-buddy-desktop "cwa_application_icons"
  PREFIX "/icons"
  FILES packaging/cw-buddy.png packaging/on-air-active.png)

# The CW dictionaries ship inside the binary so the decoder always has them,
# and are copied to the operator's data directory on first run so they can be
# edited there. The copy wins at startup when it exists.
#
# BASE rather than a QT_RESOURCE_ALIAS on each file. These paths are absolute
# and outside this directory, and per-file source properties on such paths do
# not resolve identically across generators; when the alias failed to apply the
# resource landed somewhere other than :/dictionaries, the read returned
# nothing, and an empty file was seeded that was then reused on every later
# run. BASE computes every path from one rule and cannot come apart that way.
qt_add_resources(cw-buddy-desktop "cwa_cw_dictionaries"
  PREFIX "/dictionaries"
  BASE "${PROJECT_SOURCE_DIR}/dictionaries"
  FILES
    "${PROJECT_SOURCE_DIR}/dictionaries/cw-abbreviations.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/cw-word-gap-prefixes.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/cw-distinctive-tokens.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/morse-alphabet.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/callsign-prefixes.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/dx-cluster-servers.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/contests/cq-ww-cw.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/contests/cq-wpx-cw.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/contests/arrl-field-day-cw.txt"
    "${PROJECT_SOURCE_DIR}/dictionaries/contests/arrl-sweepstakes-cw.txt")

if(WIN32)
  configure_file(packaging/cw-buddy.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/cw-buddy.rc" @ONLY)
  target_sources(cw-buddy-desktop PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/cw-buddy.rc"
    packaging/cw-buddy.ico)
endif()

if(APPLE)
  set_source_files_properties(packaging/cw-buddy.icns PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
  target_sources(cw-buddy-desktop PRIVATE packaging/cw-buddy.icns)
endif()

set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(qml/LabeledSlider.qml PROPERTIES QT_RESOURCE_ALIAS LabeledSlider.qml)
set_source_files_properties(qml/ProfileChooser.qml PROPERTIES QT_RESOURCE_ALIAS ProfileChooser.qml)
set_source_files_properties(qml/SetupWizard.qml PROPERTIES QT_RESOURCE_ALIAS SetupWizard.qml)
set_source_files_properties(qml/SettingsPane.qml PROPERTIES QT_RESOURCE_ALIAS SettingsPane.qml)

qt_add_qml_module(cw-buddy-desktop
  URI CWBuddy
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/LabeledSlider.qml
    qml/ProfileChooser.qml
    qml/SetupWizard.qml
    qml/SettingsPane.qml
)

target_link_libraries(cw-buddy-desktop PRIVATE
  cwa_core
  Qt6::Core
  Qt6::Gui
  Qt6::Multimedia
  Qt6::Network
  Qt6::Qml
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::QuickDialogs2
  Qt6::SerialPort
  Qt6::WebSockets
)
cwa_target_add_local_character_decoder(cw-buddy-desktop)
target_compile_definitions(cw-buddy-desktop PRIVATE
  CWA_VERSION="${CWA_BUILD_VERSION}"
  CWA_HAVE_SOAPY_SDR=${CWA_HAVE_SOAPY_SDR})

if(CWA_BUILD_TESTS)
  qt_add_executable(cwa_sdr_runtime_environment_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/sdr_runtime_environment_test.cpp
    sdr/sdr_runtime_environment.cpp
    sdr/sdr_runtime_environment.hpp)
  target_link_libraries(cwa_sdr_runtime_environment_test PRIVATE Qt6::Core)
  target_include_directories(cwa_sdr_runtime_environment_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_sdr_runtime_environment_test
    COMMAND cwa_sdr_runtime_environment_test)
  set_tests_properties(cwa_sdr_runtime_environment_test PROPERTIES TIMEOUT 10)

  if(WIN32 AND CWA_HAVE_SOAPY_SDR)
    qt_add_executable(cwa_soapy_sdr_runtime_test
      ${PROJECT_SOURCE_DIR}/tests/desktop/soapy_sdr_runtime_test.cpp)
    if(TARGET SoapySDR::SoapySDR)
      target_link_libraries(cwa_soapy_sdr_runtime_test PRIVATE
        SoapySDR::SoapySDR)
    else()
      target_include_directories(cwa_soapy_sdr_runtime_test PRIVATE
        ${SoapySDR_INCLUDE_DIRS})
      target_link_libraries(cwa_soapy_sdr_runtime_test PRIVATE
        ${SoapySDR_LIBRARIES})
    endif()
    add_test(NAME cwa_soapy_sdr_runtime_test
      COMMAND cwa_soapy_sdr_runtime_test
        "${CWA_SOAPY_SDR_RUNTIME_ROOT}/lib/SoapySDR/modules0.8/rtlsdrSupport.dll")
    set_tests_properties(cwa_soapy_sdr_runtime_test PROPERTIES
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${CWA_SOAPY_SDR_RUNTIME_ROOT}/bin"
      TIMEOUT 10)
  endif()

  qt_add_executable(cwa_sdr_receiver_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/sdr_receiver_test.cpp
    sdr/sdr_receiver.cpp
    sdr/sdr_receiver.hpp)
  target_link_libraries(cwa_sdr_receiver_test PRIVATE cwa_core Qt6::Core)
  target_include_directories(cwa_sdr_receiver_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(cwa_sdr_receiver_test PRIVATE
    CWA_HAVE_SOAPY_SDR=0)
  add_test(NAME cwa_sdr_receiver_test COMMAND cwa_sdr_receiver_test)
  set_tests_properties(cwa_sdr_receiver_test PROPERTIES TIMEOUT 10)

  qt_add_executable(cwa_hamlib_rigctld_client_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/hamlib_rigctld_client_test.cpp
    radio/hamlib_rigctld_client.cpp
    radio/hamlib_rigctld_client.hpp)
  target_link_libraries(cwa_hamlib_rigctld_client_test PRIVATE
    cwa_core Qt6::Core Qt6::Network)
  target_include_directories(cwa_hamlib_rigctld_client_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_hamlib_rigctld_client_test
    COMMAND cwa_hamlib_rigctld_client_test)
  set_tests_properties(cwa_hamlib_rigctld_client_test PROPERTIES TIMEOUT 10)

  qt_add_executable(cwa_transmit_controller_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/transmit_controller_test.cpp
    transmit/direct_keying_adapter.cpp
    transmit/direct_keying_adapter.hpp
    transmit/direct_transmit_engine.cpp
    transmit/direct_transmit_engine.hpp
    transmit/transmit_controller.cpp
    transmit/transmit_controller.hpp)
  target_link_libraries(cwa_transmit_controller_test PRIVATE
    cwa_core Qt6::Core Qt6::SerialPort)
  target_include_directories(cwa_transmit_controller_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_transmit_controller_test
    COMMAND cwa_transmit_controller_test)
  set_tests_properties(cwa_transmit_controller_test PROPERTIES TIMEOUT 10)

  qt_add_executable(cwa_direct_keying_adapter_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/direct_keying_adapter_test.cpp
    transmit/direct_keying_adapter.cpp
    transmit/direct_keying_adapter.hpp)
  target_link_libraries(cwa_direct_keying_adapter_test PRIVATE
    Qt6::Core Qt6::SerialPort)
  target_include_directories(cwa_direct_keying_adapter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_direct_keying_adapter_test
    COMMAND cwa_direct_keying_adapter_test)

  qt_add_executable(cwa_direct_keying_acceptance_probe_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/direct_keying_acceptance_probe_test.cpp
    transmit/direct_keying_acceptance_probe.cpp
    transmit/direct_keying_acceptance_probe.hpp
    transmit/direct_keying_adapter.hpp)
  target_link_libraries(cwa_direct_keying_acceptance_probe_test PRIVATE
    Qt6::Core Qt6::SerialPort)
  target_include_directories(cwa_direct_keying_acceptance_probe_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_direct_keying_acceptance_probe_test
    COMMAND cwa_direct_keying_acceptance_probe_test)
  set_tests_properties(cwa_direct_keying_acceptance_probe_test PROPERTIES
    TIMEOUT 10)

  qt_add_executable(cwa_direct_transmit_engine_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/direct_transmit_engine_test.cpp
    transmit/direct_keying_adapter.cpp
    transmit/direct_keying_adapter.hpp
    transmit/direct_transmit_engine.cpp
    transmit/direct_transmit_engine.hpp)
  target_link_libraries(cwa_direct_transmit_engine_test PRIVATE
    cwa_core Qt6::Core Qt6::SerialPort)
  target_include_directories(cwa_direct_transmit_engine_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_direct_transmit_engine_test
    COMMAND cwa_direct_transmit_engine_test)
  set_tests_properties(cwa_direct_transmit_engine_test PROPERTIES TIMEOUT 10)

  qt_add_executable(cwa_live_audio_pipeline_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/live_audio_pipeline_test.cpp
    replay/live_audio_worker.cpp
    replay/live_audio_worker.hpp
    replay/decoder_channel_model.cpp
    replay/decoder_channel_model.hpp
  )
  target_link_libraries(cwa_live_audio_pipeline_test PRIVATE
    cwa_core Qt6::Core Qt6::Multimedia)
  target_include_directories(cwa_live_audio_pipeline_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  cwa_target_add_local_character_decoder(cwa_live_audio_pipeline_test)
  add_test(NAME cwa_live_audio_pipeline_test
    COMMAND cwa_live_audio_pipeline_test)
  set_tests_properties(cwa_live_audio_pipeline_test PROPERTIES TIMEOUT 15)

  qt_add_executable(cwa_desktop_render_startup_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/spectrum_waterfall_startup_test.cpp
    dxcluster/dx_cluster_servers.cpp
    dxcluster/dx_cluster_servers.hpp
    dxcluster/dx_cluster_client.cpp
    dxcluster/dx_cluster_client.hpp
    diagnostics/diagnostics_server.cpp
    diagnostics/diagnostics_server.hpp
    diagnostics/audio_stream_server.cpp
    diagnostics/audio_stream_server.hpp
    replay/replay_controller.cpp
    replay/replay_controller.hpp
    replay/live_audio_worker.cpp
    replay/live_audio_worker.hpp
    replay/live_audio_pipe.hpp
    sdr/sdr_receiver.cpp
    sdr/sdr_receiver.hpp
    sdr/sdr_capture_worker.cpp
    sdr/sdr_capture_worker.hpp
    replay/decoder_channel_model.cpp
    replay/decoder_channel_model.hpp
    visualization/spectrum_frame.hpp
    visualization/waterfall_conditioner.cpp
    visualization/waterfall_conditioner.hpp
    visualization/spectrum_waterfall_item.cpp
    visualization/spectrum_waterfall_item.hpp
  )
  # Qt6::Network is named explicitly because this target now compiles the spot
  # provider, which uses the network classes directly. Other platforms happen
  # to resolve it through Qt Quick's own dependencies; a linker that drops
  # unused libraries does not, so relying on that is how a build passes on
  # three platforms and fails on the fourth.
  target_link_libraries(cwa_desktop_render_startup_test PRIVATE
    cwa_core Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Quick)
  target_include_directories(cwa_desktop_render_startup_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(cwa_desktop_render_startup_test PRIVATE
    CWA_HAVE_SOAPY_SDR=0)
  cwa_target_add_local_character_decoder(cwa_desktop_render_startup_test)
  add_test(NAME cwa_desktop_render_startup_test
    COMMAND cwa_desktop_render_startup_test)
  set_tests_properties(cwa_desktop_render_startup_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    TIMEOUT 10)

  add_test(NAME cwa_desktop_qml_startup_smoke
    COMMAND cw-buddy-desktop --profile ci-smoke --smoke-test)
  set_tests_properties(cwa_desktop_qml_startup_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    TIMEOUT 15)
  if(WIN32 AND CWA_HAVE_SOAPY_SDR)
    if(NOT CWA_SOAPY_SDR_RUNTIME_ROOT)
      message(FATAL_ERROR
        "Windows SDR-enabled tests require CWA_SOAPY_SDR_RUNTIME_ROOT")
    endif()
    set_tests_properties(cwa_desktop_qml_startup_smoke PROPERTIES
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${CWA_SOAPY_SDR_RUNTIME_ROOT}/bin")
  endif()

  qt_add_executable(cwa_dx_cluster_client_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/dx_cluster_client_test.cpp
    dxcluster/dx_cluster_client.cpp
    dxcluster/dx_cluster_client.hpp
    dxcluster/dx_cluster_servers.cpp
    dxcluster/dx_cluster_servers.hpp)
  # Qt6::Network for the loopback half: the band-filter tests drive the real
  # client against a QTcpServer on 127.0.0.1 and read back every line it
  # wrote, because what is actually sent to a stranger's machine is the thing
  # worth asserting on. dx_cluster_servers.cpp comes too -- the client calls
  # DxClusterServer::isValid() and will not link without it.
  target_link_libraries(cwa_dx_cluster_client_test PRIVATE
    cwa_core Qt6::Core Qt6::Network)
  target_include_directories(cwa_dx_cluster_client_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_dx_cluster_client_test COMMAND cwa_dx_cluster_client_test)
  set_tests_properties(cwa_dx_cluster_client_test PROPERTIES TIMEOUT 60)

  qt_add_executable(cwa_dx_cluster_servers_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/dx_cluster_servers_test.cpp
    dxcluster/dx_cluster_servers.cpp
    dxcluster/dx_cluster_servers.hpp)
  target_link_libraries(cwa_dx_cluster_servers_test PRIVATE
    cwa_core Qt6::Core)
  target_include_directories(cwa_dx_cluster_servers_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  # The shipped list is read from the source tree rather than a fixture, so a
  # server entry cannot be edited into something unparseable without a test
  # saying so.
  target_compile_definitions(cwa_dx_cluster_servers_test PRIVATE
    CWA_DX_CLUSTER_SERVERS_TXT_PATH="${PROJECT_SOURCE_DIR}/dictionaries/dx-cluster-servers.txt")
  add_test(NAME cwa_dx_cluster_servers_test COMMAND cwa_dx_cluster_servers_test)
  set_tests_properties(cwa_dx_cluster_servers_test PROPERTIES TIMEOUT 15)

  qt_add_executable(cwa_diagnostics_server_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/diagnostics_server_test.cpp
    diagnostics/diagnostics_server.cpp
    diagnostics/diagnostics_server.hpp
    diagnostics/audio_stream_server.cpp
    diagnostics/audio_stream_server.hpp)
  # Binds only loopback and an unassignable TEST-NET-3 address, so running the
  # suite never puts an interface of the build machine on a socket.
  target_link_libraries(cwa_diagnostics_server_test PRIVATE
    Qt6::Core Qt6::Network)
  target_include_directories(cwa_diagnostics_server_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_diagnostics_server_test COMMAND cwa_diagnostics_server_test)
  set_tests_properties(cwa_diagnostics_server_test PROPERTIES TIMEOUT 60)

  qt_add_executable(cwa_diagnostics_audio_stream_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/diagnostics_audio_stream_test.cpp
    diagnostics/diagnostics_server.cpp
    diagnostics/diagnostics_server.hpp
    diagnostics/audio_stream_server.cpp
    diagnostics/audio_stream_server.hpp)
  # Loopback only, in both planes. The control port is chosen to sit one below
  # a UDP port this test already holds, because the audio port is derived and
  # never configurable -- that is the property being guarded -- so the test has
  # to pick the control port to suit the receiver rather than the other way
  # round.
  target_link_libraries(cwa_diagnostics_audio_stream_test PRIVATE
    Qt6::Core Qt6::Network)
  target_include_directories(cwa_diagnostics_audio_stream_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_diagnostics_audio_stream_test
    COMMAND cwa_diagnostics_audio_stream_test)
  set_tests_properties(cwa_diagnostics_audio_stream_test PROPERTIES TIMEOUT 120)

  qt_add_executable(cwa_update_checker_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/update_checker_test.cpp
    updates/update_checker.cpp
    updates/update_checker.hpp
  )
  target_link_libraries(cwa_update_checker_test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network)
  target_include_directories(cwa_update_checker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_update_checker_test COMMAND cwa_update_checker_test)
  set_tests_properties(cwa_update_checker_test PROPERTIES TIMEOUT 10)

  qt_add_executable(cwa_callsign_database_updater_test
    ${PROJECT_SOURCE_DIR}/tests/desktop/callsign_database_updater_test.cpp
    settings/product_migration.cpp
    settings/product_migration.hpp
    updates/callsign_database_updater.cpp
    updates/callsign_database_updater.hpp
  )
  target_link_libraries(cwa_callsign_database_updater_test PRIVATE
    cwa_core Qt6::Core Qt6::Network)
  target_include_directories(cwa_callsign_database_updater_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cwa_callsign_database_updater_test
    COMMAND cwa_callsign_database_updater_test)
  set_tests_properties(cwa_callsign_database_updater_test PROPERTIES TIMEOUT 15)

  if(CWA_ENABLE_ONNX_CHARACTER_DECODER)
    qt_add_executable(cwa_onnx_character_decoder_backend_test
      ${PROJECT_SOURCE_DIR}/tests/desktop/onnx_character_decoder_backend_test.cpp
      decoder/onnx_character_decoder_backend.cpp
      decoder/onnx_character_decoder_backend.hpp)
    target_link_libraries(cwa_onnx_character_decoder_backend_test PRIVATE
      cwa_core Qt6::Core)
    cwa_target_link_onnx_runtime(cwa_onnx_character_decoder_backend_test)
    target_compile_definitions(cwa_onnx_character_decoder_backend_test PRIVATE
      CWA_HAVE_ONNX_RUNTIME=1)
    target_include_directories(cwa_onnx_character_decoder_backend_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME cwa_onnx_character_decoder_backend_test
      COMMAND cwa_onnx_character_decoder_backend_test)
    set_tests_properties(cwa_onnx_character_decoder_backend_test PROPERTIES
      TIMEOUT 10)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS cw-buddy-desktop
  BUNDLE DESTINATION .
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

set(cwa_sdr_deploy_arguments "")
if(APPLE AND CWA_HAVE_SOAPY_SDR AND CWA_SOAPY_SDR_MODULE)
  if(NOT EXISTS "${CWA_SOAPY_SDR_MODULE}")
    message(FATAL_ERROR
      "CWA_SOAPY_SDR_MODULE does not exist: ${CWA_SOAPY_SDR_MODULE}")
  endif()
  get_filename_component(cwa_soapy_module_name
    "${CWA_SOAPY_SDR_MODULE}" NAME)
  install(FILES "${CWA_SOAPY_SDR_MODULE}"
    DESTINATION
      "cw-buddy-desktop.app/Contents/PlugIns/SoapySDR")
  # macdeployqt follows this runtime-loaded module as an additional Mach-O
  # root, copies librtlsdr/libusb into Frameworks, rewrites install names, and
  # includes the module in its final bundle signature.
  list(APPEND cwa_sdr_deploy_arguments DEPLOY_TOOL_OPTIONS
    "-executable=cw-buddy-desktop.app/Contents/PlugIns/SoapySDR/${cwa_soapy_module_name}")
endif()

if(CWA_HAVE_SOAPY_SDR AND CWA_SOAPY_SDR_LICENSE_DIR)
  if(NOT IS_DIRECTORY "${CWA_SOAPY_SDR_LICENSE_DIR}")
    message(FATAL_ERROR
      "CWA_SOAPY_SDR_LICENSE_DIR is not a directory: ${CWA_SOAPY_SDR_LICENSE_DIR}")
  endif()
  install(DIRECTORY "${CWA_SOAPY_SDR_LICENSE_DIR}/"
    DESTINATION
      "${CMAKE_INSTALL_DATAROOTDIR}/doc/cw-buddy/third-party/sdr-runtime")
endif()

qt_generate_deploy_qml_app_script(
  TARGET cw-buddy-desktop
  OUTPUT_SCRIPT cwa_deploy_script
  ${cwa_sdr_deploy_arguments}
  NO_UNSUPPORTED_PLATFORM_ERROR
)

if(UNIX AND NOT APPLE)
  install(FILES packaging/cw-buddy.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
  install(FILES packaging/cw-buddy.png
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps)
  install(FILES packaging/it.iu0lfq.CWBuddy.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
endif()

install(FILES ${PROJECT_SOURCE_DIR}/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cw-buddy
  RENAME copyright)
install(FILES ${PROJECT_SOURCE_DIR}/docs/licensing.md
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cw-buddy)
if(WIN32 AND CWA_HAVE_SOAPY_SDR)
  if(NOT CWA_SOAPY_SDR_RUNTIME_ROOT)
    message(FATAL_ERROR
      "Windows SDR-enabled packages require CWA_SOAPY_SDR_RUNTIME_ROOT")
  endif()
  foreach(cwa_sdr_runtime_file IN ITEMS
      "bin/SoapySDR.dll"
      "bin/rtlsdr.dll"
      "bin/libusb-1.0.dll"
      "bin/pthreadVC3.dll"
      "lib/SoapySDR/modules0.8/rtlsdrSupport.dll"
      "lib/SoapySDR/modules0.8/sdrPlaySupport.dll"
      "share/licenses/soapysdr/LICENSE_1_0.txt"
      "share/licenses/soapyrtlsdr/LICENSE.txt"
      "share/licenses/soapysdrplay3/LICENSE.txt"
      "share/licenses/rtlsdr/copyright"
      "share/licenses/libusb/copyright"
      "share/licenses/pthreads/copyright"
      "BUILD_PROVENANCE")
    if(NOT EXISTS "${CWA_SOAPY_SDR_RUNTIME_ROOT}/${cwa_sdr_runtime_file}")
      message(FATAL_ERROR
        "The prepared Windows SDR runtime is incomplete: ${cwa_sdr_runtime_file}")
    endif()
  endforeach()
  install(DIRECTORY "${CWA_SOAPY_SDR_RUNTIME_ROOT}/bin/"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    FILES_MATCHING PATTERN "*.dll")
  install(DIRECTORY "${CWA_SOAPY_SDR_RUNTIME_ROOT}/lib/SoapySDR"
    DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY "${CWA_SOAPY_SDR_RUNTIME_ROOT}/share/licenses/"
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cw-buddy/third-party/sdr)
  install(FILES "${CWA_SOAPY_SDR_RUNTIME_ROOT}/BUILD_PROVENANCE"
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cw-buddy/third-party/sdr)
endif()
if(APPLE)
  # Keep the canonical record inside the self-contained application bundle.
  # The custom Info.plist reads the same CWA_BUILD_VERSION directly so bundle
  # metadata and this machine-readable record cannot diverge through target
  # property/template expansion.
  install(FILES "${CMAKE_BINARY_DIR}/cw-buddy-version.txt"
    DESTINATION "cw-buddy-desktop.app/Contents/Resources"
    RENAME VERSION)
else()
  install(FILES "${CMAKE_BINARY_DIR}/cw-buddy-version.txt"
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cw-buddy
    RENAME VERSION)
endif()
install(DIRECTORY ${PROJECT_SOURCE_DIR}/docs/manuals/
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cw-buddy/manuals
  FILES_MATCHING PATTERN "*.md")

# Hosted builds deploy Qt shared libraries, so preserve the corresponding Qt
# license texts beside the application documentation when the SDK provides them.
get_filename_component(cwa_qt_prefix "${Qt6Core_DIR}/../../.." ABSOLUTE)
if(EXISTS "${cwa_qt_prefix}/LICENSES")
  install(DIRECTORY "${cwa_qt_prefix}/LICENSES/"
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/cw-buddy/third-party/qt6)
endif()

# Deployment copies and signs the completed macOS bundle. Keep it after every
# install rule that can add a sealed resource (notably Contents/Resources/VERSION)
# so the published bundle's final resource envelope remains valid.
install(SCRIPT ${cwa_deploy_script})

if(WIN32)
  target_link_libraries(cw-buddy-desktop PRIVATE ole32 oleaut32)
endif()

if(MSVC)
  target_compile_options(cw-buddy-desktop PRIVATE /W4 /permissive-)
else()
  target_compile_options(cw-buddy-desktop PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Same reasoning as tests/CMakeLists.txt: the Morse alphabet has no compiled-in
# fallback, so every desktop test is pointed at the shipped dictionaries.
if(CWA_BUILD_TESTS)
  get_property(cwa_desktop_tests DIRECTORY PROPERTY TESTS)
  foreach(cwa_test IN LISTS cwa_desktop_tests)
    set_property(TEST ${cwa_test} APPEND PROPERTY ENVIRONMENT
      "CWA_DICTIONARY_DIR=${PROJECT_SOURCE_DIR}/dictionaries")
  endforeach()
endif()
