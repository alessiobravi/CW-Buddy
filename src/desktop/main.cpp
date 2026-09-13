#include <QDateTime>
#include <QMutex>
#include <QTextStream>
#include <QGuiApplication>
#include <QJsonObject>

#include <cstring>
#include <QCoreApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include "cwassistant/core/conversation_profile.hpp"
#include "cwassistant/core/cw_morse_alphabet.hpp"
#include "cwassistant/core/cw_callsign_prefixes.hpp"
#include "cwassistant/core/cw_vocabulary.hpp"
#include <array>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <qqml.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

#include "replay/replay_controller.hpp"
#include "sdr/sdr_receiver.hpp"
#include "sdr/sdr_runtime_environment.hpp"
#include "diagnostics/diagnostics_server.hpp"
#include "settings/app_settings.hpp"
#include "settings/product_migration.hpp"
#include "transmit/transmit_controller.hpp"
#include "updates/callsign_database_updater.hpp"
#include "updates/update_checker.hpp"
#include "visualization/spectrum_waterfall_item.hpp"

namespace {

bool sdr_backend_smoke_requested(const int argc, char* argv[]) {
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--sdr-backend-smoke-test") {
      return true;
    }
  }
  return false;
}

int run_sdr_backend_smoke() {
  cwassistant::desktop::SdrReceiver receiver(
      cwassistant::desktop::makeSoapySdrReceiveBackend());
  const auto report = receiver.discover();
  const bool rtl_module_loaded = std::ranges::find(
                                     report.loaded_drivers, "rtlsdr") !=
                                 report.loaded_drivers.end();
  if (!report.backend_available || !rtl_module_loaded) {
    std::cerr << "SDR backend smoke failed: " << report.diagnostic << '\n';
    return 2;
  }
  std::cout << "SDR backend smoke passed: SoapySDR "
            << report.backend_version << ", RTL-SDR module loaded\n";
  return 0;
}

}  // namespace

namespace {

// Seeds the operator's dictionary directory from the copies inside the binary
// on first run, then loads it. The operator's files win when present so an
// edited vocabulary survives an upgrade; the built-in copies are the fallback
// and guarantee the decoder is never left with no vocabulary at all. Returns
// the number of exchange words available.
std::size_t loadCwDictionaries(const QString& app_data_path) {
  static constexpr std::array<const char*, 5> kFiles{
      "cw-abbreviations.txt", "cw-word-gap-prefixes.txt",
      "morse-alphabet.txt", "cw-distinctive-tokens.txt",
      "callsign-prefixes.txt"};
  const QDir directory(app_data_path + QStringLiteral("/dictionaries"));
  QDir().mkpath(directory.absolutePath());

  const auto bundled_contents = [](const char* name) {
    QFile bundled(QStringLiteral(":/dictionaries/") +
                  QString::fromLatin1(name));
    if (!bundled.open(QIODevice::ReadOnly)) return QByteArray{};
    return bundled.readAll();
  };

  // The copy inside the application is authoritative and is always what gets
  // used unless the operator's copy is both present and usable. An upgrade
  // must never depend on what an earlier version left in the data directory:
  // a file written by a previous release, truncated, or half-written, must
  // not be able to change how a new version decodes. The operator's copy is
  // therefore an override that has to earn its place, not the primary source.
  const auto read = [&directory, &bundled_contents](
                        const char* name, const auto& usable) {
    const QByteArray bundled = bundled_contents(name);
    const QString editable = directory.filePath(QString::fromLatin1(name));
    QFile file(editable);
    QByteArray operator_copy;
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
      operator_copy = file.readAll();
      file.close();
    }
    if (!operator_copy.isEmpty() && usable(operator_copy)) return operator_copy;
    if (bundled.isEmpty()) return operator_copy;
    // Replace a missing or unusable copy so the operator sees what is actually
    // in force and can edit from it. A usable copy is never overwritten.
    if (operator_copy.isEmpty() || !usable(operator_copy)) {
      QFile seed(editable);
      if (seed.open(QIODevice::WriteOnly)) {
        seed.write(bundled);
        seed.close();
      }
    }
    return bundled;
  };

  // A dictionary is usable when it parses to something. The alphabet has to
  // carry the letters and digits as well, because a file that parses to three
  // entries would leave the decoder reading almost nothing.
  const auto parses_to_tokens = [](const QByteArray& text) {
    cwassistant::core::CwVocabulary probe;
    return probe.importExchangeWords(
               std::string_view(text.constData(),
                                static_cast<std::size_t>(text.size())))
               .inserted_tokens > 0U;
  };
  const auto parses_to_alphabet = [](const QByteArray& text) {
    cwassistant::core::CwMorseAlphabet probe;
    static_cast<void>(probe.importText(
        std::string_view(text.constData(),
                         static_cast<std::size_t>(text.size()))));
    return probe.symbolFor(".-") == "A" && probe.symbolFor("-----") == "0";
  };
  // A prefix table is usable when it covers the allocations an operator will
  // certainly hear. A file that parsed but had lost most of its blocks would
  // silently refuse real stations, which is the one failure this table must
  // not have, so a truncated copy is rejected in favour of the built-in one.
  const auto parses_to_prefixes = [](const QByteArray& text) {
    cwassistant::core::CwCallsignPrefixTable probe;
    static_cast<void>(probe.importText(
        std::string_view(text.constData(),
                         static_cast<std::size_t>(text.size()))));
    return probe.isAllocatedPrefix("W1AW") && probe.isAllocatedPrefix("IU0LFQ") &&
           probe.isAllocatedPrefix("G4ABC") && probe.isAllocatedPrefix("JA1XYZ") &&
           !probe.isAllocatedPrefix("QK7SS");
  };

  auto& vocabulary = cwassistant::core::cwSharedVocabulary();
  vocabulary.clear();
  const QByteArray words = read(kFiles[0], parses_to_tokens);
  const QByteArray prefixes = read(kFiles[1], parses_to_tokens);
  static_cast<void>(vocabulary.importExchangeWords(
      std::string_view(words.constData(),
                       static_cast<std::size_t>(words.size()))));
  static_cast<void>(vocabulary.importWordGapPrefixes(
      std::string_view(prefixes.constData(),
                       static_cast<std::size_t>(prefixes.size()))));

  // Loaded explicitly rather than left to the shared instance's own recovery,
  // so that the operator's copy is what takes effect when there is one. The
  // compiled-in copy behind it is a last resort, not the normal path.
  const QByteArray distinctive = read(kFiles[3], parses_to_tokens);
  static_cast<void>(vocabulary.importDistinctiveTokens(
      std::string_view(distinctive.constData(),
                       static_cast<std::size_t>(distinctive.size()))));

  // Contest exchanges are a directory rather than a single file. Seed it the
  // same way, then read the operator's copy so an updated or added contest
  // needs no rebuild. An extra file the operator adds is read too; only the
  // ones carried inside the application are ever written out.
  static constexpr std::array<const char*, 4> kContests{
      "cq-ww-cw.txt", "cq-wpx-cw.txt", "arrl-field-day-cw.txt",
      "arrl-sweepstakes-cw.txt"};
  const QDir contests(directory.filePath(QStringLiteral("contests")));
  QDir().mkpath(contests.absolutePath());
  for (const char* name : kContests) {
    const QString target = contests.filePath(QString::fromLatin1(name));
    if (QFile::exists(target)) continue;
    QFile bundled(QStringLiteral(":/dictionaries/contests/") +
                  QString::fromLatin1(name));
    if (!bundled.open(QIODevice::ReadOnly)) continue;
    QFile out(target);
    if (out.open(QIODevice::WriteOnly)) out.write(bundled.readAll());
  }

  static_cast<void>(cwassistant::core::load_conversation_profile_directory(
      (directory.absolutePath() + QStringLiteral("/contests"))
          .toStdString()));

  const QByteArray alphabet = read(kFiles[2], parses_to_alphabet);
  auto& morse = cwassistant::core::cwMutableSharedMorseAlphabet();
  morse.clear();
  static_cast<void>(morse.importText(
      std::string_view(alphabet.constData(),
                       static_cast<std::size_t>(alphabet.size()))));

  // Loaded the same way, and for the same reason: the table decides which
  // decoded tokens may name a station, so the copy the operator can see and
  // edit is the one that has to be in force.
  const QByteArray callsign_prefixes = read(kFiles[4], parses_to_prefixes);
  auto& prefix_table = cwassistant::core::cwMutableSharedCallsignPrefixes();
  prefix_table.clear();
  static_cast<void>(prefix_table.importText(
      std::string_view(callsign_prefixes.constData(),
                       static_cast<std::size_t>(callsign_prefixes.size()))));
  return vocabulary.exchangeWordCount();
}

// Writes Qt's own diagnostic output to a file for the life of the session.
//
// Nothing was recorded outside a debug capture, so an operator seeing the
// application misbehave had nothing to send but a description, and a fault
// that did not reproduce here could not be narrowed at all. Opt-in, because a
// log nobody asked for is a file that grows on somebody's machine forever:
// pass --log-file <path>, or set CWA_LOG_FILE.
QFile* g_log_file = nullptr;
QMutex g_log_mutex;

void writeLogMessage(const QtMsgType type, const QMessageLogContext& context,
                     const QString& message) {
  static const char* const kLevels[] = {"debug", "warning", "critical",
                                        "fatal", "info"};
  const QMutexLocker locker(&g_log_mutex);
  if (g_log_file == nullptr) return;
  QTextStream stream(g_log_file);
  stream << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << ' '
         << kLevels[static_cast<int>(type) < 5 ? static_cast<int>(type) : 0]
         << ' ' << (context.category != nullptr ? context.category : "default")
         << ": " << message << '\n';
  // Flushed every line on purpose. A log that loses its last buffer is silent
  // about exactly the moment worth reading: the one before a crash or a hang.
  stream.flush();
}

[[nodiscard]] QString requested_log_file_path(const int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--log-file") == 0 && index + 1 < argc)
      return QString::fromLocal8Bit(argv[index + 1]);
  }
  return qEnvironmentVariable("CWA_LOG_FILE");
}

}  // namespace

int main(int argc, char* argv[]) {
  if (const QString log_path = requested_log_file_path(argc, argv);
      !log_path.isEmpty()) {
    auto* file = new QFile(log_path);
    if (file->open(QIODevice::WriteOnly | QIODevice::Append |
                   QIODevice::Text)) {
      g_log_file = file;
      qInstallMessageHandler(writeLogMessage);
    } else {
      delete file;
    }
  }
  if (sdr_backend_smoke_requested(argc, argv)) {
    QCoreApplication application(argc, argv);
    cwassistant::desktop::configureBundledSoapyRuntime(
        QCoreApplication::applicationDirPath());
    return run_sdr_backend_smoke();
  }

  QGuiApplication application(argc, argv);
  // Resolve the legacy locations before adopting the new public identity.
  // The old bundle identifier remains stable for installer compatibility,
  // while Qt settings and the managed SCP cache move forward once and safely.
  QCoreApplication::setOrganizationName(QStringLiteral("CW Assistant"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("cw-assistant.org"));
  QCoreApplication::setApplicationName(QStringLiteral("CW Assistant"));
  const QString legacy_app_data_path =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QSettings legacy_settings;

  QCoreApplication::setOrganizationName(QStringLiteral("CW Buddy"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("cw-buddy.org"));
  QCoreApplication::setApplicationName(QStringLiteral("CW Buddy"));
  const QString current_app_data_path =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QSettings current_settings;
  static_cast<void>(cwassistant::desktop::migrateLegacyProductState(
      legacy_settings, current_settings, legacy_app_data_path,
      current_app_data_path));
  static_cast<void>(loadCwDictionaries(current_app_data_path));
  QCoreApplication::setApplicationVersion(QStringLiteral(CWA_VERSION));
  application.setWindowIcon(QIcon(QStringLiteral(":/icons/cw-buddy.png")));
  cwassistant::desktop::configureBundledSoapyRuntime(
      QCoreApplication::applicationDirPath());

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Cross-platform multi-channel CW operating assistant"));
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption profile_option(
      QStringList{QStringLiteral("p"), QStringLiteral("profile")},
      QStringLiteral("Use an isolated station configuration profile."),
      QStringLiteral("name"), QStringLiteral("default"));
  parser.addOption(profile_option);
  QCommandLineOption smoke_test_option(
      QStringLiteral("smoke-test"),
      QStringLiteral("Load and render the desktop shell, then exit."));
  parser.addOption(smoke_test_option);
  parser.process(application);

  QQuickStyle::setStyle(QStringLiteral("Material"));

  cwassistant::desktop::AppSettings settings(parser.value(profile_option),
                                             parser.isSet(profile_option));
  if (parser.isSet(smoke_test_option)) {
    settings.setOwnCallsign(QStringLiteral(" iu0lfq/p "));
  }
  cwassistant::desktop::ReplayController replay_controller;
  cwassistant::desktop::UpdateChecker update_checker;
  cwassistant::desktop::CallsignDatabaseUpdater callsign_database_updater;
  cwassistant::desktop::TransmitController transmit_controller;
  transmit_controller.setOwnCallsign(settings.ownCallsign());
  const auto apply_spectrum_processing = [&settings, &replay_controller] {
    replay_controller.setAveragingFrames(settings.averagingFrames());
    replay_controller.setSpectrumProcessing(
        settings.audioDcRejection(), settings.audioAutomaticGain(),
        settings.audioGainDb(), settings.audioAutomaticGainTargetDbfs(),
        settings.audioAutomaticBandwidth(), settings.audioLowerFrequencyHz(),
        settings.audioUpperFrequencyHz(), settings.waterfallRate());
  };
  // The spot feed is receive-only and carries no authority: it can place a
  // marker where another receiver reports a station, and can corroborate a
  // callsign this receiver decoded for itself, but it never supplies one.
  //
  // The cluster logs in as the station callsign rather than one of its own, so
  // this has to be reapplied when the callsign changes and not only when a
  // dxcluster setting does. Both arrive on settingsChanged, so one connection
  // covers it.
  //
  // dxClusterLoginCallsign rather than ownCallsign: it is the station callsign
  // with the optional cluster SSID appended, which is how an operator running
  // more than one connection from one station is told apart on a node.
  const auto apply_dx_cluster = [&settings, &replay_controller] {
    replay_controller.configureDxCluster(
        settings.dxClusterEnabled(), settings.dxClusterServerIndex(),
        settings.dxClusterCustomHost(), settings.dxClusterCustomPort(),
        settings.dxClusterLoginCallsign(), settings.dxSpotsRetentionMinutes(),
        settings.dxSpotsToleranceHz());
  };
  const auto apply_radio_frequency = [&settings, &replay_controller] {
    const auto rx_rf_hz = settings.controlledRxRfHz();
    const auto tx_rf_hz = settings.controlledTxRfHz();
    replay_controller.setRadioFrequencyContext(
        rx_rf_hz.has_value(),
        rx_rf_hz ? static_cast<qulonglong>(*rx_rf_hz) : 0,
        tx_rf_hz ? static_cast<qulonglong>(*tx_rf_hz) : 0,
        settings.controlledSplitActive(), settings.cwToneSidebandIndex(),
        settings.cwGuideCenterHz());
  };
  const auto apply_decoded_signal_timeout = [&settings, &replay_controller] {
    replay_controller.setDecodedSignalTimeoutSeconds(
        settings.decodedSignalTimeoutSeconds());
  };
  // Must be applied after the decoded-signal timeout, both here and in the
  // change notifications below. That timeout reaches the channel bank through
  // a configure() call that replaces the whole configuration, so applying the
  // weak-signal gate first would let the next timeout update quietly restore
  // the bank's built-in defaults over the operator's choice.
  const auto apply_weak_signal_decoding = [&settings, &replay_controller] {
    replay_controller.setWeakSignalDecoding(settings.decodeWeakSignals(),
                                            settings.minimumDecodeSnrDb());
  };
  const auto apply_local_character_decoder = [&settings,
                                               &replay_controller] {
    replay_controller.configureLocalCharacterDecoder(
        settings.localDecoderEnabled(), settings.localDecoderModelPath(),
        settings.localDecoderMetadataPath());
  };
  const auto apply_own_callsign = [&settings, &replay_controller] {
    replay_controller.setOwnCallsign(settings.ownCallsign());
  };
  const auto apply_callsign_database_correction = [&settings,
                                                   &replay_controller] {
    replay_controller.setCallsignDatabaseCorrectionEnabled(
        settings.callsignDatabaseCorrectionEnabled());
  };
  const auto apply_keying_model = [&settings, &replay_controller] {
    replay_controller.setKeyingModel(settings.keyingModel());
  };
  const auto apply_operator_role = [&settings, &replay_controller] {
    replay_controller.setOperatorRole(settings.operatorRole());
  };
  const auto apply_debug_capture_limit = [&settings, &replay_controller] {
    replay_controller.setDebugCaptureMaximumSeconds(
        settings.debugCaptureMaximumSeconds());
  };
  const auto apply_transmit_hardware = [&settings, &transmit_controller] {
    transmit_controller.configureHardware(
        settings.radioEnabled() && settings.directKeyingEnabled(),
        settings.keyingPort(), settings.pttLineIndex(), settings.keyLineIndex(),
        settings.pttActiveHigh(), settings.keyActiveHigh(), settings.catPort(),
        settings.directKeyingValidated());
  };
  const auto apply_transmit_speed = [&settings, &transmit_controller] {
    transmit_controller.configureTxSpeed(settings.txSpeedMode(),
                                         settings.fixedTxWpm());
  };
  const auto apply_transmit_radio_safety = [&settings, &transmit_controller] {
    const auto tx_rf_hz = settings.controlledTxRfHz();
    transmit_controller.configureRadioSafety(
        settings.radioEnabled(),
        tx_rf_hz ? static_cast<qulonglong>(*tx_rf_hz) : 0U,
        settings.radioTxModeTarget(), settings.radioTxModeConfirmed(),
        settings.radioSplitKnown(), settings.controlledSplitActive());
  };
  const auto follow_sdr_to_radio_vfo = [&settings] {
    settings.followSdrToRadioVfo();
  };
  const auto apply_sdr_input = [&settings, &replay_controller] {
    const qint64 sample_rate_hz = settings.sdrSampleRateHz();
    const qint64 decoder_bandwidth_hz = std::clamp<qint64>(
        settings.sdrDecoderBandwidthHz(), 2'000,
        std::max<qint64>(2'000, sample_rate_hz - 2'000));
    const qint64 acquisition_center_hz =
        static_cast<qint64>(settings.sdrCenterFrequencyHz());
    const qint64 edge_margin_hz = decoder_bandwidth_hz / 2 + 1'000;
    const qint64 lowest_decoder_center =
        acquisition_center_hz - sample_rate_hz / 2 + edge_margin_hz;
    const qint64 highest_decoder_center =
        acquisition_center_hz + sample_rate_hz / 2 - edge_margin_hz;
    const qint64 decoder_center_hz = std::clamp<qint64>(
        static_cast<qint64>(settings.sdrDecoderCenterFrequencyHz()),
        lowest_decoder_center, highest_decoder_center);
    if (settings.sdrDecoderBandwidthHz() != decoder_bandwidth_hz)
      settings.setSdrDecoderBandwidthHz(
          static_cast<int>(decoder_bandwidth_hz));
    if (static_cast<qint64>(settings.sdrDecoderCenterFrequencyHz()) !=
        decoder_center_hz)
      settings.setSdrDecoderCenterFrequencyHz(
          static_cast<qulonglong>(decoder_center_hz));
    replay_controller.setSdrInputSelection(
        settings.sdrDeviceId(), settings.sdrDeviceDisplayName(),
        settings.sdrCenterFrequencyHz(), settings.sdrSampleRateHz(),
        settings.sdrBandwidthHz(), settings.sdrAntenna(),
        settings.sdrAutomaticGain(),
        settings.sdrGainDb(),
        static_cast<qulonglong>(decoder_center_hz),
        static_cast<int>(decoder_bandwidth_hz));
  };
  const auto apply_offline_callsign_database =
      [&settings, &replay_controller, &callsign_database_updater] {
    if (callsign_database_updater.managedEnabled()) {
      const QString path = callsign_database_updater.installedFilePath();
      if (!path.isEmpty()) {
        replay_controller.configureOfflineCallsignDatabase(true, path);
        return;
      }
    }
    replay_controller.configureOfflineCallsignDatabase(
        settings.localCallsignDatabaseEnabled(),
        settings.localCallsignDatabasePath());
  };
  apply_spectrum_processing();
  apply_radio_frequency();
  apply_decoded_signal_timeout();
  apply_weak_signal_decoding();
  apply_dx_cluster();

  // Come back where the operator left off.
  //
  // The receiver source was not persisted at all, so every restart landed in
  // sound-card audio; and the saved SDR device, which was persisted, could not
  // be used because the Start control is gated on an index into the discovered
  // device list and discovery only ran when the operator opened the SDR
  // settings tab. A saved receiver was therefore present and unusable at the
  // same time, which is what "I am not able to start SDR RX after a restart"
  // was.
  replay_controller.setSourceMode(settings.preferredSourceMode());
  QObject::connect(&replay_controller,
                   &cwassistant::desktop::ReplayController::stateChanged,
                   &settings, [&settings, &replay_controller] {
                     settings.setPreferredSourceMode(
                         replay_controller.sourceMode());
                   });
  // The fallback. Settings reports whether the saved receiver came back; what
  // to do about it is the application's decision, and a settings object that
  // silently changed the operator's source would be the wrong place for it.
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::sdrSelectionRestored,
      &replay_controller,
      [&replay_controller](const bool found, const QString& saved_name) {
        if (found || replay_controller.sourceMode() != 2) return;
        // Sound-card audio and file replay are both still available, so
        // sitting in a source that cannot start would strand the operator in
        // the one mode that does nothing. The status line already names the
        // receiver that is missing.
        static_cast<void>(saved_name);
        replay_controller.setSourceMode(0);
      });
  // Discovery runs off the drawing thread and returns immediately when no
  // receiver was ever saved, so a station that has never used an SDR pays
  // nothing for this.
  settings.restoreSdrSelectionAtStartup();
  apply_local_character_decoder();
  apply_callsign_database_correction();
  apply_keying_model();
  apply_operator_role();
  apply_debug_capture_limit();
  apply_own_callsign();
  apply_offline_callsign_database();
  apply_transmit_hardware();
  apply_transmit_speed();
  apply_transmit_radio_safety();
  follow_sdr_to_radio_vfo();
  replay_controller.setAudioInputSelection(settings.audioInputId(),
                                           settings.audioInputDisplayName(),
                                           settings.audioInputDeviceName());
  apply_sdr_input();
  if (settings.receiverInputTypeIndex() == 1)
    replay_controller.setSourceMode(2);
  replay_controller.setMonitorOutputSelection(settings.audioOutputId());
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_spectrum_processing);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_radio_frequency);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_decoded_signal_timeout);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_weak_signal_decoding);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_dx_cluster);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_callsign_database_correction);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_keying_model);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_debug_capture_limit);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_operator_role);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &replay_controller, apply_own_callsign);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &transmit_controller,
      [&settings, &transmit_controller] {
        transmit_controller.setOwnCallsign(settings.ownCallsign());
      });
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &transmit_controller, apply_transmit_hardware);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &transmit_controller, apply_transmit_speed);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::settingsChanged,
      &transmit_controller, apply_transmit_radio_safety);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::radioFrequencyChanged,
      &transmit_controller, apply_transmit_radio_safety);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::cat4omChanged,
      &transmit_controller, apply_transmit_radio_safety);
  QObject::connect(&application, &QCoreApplication::aboutToQuit,
                   &transmit_controller, &cwassistant::desktop::TransmitController::disarm);
  QObject::connect(
      &replay_controller, &cwassistant::desktop::ReplayController::decoderChanged,
      &transmit_controller,
      [&replay_controller, &transmit_controller] {
        transmit_controller.observeDecoderChannels(
            replay_controller.decoderChannels());
      });
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::cat4omChanged,
      &replay_controller, apply_radio_frequency);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::radioFrequencyChanged,
      &replay_controller, apply_radio_frequency);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::radioFrequencyChanged,
      &settings, follow_sdr_to_radio_vfo);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::cat4omChanged,
      &settings, follow_sdr_to_radio_vfo);
  QObject::connect(
      &settings,
      &cwassistant::desktop::AppSettings::localCallsignDatabaseConfigurationCommitted,
      &replay_controller, apply_offline_callsign_database);
  QObject::connect(
      &callsign_database_updater,
      &cwassistant::desktop::CallsignDatabaseUpdater::databaseInstalled,
      &replay_controller, apply_offline_callsign_database);
  QObject::connect(
      &callsign_database_updater,
      &cwassistant::desktop::CallsignDatabaseUpdater::managedEnabledChanged,
      &replay_controller,
      [&callsign_database_updater, &apply_offline_callsign_database,
       smoke_test = parser.isSet(smoke_test_option)] {
        apply_offline_callsign_database();
        if (!smoke_test && callsign_database_updater.managedEnabled() &&
            callsign_database_updater.autoUpdateEnabled()) {
          callsign_database_updater.checkAndInstallIfDue();
        }
      });
  QObject::connect(
      &callsign_database_updater,
      &cwassistant::desktop::CallsignDatabaseUpdater::autoUpdateEnabledChanged,
      &callsign_database_updater,
      [&callsign_database_updater,
       smoke_test = parser.isSet(smoke_test_option)] {
        if (!smoke_test && callsign_database_updater.managedEnabled() &&
            callsign_database_updater.autoUpdateEnabled()) {
          callsign_database_updater.checkAndInstallIfDue();
        }
      });
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::audioInputsChanged,
      &replay_controller, [&settings, &replay_controller] {
        replay_controller.setAudioInputSelection(
            settings.audioInputId(), settings.audioInputDisplayName(),
            settings.audioInputDeviceName());
      });
  // An input found by name after its identifier changed is adopted for good,
  // so the next start matches on the identifier and never has to recover a
  // second time.
  QObject::connect(
      &replay_controller,
      &cwassistant::desktop::ReplayController::audioInputRecovered, &settings,
      [&settings](const QString& adopted_id) {
        settings.adoptRecoveredAudioInput(adopted_id);
      });
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::sdrSettingsChanged,
      &replay_controller, apply_sdr_input);
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::receiverInputTypeChanged,
      &replay_controller, [&settings, &replay_controller] {
        replay_controller.setSourceMode(
            settings.receiverInputTypeIndex() == 1 ? 2 : 0);
      });
  QObject::connect(
      &settings, &cwassistant::desktop::AppSettings::audioOutputsChanged,
      &replay_controller, [&settings, &replay_controller] {
        replay_controller.setMonitorOutputSelection(settings.audioOutputId());
      });
  QObject::connect(
      &settings,
      &cwassistant::desktop::AppSettings::localDecoderConfigurationCommitted,
      &replay_controller, apply_local_character_decoder);
  // The live diagnostics stream. Owned here rather than by the controller,
  // which relays records without knowing where they go: the controller has no
  // business holding network addresses, and the server has none decoding.
  cwassistant::desktop::DiagnosticsServer diagnostics_server;
  // configure() rebinds unconditionally on purpose, so that re-applying the
  // same settings is how an operator retries after fixing the network under
  // it. That makes it the wrong thing to call on every settingsChanged, which
  // fires for anything an operator edits anywhere: each unrelated change would
  // drop every connected observer and rebuild the sockets. Only a change to
  // what the service is actually bound to reaches it.
  QStringList applied_diagnostics_addresses;
  int applied_diagnostics_port = -1;
  QString applied_diagnostics_token;
  QStringList applied_diagnostics_peers;
  bool applied_diagnostics_peers_valid = false;
  const auto apply_diagnostics_server =
      [&settings, &diagnostics_server, &applied_diagnostics_addresses,
       &applied_diagnostics_port, &applied_diagnostics_token,
       &applied_diagnostics_peers, &applied_diagnostics_peers_valid] {
        const QStringList addresses = settings.diagnosticsServerAddresses();
        const int port = settings.diagnosticsServerPort();
        const QString token = settings.diagnosticsServerToken();
        if (addresses != applied_diagnostics_addresses ||
            port != applied_diagnostics_port ||
            token != applied_diagnostics_token) {
          applied_diagnostics_addresses = addresses;
          applied_diagnostics_port = port;
          applied_diagnostics_token = token;
          diagnostics_server.configure(
              addresses, static_cast<std::uint16_t>(port), token);
        }
        // Applied separately from the binding, because tightening or widening
        // who may read does not need the sockets rebuilt and should not drop
        // the readers a still-permitted operator is watching with. The server
        // drops only those the new list no longer covers.
        const QStringList peers = settings.diagnosticsAllowedPeers();
        if (!applied_diagnostics_peers_valid ||
            peers != applied_diagnostics_peers) {
          applied_diagnostics_peers = peers;
          applied_diagnostics_peers_valid = true;
          diagnostics_server.setAllowedPeers(peers);
        }
        diagnostics_server.setEnabled(settings.diagnosticsServerEnabled());
      };
  QObject::connect(&settings,
                   &cwassistant::desktop::AppSettings::settingsChanged,
                   &diagnostics_server, apply_diagnostics_server);
  // One record in, one record out. Nothing on this path can reach the radio,
  // and nothing arriving on the socket reaches this path at all -- the stream
  // never reads from a client.
  QObject::connect(
      &replay_controller,
      &cwassistant::desktop::ReplayController::diagnosticsRecordProduced,
      &diagnostics_server,
      [&diagnostics_server](const QJsonObject& record) {
        diagnostics_server.publish(record);
      });
  // Receive audio out, on the same terms as the records beside it. The
  // controller relays it on the DSP worker's thread and the audio plane
  // discards it in one atomic load when nobody has subscribed, so the whole
  // path costs a station with no observer nothing at all.
  QObject::connect(
      &replay_controller,
      &cwassistant::desktop::ReplayController::receiveAudioProduced,
      &diagnostics_server,
      [&diagnostics_server](const QByteArray& float_mono_audio,
                            const double sample_rate_hz) {
        diagnostics_server.publishAudio(float_mono_audio, sample_rate_hz);
      });
  // And the other direction: whether anybody is actually being sent audio
  // right now, so the region is demodulated for a listener who exists rather
  // than for a permission that was granted once. The diagnostics service is
  // the only thing that knows, and the decoder has no business asking it, so
  // the application tells the controller and the controller tells the worker.
  QObject::connect(&diagnostics_server,
                   &cwassistant::desktop::DiagnosticsServer::stateChanged,
                   &replay_controller,
                   [&diagnostics_server, &replay_controller] {
                     replay_controller.setRemoteAudioSubscribed(
                         diagnostics_server.audioSubscriberCount() > 0);
                   });
  apply_diagnostics_server();

  qmlRegisterType<cwassistant::desktop::SpectrumWaterfallItem>(
      "CWBuddy", 1, 0, "SpectrumWaterfall");
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("appSettings"),
                                           &settings);
  engine.rootContext()->setContextProperty(QStringLiteral("replayController"),
                                           &replay_controller);
  engine.rootContext()->setContextProperty(QStringLiteral("updateChecker"),
                                           &update_checker);
  engine.rootContext()->setContextProperty(
      QStringLiteral("callsignDatabaseUpdater"), &callsign_database_updater);
  engine.rootContext()->setContextProperty(QStringLiteral("transmitController"),
                                           &transmit_controller);
  engine.rootContext()->setContextProperty(QStringLiteral("diagnosticsServer"),
                                           &diagnostics_server);
  if (!parser.isSet(smoke_test_option) && update_checker.autoCheckEnabled()) {
    // A short delay so the background check never competes with startup
    // rendering/audio work; never runs during the smoke test, which must
    // stay hermetic (no real network access).
    QTimer::singleShot(4'000, &update_checker,
                       [&update_checker] { update_checker.checkForUpdates(); });
  }
  if (!parser.isSet(smoke_test_option) &&
      callsign_database_updater.managedEnabled() &&
      callsign_database_updater.autoUpdateEnabled()) {
    QTimer::singleShot(6'000, &callsign_database_updater,
                       [&callsign_database_updater] {
                         callsign_database_updater.checkAndInstallIfDue();
                       });
  }
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
      [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
  engine.loadFromModule(QStringLiteral("CWBuddy"), QStringLiteral("Main"));
  if (parser.isSet(smoke_test_option)) {
    QTimer::singleShot(250, &application, [&engine] {
      QObject* root_object = engine.rootObjects().isEmpty()
                                 ? nullptr
                                 : engine.rootObjects().constFirst();
      auto* display =
          root_object == nullptr
              ? nullptr
              : root_object
                    ->findChild<cwassistant::desktop::SpectrumWaterfallItem*>(
                        QStringLiteral("spectrumDisplay"));
      if (display == nullptr) {
        QCoreApplication::exit(EXIT_FAILURE);
        return;
      }
      auto* next_button =
          root_object->findChild<QQuickItem*>(QStringLiteral("setupNextButton"));
      auto* audio_input_combo = root_object->findChild<QQuickItem*>(
          QStringLiteral("setupAudioInputCombo"));
      auto* live_audio_button = root_object->findChild<QQuickItem*>(
          QStringLiteral("startLiveAudioButton"));
      auto* own_callsign_field = root_object->findChild<QQuickItem*>(
          QStringLiteral("ownCallsignField"));
      auto* dc_rejection_check = root_object->findChild<QQuickItem*>(
          QStringLiteral("audioDcRejectionCheck"));
      auto* automatic_gain_check = root_object->findChild<QQuickItem*>(
          QStringLiteral("audioAutomaticGainCheck"));
      auto* automatic_bandwidth_check = root_object->findChild<QQuickItem*>(
          QStringLiteral("audioAutomaticBandwidthCheck"));
      auto* live_levels_check = root_object->findChild<QQuickItem*>(
          QStringLiteral("liveAutomaticLevelsCheck"));
      auto* live_noise_check = root_object->findChild<QQuickItem*>(
          QStringLiteral("liveNoiseSuppressionCheck"));
      auto* live_cw_guide_check = root_object->findChild<QQuickItem*>(
          QStringLiteral("liveCwGuideCheck"));
      auto* tx_slice_guide = root_object->findChild<QQuickItem*>(
          QStringLiteral("txSliceGuideOverlay"));
      auto* decoder_channel_list = root_object->findChild<QQuickItem*>(
          QStringLiteral("decoderChannelList"));
      auto* live_controls = root_object->findChild<QQuickItem*>(
          QStringLiteral("liveControlsFrame"));
      auto* view_selector = root_object->findChild<QQuickItem*>(
          QStringLiteral("viewSelector"));
      auto* pin_live_controls = root_object->findChild<QQuickItem*>(
          QStringLiteral("pinLiveControlsButton"));
      auto* about_version_label = root_object->findChild<QQuickItem*>(
          QStringLiteral("aboutVersionLabel"));
      if (next_button == nullptr || !next_button->isVisible() ||
          next_button->width() < 1.0 || next_button->height() < 1.0 ||
          next_button->window() == nullptr ||
          next_button->mapToScene(
              QPointF(next_button->width(), next_button->height())).y() >
          next_button->window()->height() || audio_input_combo == nullptr ||
          audio_input_combo->property("count").toInt() < 1 ||
          audio_input_combo->property("currentIndex").toInt() < 0 ||
          live_audio_button == nullptr || own_callsign_field == nullptr ||
          dc_rejection_check == nullptr || automatic_gain_check == nullptr ||
          automatic_bandwidth_check == nullptr ||
          live_levels_check == nullptr || live_noise_check == nullptr ||
          live_cw_guide_check == nullptr || tx_slice_guide == nullptr ||
          decoder_channel_list == nullptr ||
          live_controls == nullptr || view_selector == nullptr ||
          pin_live_controls == nullptr ||
          live_controls->property("expanded").toBool() ||
          about_version_label == nullptr ||
          about_version_label->property("text").toString() !=
              QStringLiteral("Version %1").arg(
                  QCoreApplication::applicationVersion()) ||
          own_callsign_field->property("text").toString() !=
              QStringLiteral("IU0LFQ/P")) {
        QCoreApplication::exit(EXIT_FAILURE);
        return;
      }
      live_controls->setProperty("pinned", true);
      if (!live_controls->property("expanded").toBool()) {
        QCoreApplication::exit(EXIT_FAILURE);
        return;
      }
      QObject* setup_wizard =
          root_object->findChild<QObject*>(QStringLiteral("setupWizard"));
      if (setup_wizard == nullptr ||
          !QMetaObject::invokeMethod(setup_wizard, "goForward",
                                     Qt::DirectConnection) ||
          setup_wizard->property("step").toInt() != 1 ||
          !QMetaObject::invokeMethod(setup_wizard, "goForward",
                                     Qt::DirectConnection) ||
          setup_wizard->property("step").toInt() != 4 ||
          !QMetaObject::invokeMethod(setup_wizard, "goBack",
                                     Qt::DirectConnection) ||
          setup_wizard->property("step").toInt() != 1 ||
          !QMetaObject::invokeMethod(setup_wizard, "goBack",
                                     Qt::DirectConnection) ||
          setup_wizard->property("step").toInt() != 0) {
        QCoreApplication::exit(EXIT_FAILURE);
        return;
      }
      QVector<float> bins(256, -112.0F);
      for (qsizetype i = 92; i < 104; ++i) {
        bins[i] = -42.0F + static_cast<float>(std::abs(i - 98)) * -3.0F;
      }
      cwassistant::desktop::SpectrumFrame frame{
          .bins_dbfs = std::move(bins),
          .sequence = 1,
          .timestamp_ns = 1,
          .lower_frequency_hz = 0.0,
          .upper_frequency_hz = 24'000.0,
      };
      if (!QMetaObject::invokeMethod(
              display, "acceptFrame", Qt::DirectConnection,
              Q_ARG(cwassistant::desktop::SpectrumFrame, frame))) {
        QCoreApplication::exit(EXIT_FAILURE);
      }
    });
    QTimer::singleShot(1'500, &application, &QCoreApplication::quit);
  }
  return application.exec();
}
