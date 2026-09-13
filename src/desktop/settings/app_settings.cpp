#include "app_settings.hpp"

#include <QAudioDevice>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QMediaDevices>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QSettings>
#include <QThreadPool>
#include <QtGlobal>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
// Windows.h defines the COM declarations consumed by OleAuto.h. Keep this
// order explicit: including OleAuto.h first breaks current MSVC Windows SDKs.
// clang-format off
#include <Windows.h>
#include <OleAuto.h>
// clang-format on
#endif

#include "../diagnostics/diagnostics_server.hpp"
#include "../dxcluster/dx_cluster_servers.hpp"
#include "../radio/cat4om_client.hpp"
#include "../radio/hamlib_rigctld_client.hpp"
#include "../sdr/sdr_receiver.hpp"
#include "../transmit/direct_keying_acceptance_probe.hpp"
#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/frequency_plan.hpp"
#include "cwassistant/core/reference_rig_profiles.hpp"

namespace cwassistant::desktop {
namespace {

constexpr auto kSchemaVersion = 1;

// Whether this build carries the SoapySDR reception backend at all. Runtime
// availability is what discovery answers; this is the cheaper question asked
// first, so a build that could never receive from an SDR does not start an
// enumeration whose only possible finding is that same absence.
#if defined(CWA_HAVE_SOAPY_SDR)
constexpr bool kSdrBackendCompiledIn = CWA_HAVE_SOAPY_SDR != 0;
#else
constexpr bool kSdrBackendCompiledIn = false;
#endif

// A hostname is at most 253 characters, and nothing longer can resolve. The
// bound exists so a settings file cannot hand the socket layer an unbounded
// string, not to judge whether the name is reachable.
constexpr int kMaximumDxClusterHostLength = 253;

// The index a stored selection falls back to when the server list no longer
// has the entry it named -- the first offered server, never the custom entry,
// because a custom entry with no host contacts nobody and would look broken.
constexpr int kDxClusterCustomServerIndex = -1;

// The diagnostics service's own default, named once here so the settings layer
// and the server cannot drift apart on it.
constexpr int kDefaultDiagnosticsServerPort =
    static_cast<int>(DiagnosticsServer::kDefaultPort);
static_assert(kDefaultDiagnosticsServerPort == 17300,
              "AppSettings carries this value as an inline member default in "
              "app_settings.hpp; update the header to match.");

// Privileged ports are refused rather than quietly accepted. Binding one needs
// this process to have been started with privileges it has no other reason to
// hold, and a diagnostics stream is not a reason to acquire them.
constexpr int kMinimumDiagnosticsServerPort = 1024;

// Generous enough for anything an operator or a password manager will produce,
// bounded so that a settings file cannot hand the token comparison an
// unbounded string.
constexpr int kMaximumDiagnosticsTokenLength = 128;

// An IPv6 literal with a scope identifier is the longest address that can
// legitimately appear here. Both bounds exist so a settings file cannot grow
// the bind list without limit, not to judge whether an address is reachable:
// the server reports an address it cannot bind and brings the others up
// regardless, because a typo in one interface should not take the service
// down.
constexpr int kMaximumDiagnosticsAddressLength = 128;
constexpr int kMaximumDiagnosticsAddresses = 16;

// An allowed-peer entry is an address with a prefix after it, so it needs the
// address bound above plus room for `/128`. The same count as the bind list,
// because both are lists an operator types by hand and neither has any reason
// to grow without limit from a settings file.
constexpr int kMaximumDiagnosticsPeerLength =
    kMaximumDiagnosticsAddressLength + 4;
constexpr int kMaximumDiagnosticsPeers = kMaximumDiagnosticsAddresses;

// Deliberately not the whole alphabet. 0/O and 1/l/I are one transcription
// error apart, and this token gets read off one screen and typed into another.
constexpr char kDiagnosticsTokenAlphabet[] =
    "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int kGeneratedDiagnosticsTokenLength = 24;
static_assert(kGeneratedDiagnosticsTokenLength >=
                  DiagnosticsServer::kMinimumTokenLength,
              "A generated token must satisfy the server's own minimum length.");

// The first chosen address that something other than this machine could reach,
// or an empty string when every chosen address is loopback. Returned rather
// than a bare bool so the refusal can name the address that made a token
// necessary.
//
// An address that does not parse counts as reachable. A typed address whose
// form is not understood here is not evidence that it is harmless, and the
// safe reading of an unknown is the one that asks for a token.
QString first_routable_diagnostics_address(const QStringList& addresses) {
  for (const QString& text : addresses) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) continue;
    QHostAddress address;
    if (!address.setAddress(trimmed)) return trimmed;
    if (!address.isLoopback()) return trimmed;
  }
  return {};
}

QStringList sanitize_diagnostics_addresses(const QStringList& addresses) {
  QStringList sanitized;
  for (const QString& text : addresses) {
    const QString trimmed =
        text.trimmed().left(kMaximumDiagnosticsAddressLength);
    if (trimmed.isEmpty()) continue;
    // Binding the same address twice would cost a second listening socket and
    // report the second as unbindable, which reads as a fault rather than as a
    // duplicate.
    if (sanitized.contains(trimmed, Qt::CaseInsensitive)) continue;
    sanitized.append(trimmed);
    if (sanitized.size() >= kMaximumDiagnosticsAddresses) break;
  }
  return sanitized;
}

// The same shaping for the allowed-peer list, and deliberately no judgement of
// what an entry means. Blank rows are dropped because the editor creates one
// the moment "+ Add" is pressed and an uncommitted row is not a rule;
// duplicates are dropped because a second copy of a rule permits nothing the
// first did not.
//
// Nothing here decides whether an entry is a readable rule. That belongs to
// DiagnosticsServer::peerIsAllowed, where an unreadable entry admits nobody --
// so an entry kept here that turns out to be a typo narrows access rather than
// widening it, and keeping it is what stops the list going empty and falling
// back to the loopback default the operator did not ask for. The editor marks
// such a row so it is not discovered as a service that refuses everyone.
QStringList sanitize_diagnostics_peers(const QStringList& patterns) {
  QStringList sanitized;
  for (const QString& text : patterns) {
    const QString trimmed = text.trimmed().left(kMaximumDiagnosticsPeerLength);
    if (trimmed.isEmpty()) continue;
    if (sanitized.contains(trimmed, Qt::CaseInsensitive)) continue;
    sanitized.append(trimmed);
    if (sanitized.size() >= kMaximumDiagnosticsPeers) break;
  }
  return sanitized;
}

QString acceptancePlatformToken() {
#ifdef Q_OS_WIN
  return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
  return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
  return QStringLiteral("linux");
#else
  return QStringLiteral("unknown");
#endif
}

#ifdef Q_OS_WIN
constexpr long kOmniRigOnlineStatus = 4;
constexpr long kOmniRigReceiveState = 0x00200000;
constexpr long kOmniRigVfoAa = 0x00000080;
constexpr long kOmniRigVfoAb = 0x00000100;
constexpr long kOmniRigVfoBa = 0x00000200;
constexpr long kOmniRigVfoBb = 0x00000400;
constexpr long kOmniRigVfoA = 0x00000800;
constexpr long kOmniRigVfoB = 0x00001000;
constexpr long kOmniRigSplitOn = 0x00008000;
constexpr long kOmniRigSplitOff = 0x00010000;
constexpr long kOmniRigCwUpper = 0x00800000;
constexpr long kOmniRigCwLower = 0x01000000;
constexpr long kOmniRigSsbUpper = 0x02000000;
constexpr long kOmniRigSsbLower = 0x04000000;
constexpr long kOmniRigDigitalUpper = 0x08000000;
constexpr long kOmniRigDigitalLower = 0x10000000;
constexpr long kOmniRigAm = 0x20000000;
constexpr long kOmniRigFm = 0x40000000;

bool automation_property(IDispatch* object, const wchar_t* name,
                         VARIANT* value) {
  OLECHAR* property_name = const_cast<OLECHAR*>(name);
  DISPID property_id{};
  if (FAILED(object->GetIDsOfNames(IID_NULL, &property_name, 1,
                                   LOCALE_USER_DEFAULT, &property_id))) {
    return false;
  }
  DISPPARAMS parameters{nullptr, nullptr, 0, 0};
  VariantInit(value);
  return SUCCEEDED(object->Invoke(property_id, IID_NULL, LOCALE_USER_DEFAULT,
                                  DISPATCH_PROPERTYGET, &parameters, value,
                                  nullptr, nullptr));
}

std::optional<std::uint64_t> automation_frequency(const VARIANT& value) {
  switch (value.vt) {
    case VT_I4:
    case VT_INT:
      return value.lVal > 0 ? std::optional<std::uint64_t>(value.lVal)
                            : std::nullopt;
    case VT_UI4:
    case VT_UINT:
      return value.ulVal > 0 ? std::optional<std::uint64_t>(value.ulVal)
                             : std::nullopt;
    case VT_I8:
      return value.llVal > 0 ? std::optional<std::uint64_t>(value.llVal)
                             : std::nullopt;
    case VT_UI8:
      return value.ullVal > 0 ? std::optional<std::uint64_t>(value.ullVal)
                              : std::nullopt;
    case VT_R8:
      return value.dblVal > 0.0 && std::isfinite(value.dblVal)
                 ? std::optional<std::uint64_t>(
                       static_cast<std::uint64_t>(std::llround(value.dblVal)))
                 : std::nullopt;
    default: return std::nullopt;
  }
}

std::optional<long> automation_integer(const VARIANT& value) {
  switch (value.vt) {
    case VT_I4:
    case VT_INT: return value.lVal;
    case VT_UI4:
    case VT_UINT:
      return value.ulVal <= static_cast<unsigned long>(
                                std::numeric_limits<LONG>::max())
                 ? std::optional<long>(static_cast<long>(value.ulVal))
                 : std::nullopt;
    default: return std::nullopt;
  }
}

cwassistant::core::OmniRigRxFrequencyTarget omni_rig_rx_write_target(
    IDispatch* rig) {
  VARIANT status_value;
  VARIANT writable_value;
  VARIANT vfo_value;
  VARIANT tx_value;
  const bool has_status = automation_property(rig, L"Status", &status_value);
  const bool has_writable =
      automation_property(rig, L"WriteableParams", &writable_value);
  const bool has_vfo = automation_property(rig, L"Vfo", &vfo_value);
  const bool has_tx = automation_property(rig, L"Tx", &tx_value);
  const auto status =
      has_status ? automation_integer(status_value) : std::nullopt;
  const auto writable =
      has_writable ? automation_integer(writable_value) : std::nullopt;
  const auto vfo = has_vfo ? automation_integer(vfo_value) : std::nullopt;
  const auto tx = has_tx ? automation_integer(tx_value) : std::nullopt;
  if (has_status) VariantClear(&status_value);
  if (has_writable) VariantClear(&writable_value);
  if (has_vfo) VariantClear(&vfo_value);
  if (has_tx) VariantClear(&tx_value);

  return cwassistant::core::select_omnirig_rx_frequency_target(
      status && *status == kOmniRigOnlineStatus,
      tx && *tx == kOmniRigReceiveState,
      writable ? static_cast<std::uint32_t>(*writable) : 0U,
      vfo ? static_cast<std::uint32_t>(*vfo) : 0U);
}

bool automation_put_frequency(IDispatch* object, const wchar_t* name,
                              const std::uint64_t frequency_hz) {
  if (frequency_hz == 0 ||
      frequency_hz >
          static_cast<std::uint64_t>(std::numeric_limits<LONG>::max())) {
    return false;
  }
  OLECHAR* property_name = const_cast<OLECHAR*>(name);
  DISPID property_id{};
  if (FAILED(object->GetIDsOfNames(IID_NULL, &property_name, 1,
                                   LOCALE_USER_DEFAULT, &property_id))) {
    return false;
  }
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_I4;
  value.lVal = static_cast<LONG>(frequency_hz);
  DISPID named_argument = DISPID_PROPERTYPUT;
  DISPPARAMS parameters{&value, &named_argument, 1, 1};
  return SUCCEEDED(object->Invoke(property_id, IID_NULL, LOCALE_USER_DEFAULT,
                                  DISPATCH_PROPERTYPUT, &parameters, nullptr,
                                  nullptr, nullptr));
}

bool automation_put_integer(IDispatch* object, const wchar_t* name,
                            const long integer) {
  OLECHAR* property_name = const_cast<OLECHAR*>(name);
  DISPID property_id{};
  if (FAILED(object->GetIDsOfNames(IID_NULL, &property_name, 1,
                                   LOCALE_USER_DEFAULT, &property_id))) {
    return false;
  }
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_I4;
  value.lVal = integer;
  DISPID named_argument = DISPID_PROPERTYPUT;
  DISPPARAMS parameters{&value, &named_argument, 1, 1};
  return SUCCEEDED(object->Invoke(property_id, IID_NULL, LOCALE_USER_DEFAULT,
                                  DISPATCH_PROPERTYPUT, &parameters, nullptr,
                                  nullptr, nullptr));
}

cwassistant::core::RadioMode omni_rig_mode(const long mode) noexcept {
  using cwassistant::core::RadioMode;
  switch (mode) {
    case kOmniRigCwUpper: return RadioMode::Cw;
    case kOmniRigCwLower: return RadioMode::CwReverse;
    case kOmniRigSsbUpper: return RadioMode::UpperSideband;
    case kOmniRigSsbLower: return RadioMode::LowerSideband;
    case kOmniRigDigitalUpper: return RadioMode::DigitalUpper;
    case kOmniRigDigitalLower: return RadioMode::DigitalLower;
    case kOmniRigAm: return RadioMode::Am;
    case kOmniRigFm: return RadioMode::Fm;
    default: return RadioMode::Unknown;
  }
}

std::optional<long> omni_rig_mode_value(
    const cwassistant::core::RadioMode mode) noexcept {
  using cwassistant::core::RadioMode;
  switch (mode) {
    case RadioMode::Cw: return kOmniRigCwUpper;
    case RadioMode::CwReverse: return kOmniRigCwLower;
    case RadioMode::UpperSideband: return kOmniRigSsbUpper;
    case RadioMode::LowerSideband: return kOmniRigSsbLower;
    case RadioMode::DigitalUpper: return kOmniRigDigitalUpper;
    case RadioMode::DigitalLower: return kOmniRigDigitalLower;
    case RadioMode::Am: return kOmniRigAm;
    case RadioMode::Fm: return kOmniRigFm;
    default: return std::nullopt;
  }
}

const wchar_t* omni_rig_frequency_property(const long vfo,
                                           const bool tx) noexcept {
  const bool uses_a =
      tx ? vfo == kOmniRigVfoAa || vfo == kOmniRigVfoBa || vfo == kOmniRigVfoA
         : vfo == kOmniRigVfoAa || vfo == kOmniRigVfoAb || vfo == kOmniRigVfoA;
  const bool uses_b =
      tx ? vfo == kOmniRigVfoAb || vfo == kOmniRigVfoBb || vfo == kOmniRigVfoB
         : vfo == kOmniRigVfoBa || vfo == kOmniRigVfoBb || vfo == kOmniRigVfoB;
  return uses_a ? L"FreqA" : uses_b ? L"FreqB" : nullptr;
}

QString omni_rig_vfo_label(const wchar_t* property) {
  if (property == nullptr) return {};
  return property[4] == L'A' ? QStringLiteral("A") : QStringLiteral("B");
}

const wchar_t* omni_rig_other_frequency_property(
    const wchar_t* receive_property) noexcept {
  if (receive_property == nullptr) return nullptr;
  return receive_property[4] == L'A' ? L"FreqB" : L"FreqA";
}
#endif

template <typename T>
bool assign_if_changed(T& destination, const T& value) {
  if (destination == value) {
    return false;
  }
  destination = value;
  return true;
}

}  // namespace

AppSettings::AppSettings(QString profile_name, const bool profile_was_explicit,
                         QObject* parent)
    : QObject(parent), profile_name_(profile_name.trimmed()) {
  if (profile_name_.isEmpty()) {
    profile_name_ = QStringLiteral("default");
  }
  profile_storage_key_ = normalizeProfileKey(profile_name_);
  if (profile_storage_key_.isEmpty()) {
    profile_storage_key_ = QStringLiteral("default");
  }
  applyReferenceDefaults(0);
  load();
  refreshProfiles();
  profile_selection_required_ =
      !profile_was_explicit && available_profiles_.size() > 1;
  refreshSerialPorts();
  media_devices_ = std::make_unique<QMediaDevices>();
  connect(media_devices_.get(), &QMediaDevices::audioInputsChanged, this,
          &AppSettings::refreshAudioInputs);
  connect(media_devices_.get(), &QMediaDevices::audioOutputsChanged, this,
          &AppSettings::refreshAudioOutputs);
  refreshAudioInputs();
  refreshAudioOutputs();
  cat4om_client_ = std::make_unique<Cat4OmClient>(this);
  connect(cat4om_client_.get(), &Cat4OmClient::statusChanged, this, [this] {
    setStatusMessage(cat4om_client_->statusText());
    emit cat4omChanged();
    emit radioFrequencyChanged();
    emit radioFrequencyControlChanged();
  });
  connect(cat4om_client_.get(), &Cat4OmClient::radioStateChanged, this, [this] {
    reconcilePendingRxFrequency();
    emit cat4omChanged();
    emit radioFrequencyChanged();
    emit radioFrequencyControlChanged();
  });
  hamlib_client_ = std::make_unique<HamlibRigctldClient>(this);
  connect(hamlib_client_.get(), &HamlibRigctldClient::statusChanged, this,
          [this] {
            setStatusMessage(hamlib_client_->statusText());
            emit hamlibChanged();
          });
  connect(hamlib_client_.get(), &HamlibRigctldClient::radioStateChanged, this,
          [this] {
            refreshControlledFrequency();
            emit hamlibChanged();
          });
  radio_frequency_timer_.setInterval(200);
  connect(&radio_frequency_timer_, &QTimer::timeout, this,
          &AppSettings::refreshControlledFrequency);
  connect(this, &AppSettings::settingsChanged, this,
          &AppSettings::refreshControlledFrequency);
  connect(this, &AppSettings::settingsChanged, this,
          &AppSettings::radioFrequencyControlChanged);
  radio_frequency_timer_.start();
  radio_frequency_request_timer_.setSingleShot(true);
  radio_frequency_request_timer_.setInterval(2'000);
  connect(&radio_frequency_request_timer_, &QTimer::timeout, this, [this] {
    pending_rx_rf_hz_.reset();
    pending_frequency_backend_index_ = -1;
  });
  refreshControlledFrequency();
}

AppSettings::~AppSettings() {
#ifdef Q_OS_WIN
  if (omnirig_automation_ != nullptr) {
    static_cast<IDispatch*>(omnirig_automation_)->Release();
  }
  if (com_initialized_) {
    CoUninitialize();
  }
#endif
}

QStringList AppSettings::referenceRigNames() const {
  QStringList names;
  for (const auto& profile : cwassistant::core::reference_rig_profiles()) {
    names.push_back(QString::fromStdString(profile.display_name));
  }
  return names;
}

const QString& AppSettings::profileName() const noexcept {
  return profile_name_;
}
const QStringList& AppSettings::availableProfiles() const noexcept {
  return available_profiles_;
}
bool AppSettings::profileSelectionRequired() const noexcept {
  return profile_selection_required_;
}
bool AppSettings::setupComplete() const noexcept { return setup_complete_; }

const QStringList& AppSettings::serialPorts() const noexcept {
  return serial_ports_;
}

const QStringList& AppSettings::audioInputNames() const noexcept {
  return audio_input_names_;
}

int AppSettings::audioInputIndex() const noexcept {
  return audio_input_ids_.indexOf(audio_input_id_);
}

QString AppSettings::audioInputDisplayName() const {
  return audio_input_id_.isEmpty() ? QStringLiteral("System default input")
                                   : audio_input_name_;
}

bool AppSettings::audioInputNamesAmbiguous() const noexcept {
  return audio_input_names_ambiguous_;
}

const QString& AppSettings::audioInputId() const noexcept {
  return audio_input_id_;
}

const QString& AppSettings::audioInputDeviceName() const noexcept {
  return audio_input_device_name_;
}
const QStringList& AppSettings::audioOutputNames() const noexcept {
  return audio_output_names_;
}
int AppSettings::audioOutputIndex() const noexcept {
  return static_cast<int>(
      std::max<qsizetype>(0, audio_output_ids_.indexOf(audio_output_id_)));
}
QString AppSettings::audioOutputDisplayName() const {
  const int index = audioOutputIndex();
  return index > 0 && index < audio_output_names_.size()
             ? audio_output_names_.at(index)
             : QStringLiteral("System default output");
}
const QString& AppSettings::audioOutputId() const noexcept {
  return audio_output_id_;
}
QStringList AppSettings::receiverInputTypeNames() const {
  return {QStringLiteral("Sound-card audio"),
          QStringLiteral("SDR device — wide passband")};
}
int AppSettings::receiverInputTypeIndex() const noexcept {
  return receiver_input_type_index_;
}
int AppSettings::preferredSourceMode() const noexcept {
  return preferred_source_mode_;
}
bool AppSettings::sdrBackendAvailable() const noexcept {
  return sdr_backend_available_;
}
const QString& AppSettings::sdrBackendVersion() const noexcept {
  return sdr_backend_version_;
}
const QStringList& AppSettings::sdrModuleNames() const noexcept {
  return sdr_module_names_;
}
const QStringList& AppSettings::sdrDeviceNames() const noexcept {
  return sdr_device_names_;
}
int AppSettings::sdrDeviceIndex() const noexcept {
  return sdr_device_ids_.indexOf(sdr_physical_device_id_);
}
const QString& AppSettings::sdrDeviceId() const noexcept {
  return sdr_device_id_;
}
QString AppSettings::sdrDeviceDisplayName() const {
  const int index = sdrDeviceIndex();
  return index >= 0 && index < sdr_device_names_.size()
             ? sdr_device_names_.at(index)
             : (sdr_device_name_.isEmpty() ? QStringLiteral("No SDR selected")
                                           : sdr_device_name_);
}
const QStringList& AppSettings::sdrOperatingModeNames() const noexcept {
  return sdr_device_mode_names_;
}
int AppSettings::sdrOperatingModeIndex() const noexcept {
  return sdr_device_mode_ids_.indexOf(sdr_device_id_);
}
const QString& AppSettings::sdrDiagnostic() const noexcept {
  return sdr_diagnostic_;
}
qulonglong AppSettings::sdrCenterFrequencyHz() const noexcept {
  return sdr_center_frequency_hz_;
}
int AppSettings::sdrSampleRateHz() const noexcept {
  return sdr_sample_rate_hz_;
}
int AppSettings::sdrBandwidthHz() const noexcept { return sdr_bandwidth_hz_; }
const QVariantList& AppSettings::sdrSampleRateOptions() const noexcept {
  return sdr_sample_rate_options_;
}
const QVariantList& AppSettings::sdrBandwidthOptions() const noexcept {
  return sdr_bandwidth_options_;
}
const QStringList& AppSettings::sdrAntennaNames() const noexcept {
  return sdr_antenna_names_;
}
int AppSettings::sdrAntennaIndex() const noexcept {
  return sdr_antenna_names_.indexOf(sdr_antenna_);
}
const QString& AppSettings::sdrAntenna() const noexcept { return sdr_antenna_; }
qulonglong AppSettings::sdrDecoderCenterFrequencyHz() const noexcept {
  return sdr_decoder_center_frequency_hz_;
}
int AppSettings::sdrDecoderBandwidthHz() const noexcept {
  return sdr_decoder_bandwidth_hz_;
}
bool AppSettings::sdrFollowRadioVfo() const noexcept {
  return sdr_follow_radio_vfo_;
}
int AppSettings::sdrTuningStepHz() const noexcept { return sdr_tuning_step_hz_; }
QString AppSettings::sdrRadioSyncStatus() const {
  if (!sdr_follow_radio_vfo_)
    return QStringLiteral("Radio Sync is off; SDR tuning is independent.");
  if (!observedRadioRxRfHz())
    return QStringLiteral(
        "Radio Sync is waiting for authoritative CAT RX readback.");
  const bool writable = cwassistant::core::radio_has_capability(
      radio_state_.capabilities,
      cwassistant::core::RadioCapability::SetRxFrequency);
  return writable
             ? QStringLiteral(
                   "Radio Sync is bidirectional; CAT readback remains authoritative.")
             : QStringLiteral(
                   "SDR follows CAT; this provider does not allow RX tuning.");
}
qint64 AppSettings::sdrRadioLoOffsetHz() const noexcept {
  return sdr_radio_lo_offset_hz_;
}
bool AppSettings::sdrAutomaticGain() const noexcept {
  return sdr_automatic_gain_;
}
bool AppSettings::sdrAutomaticGainAvailable() const noexcept {
  return sdr_automatic_gain_available_;
}
double AppSettings::sdrGainDb() const noexcept { return sdr_gain_db_; }
double AppSettings::sdrMinimumGainDb() const noexcept {
  return sdr_minimum_gain_db_;
}
double AppSettings::sdrMaximumGainDb() const noexcept {
  return sdr_maximum_gain_db_;
}
QString AppSettings::sdrWidePassbandSummary() const {
  const double bandwidth_mhz = static_cast<double>(sdr_sample_rate_hz_) / 1e6;
  const double lower_mhz = (static_cast<double>(sdr_center_frequency_hz_) -
                            static_cast<double>(sdr_sample_rate_hz_) * 0.5) /
                           1e6;
  const double upper_mhz = (static_cast<double>(sdr_center_frequency_hz_) +
                            static_cast<double>(sdr_sample_rate_hz_) * 0.5) /
                           1e6;
  return QStringLiteral("%1 MHz visible passband (%2–%3 MHz RF)")
      .arg(bandwidth_mhz, 0, 'f', 3)
      .arg(lower_mhz, 0, 'f', 6)
      .arg(upper_mhz, 0, 'f', 6);
}
bool AppSettings::audioDcRejection() const noexcept {
  return audio_dc_rejection_;
}
bool AppSettings::audioAutomaticGain() const noexcept {
  return audio_automatic_gain_;
}
double AppSettings::audioGainDb() const noexcept { return audio_gain_db_; }
double AppSettings::audioAutomaticGainTargetDbfs() const noexcept {
  return audio_automatic_gain_target_dbfs_;
}
bool AppSettings::audioAutomaticBandwidth() const noexcept {
  return audio_automatic_bandwidth_;
}
double AppSettings::audioLowerFrequencyHz() const noexcept {
  return audio_lower_frequency_hz_;
}
double AppSettings::audioUpperFrequencyHz() const noexcept {
  return audio_upper_frequency_hz_;
}
bool AppSettings::audioInputRadioLinked() const noexcept {
  return audio_input_radio_linked_;
}

const QString& AppSettings::ownCallsign() const noexcept {
  return own_callsign_;
}

bool AppSettings::omniRigAvailable() const noexcept {
#ifdef Q_OS_WIN
  CLSID class_id{};
  return SUCCEEDED(CLSIDFromProgID(L"OmniRig.OmniRigX", &class_id));
#else
  return false;
#endif
}

bool AppSettings::radioEnabled() const noexcept { return radio_enabled_; }

QString AppSettings::radioDisplayName() const {
  if (!radio_enabled_) {
    return QStringLiteral("No radio — receive-only (SWL)");
  }
  const auto detected_index = detected_radio_slots_.indexOf(omnirig_slot_);
  if (frequency_backend_index_ == 0 && detected_index >= 0) {
    return detected_radio_names_.at(detected_index);
  }
  const auto names = referenceRigNames();
  return names.value(reference_rig_index_,
                     QStringLiteral("Manually configured radio"));
}

const QStringList& AppSettings::detectedRadioNames() const noexcept {
  return detected_radio_names_;
}

int AppSettings::detectedRadioIndex() const noexcept {
  return detected_radio_slots_.indexOf(omnirig_slot_);
}

int AppSettings::referenceRigIndex() const noexcept {
  return reference_rig_index_;
}
int AppSettings::frequencyBackendIndex() const noexcept {
  return frequency_backend_index_;
}
int AppSettings::radioTuningStepHz() const noexcept {
  return radio_tuning_step_hz_;
}
bool AppSettings::radioFrequencyWritable() const noexcept {
  return controlledRxRfHz().has_value() &&
         cwassistant::core::radio_has_capability(
             radio_state_.capabilities,
             cwassistant::core::RadioCapability::SetRxFrequency);
}
bool AppSettings::radioTxFrequencyWritable() const noexcept {
  return cwassistant::core::radio_has_capability(
      radio_state_.capabilities,
      cwassistant::core::RadioCapability::SetTxFrequency);
}
bool AppSettings::radioTxFrequencySyncAvailable() const noexcept {
  return controlledRxRfHz().has_value() &&
         cwassistant::core::radio_tx_frequency_sync_is_available(radio_state_);
}
void AppSettings::reportPointedTxUnavailable() {
  setStatusMessage(
      radio_state_.availability != cwassistant::core::RadioObservation::Known
          ? QStringLiteral(
                "No radio is reporting its state, so a TX frequency cannot be "
                "pointed.")
          : QStringLiteral(
                "This radio does not offer split or TX-frequency control, so "
                "TX cannot be pointed from the spectrum."));
}

bool AppSettings::radioPointedTxFrequencyAvailable() const noexcept {
  return cwassistant::core::radio_pointed_tx_frequency_is_available(
      radio_state_);
}
bool AppSettings::radioRxModeWritable() const noexcept {
  return cwassistant::core::radio_has_capability(
      radio_state_.capabilities, cwassistant::core::RadioCapability::SetRxMode);
}
bool AppSettings::radioTxModeWritable() const noexcept {
  return cwassistant::core::radio_has_capability(
      radio_state_.capabilities, cwassistant::core::RadioCapability::SetTxMode);
}
bool AppSettings::radioSplitWritable() const noexcept {
  return cwassistant::core::radio_has_capability(
      radio_state_.capabilities, cwassistant::core::RadioCapability::SetSplit);
}
QString AppSettings::radioRxMode() const {
  const auto token =
      cwassistant::core::radio_mode_token(radio_state_.rx_mode.mode);
  return QString::fromLatin1(token.data(),
                             static_cast<qsizetype>(token.size()));
}
QString AppSettings::radioTxMode() const {
  const auto token =
      cwassistant::core::radio_mode_token(radio_state_.tx_mode.mode);
  return QString::fromLatin1(token.data(),
                             static_cast<qsizetype>(token.size()));
}
QString AppSettings::radioTxModeTarget() const {
  const auto token = cwassistant::core::radio_mode_token(radio_tx_mode_target_);
  return QString::fromLatin1(token.data(),
                             static_cast<qsizetype>(token.size()));
}
bool AppSettings::radioTxModeConfirmed() const noexcept {
  return cwassistant::core::radio_mode_target_is_confirmed(
      radio_state_.tx_mode, radio_tx_mode_target_);
}
QString AppSettings::radioRxVfo() const {
  return radio_state_.rx_vfo.observation ==
                 cwassistant::core::RadioObservation::Known
             ? QString::fromStdString(radio_state_.rx_vfo.identifier)
             : QStringLiteral("?");
}
QString AppSettings::radioTxVfo() const {
  return radio_state_.tx_vfo.observation ==
                 cwassistant::core::RadioObservation::Known
             ? QString::fromStdString(radio_state_.tx_vfo.identifier)
             : QStringLiteral("?");
}
qulonglong AppSettings::radioTxVfoFrequencyHz() const noexcept {
  if (radio_state_.tx_frequency.observation !=
      cwassistant::core::RadioObservation::Known) {
    return 0U;
  }
  const auto resolved = cwassistant::core::resolve_frequencies(
      {.rx_dial_hz = radio_state_.tx_frequency.hz,
       .tx_dial_hz = radio_state_.tx_frequency.hz,
       .split_enabled = true},
      {.rx_offset_hz = tx_transverter_offset_hz_,
       .tx_offset_hz = tx_transverter_offset_hz_});
  return resolved ? static_cast<qulonglong>(resolved->tx_rf_hz) : 0U;
}
bool AppSettings::radioSplitKnown() const noexcept {
  return radio_state_.split.observation ==
         cwassistant::core::RadioObservation::Known;
}
int AppSettings::omniRigSlot() const noexcept { return omnirig_slot_; }
const QString& AppSettings::hamlibHost() const noexcept { return hamlib_host_; }
int AppSettings::hamlibPort() const noexcept { return hamlib_port_; }
const QString& AppSettings::hamlibRxVfo() const noexcept {
  return hamlib_rx_vfo_;
}
const QString& AppSettings::hamlibTxVfo() const noexcept {
  return hamlib_tx_vfo_;
}
bool AppSettings::hamlibWritable() const noexcept { return hamlib_writable_; }
QString AppSettings::hamlibState() const {
  return hamlib_client_ ? hamlib_client_->statusText()
                        : QStringLiteral("Unavailable");
}
bool AppSettings::hamlibCanWrite() const noexcept {
  return hamlib_client_ && hamlib_client_->canWrite();
}
const QString& AppSettings::cat4omUrl() const noexcept { return cat4om_url_; }
const QString& AppSettings::cat4omRadioId() const noexcept {
  return cat4om_radio_id_;
}
const QString& AppSettings::cat4omPassword() const noexcept {
  return cat4om_password_;
}
QString AppSettings::cat4omState() const {
  return cat4om_client_ ? cat4om_client_->statusText()
                        : QStringLiteral("Disconnected");
}
QString AppSettings::cat4omFrequencySummary() const {
  if (!cat4om_client_) {
    return QStringLiteral("No radio state");
  }
  const auto plan = cat4om_client_->frequencyPlan();
  if (!plan) {
    return QStringLiteral("No radio state");
  }
  return plan->split_enabled
             ? QStringLiteral("RX %1 Hz • TX %2 Hz • split")
                   .arg(static_cast<qulonglong>(plan->rx_dial_hz))
                   .arg(static_cast<qulonglong>(plan->tx_dial_hz))
             : QStringLiteral("%1 Hz • simplex")
                   .arg(static_cast<qulonglong>(plan->rx_dial_hz));
}
bool AppSettings::cat4omCanWrite() const noexcept {
  return cat4om_client_ && cat4om_client_->canWrite();
}
const QString& AppSettings::catPort() const noexcept { return cat_port_; }
QVariantList AppSettings::supportedCatBaudRates() const {
  QVariantList values;
  values.reserve(static_cast<qsizetype>(
      cwassistant::core::kSupportedSerialBaudRates.size()));
  for (const auto baud_rate : cwassistant::core::kSupportedSerialBaudRates) {
    values.append(static_cast<int>(baud_rate));
  }
  return values;
}
int AppSettings::catBaudRate() const noexcept { return cat_baud_rate_; }
int AppSettings::catDataBits() const noexcept { return cat_data_bits_; }
int AppSettings::catParityIndex() const noexcept { return cat_parity_index_; }
int AppSettings::catStopBits() const noexcept { return cat_stop_bits_; }
int AppSettings::catFlowControlIndex() const noexcept {
  return cat_flow_control_index_;
}
int AppSettings::pollIntervalMs() const noexcept { return poll_interval_ms_; }
int AppSettings::timeoutMs() const noexcept { return timeout_ms_; }
bool AppSettings::splitEnabled() const noexcept { return split_enabled_; }
qint64 AppSettings::rxTransverterOffsetHz() const noexcept {
  return rx_transverter_offset_hz_;
}
qint64 AppSettings::txTransverterOffsetHz() const noexcept {
  return tx_transverter_offset_hz_;
}
int AppSettings::cwToneSidebandIndex() const noexcept {
  return cw_tone_sideband_index_;
}
std::optional<cwassistant::core::ResolvedFrequencies>
AppSettings::resolvedControlledFrequencies() const noexcept {
  if (!radio_enabled_ || !audio_input_radio_linked_) {
    return std::nullopt;
  }
  if (radio_state_.rx_frequency.observation !=
          cwassistant::core::RadioObservation::Known ||
      radio_state_.tx_frequency.observation !=
          cwassistant::core::RadioObservation::Known ||
      radio_state_.split.observation !=
          cwassistant::core::RadioObservation::Known) {
    return std::nullopt;
  }
  const cwassistant::core::VfoFrequencyPlan plan{
      .rx_dial_hz = radio_state_.rx_frequency.hz,
      .tx_dial_hz = radio_state_.tx_frequency.hz,
      .split_enabled =
          radio_state_.split.split == cwassistant::core::RadioSplit::Enabled};
  return cwassistant::core::resolve_frequencies(
      plan, {.rx_offset_hz = rx_transverter_offset_hz_,
             .tx_offset_hz = tx_transverter_offset_hz_});
}

std::optional<std::uint64_t> AppSettings::controlledRxRfHz() const noexcept {
  const auto resolved = resolvedControlledFrequencies();
  return resolved ? std::optional<std::uint64_t>(resolved->rx_rf_hz)
                  : std::nullopt;
}

std::optional<std::uint64_t> AppSettings::controlledTxRfHz() const noexcept {
  const auto resolved = resolvedControlledFrequencies();
  return resolved ? std::optional<std::uint64_t>(resolved->tx_rf_hz)
                  : std::nullopt;
}

bool AppSettings::controlledSplitActive() const noexcept {
  const auto resolved = resolvedControlledFrequencies();
  return resolved && resolved->split_enabled;
}

void AppSettings::refreshControlledFrequency() {
  std::optional<std::uint64_t> frequency;
  auto write_target = cwassistant::core::OmniRigRxFrequencyTarget::None;
  cwassistant::core::RadioState next_state;
  next_state.availability = cwassistant::core::RadioObservation::Unavailable;
  next_state.rx_frequency.observation =
      cwassistant::core::RadioObservation::Unavailable;
  next_state.tx_frequency.observation =
      cwassistant::core::RadioObservation::Unavailable;
  next_state.rx_mode.observation =
      cwassistant::core::RadioObservation::Unavailable;
  next_state.tx_mode.observation =
      cwassistant::core::RadioObservation::Unavailable;
  next_state.rx_vfo.observation =
      cwassistant::core::RadioObservation::Unavailable;
  next_state.tx_vfo.observation =
      cwassistant::core::RadioObservation::Unavailable;
  next_state.split.observation =
      cwassistant::core::RadioObservation::Unavailable;
  next_state.capabilities.observation =
      cwassistant::core::RadioObservation::Unavailable;
#ifdef Q_OS_WIN
  if (radio_enabled_ && audio_input_radio_linked_ &&
      frequency_backend_index_ == 0 && ensureOmniRigAutomation()) {
    auto* automation = static_cast<IDispatch*>(omnirig_automation_);
    VARIANT rig_value;
    const auto property = omnirig_slot_ == 2 ? L"Rig2" : L"Rig1";
    if (automation_property(automation, property, &rig_value)) {
      IDispatch* rig =
          rig_value.vt == VT_DISPATCH ? rig_value.pdispVal : nullptr;
      if (rig != nullptr) {
        // Frequency readback stays responsive at 5 Hz. Capability/VFO
        // discovery crosses the out-of-process COM boundary four more times,
        // so cache that UI hint for one second. Every actual write performs a
        // fresh complete check and never relies on this cache for safety.
        if (omnirig_capability_refresh_clock_.isValid() &&
            omnirig_capability_refresh_clock_.elapsed() < 1'000) {
          write_target = omnirig_rx_write_target_;
        } else {
          write_target = omni_rig_rx_write_target(rig);
          omnirig_capability_refresh_clock_.restart();
        }
        const auto integer_property = [rig](const wchar_t* name) {
          VARIANT value;
          const bool present = automation_property(rig, name, &value);
          const auto result =
              present ? automation_integer(value) : std::nullopt;
          if (present) VariantClear(&value);
          return result;
        };
        const auto frequency_property = [rig](const wchar_t* name) {
          VARIANT value;
          const bool present = automation_property(rig, name, &value);
          const auto result =
              present ? automation_frequency(value) : std::nullopt;
          if (present) VariantClear(&value);
          return result;
        };
        const auto status = integer_property(L"Status");
        if (status && *status == kOmniRigOnlineStatus) {
          using namespace cwassistant::core;
          next_state = {};
          next_state.availability = RadioObservation::Known;
          next_state.capabilities.observation = RadioObservation::Known;
          const auto writable = integer_property(L"WriteableParams");
          const auto vfo = integer_property(L"Vfo");
          const auto split = integer_property(L"Split");
          const auto mode = integer_property(L"Mode");
          const bool split_on = split && *split == kOmniRigSplitOn;
          const wchar_t* rx_property =
              vfo ? omni_rig_frequency_property(*vfo, false) : nullptr;
          // A radio that publishes no VFO identity was treated as having no
          // transmit VFO at all, so pointing the transmit frequency was
          // refused even where the parameter mask said FreqB was writable.
          // With split on, A receives and B transmits on essentially every
          // transceiver; that convention is claimed only where the mask agrees
          // the property can be written.
          const auto conventional = [&](const bool transmit) {
            return omni_rig_conventional_vfo_target(
                rx_property != nullptr, split_on, transmit,
                writable ? static_cast<std::uint32_t>(*writable) : 0U);
          };
          const auto target_property =
              [](const cwassistant::core::OmniRigRxFrequencyTarget target)
              -> const wchar_t* {
            return target == cwassistant::core::OmniRigRxFrequencyTarget::
                                 FrequencyA
                       ? L"FreqA"
                   : target == cwassistant::core::OmniRigRxFrequencyTarget::
                                   FrequencyB
                       ? L"FreqB"
                       : nullptr;
          };
          // In simplex the effective transmitter is the RX VFO, but the
          // second faceplate row represents the standby VFO the operator will
          // use for split. Read that VFO independently instead of cloning RX.
          const wchar_t* tx_property =
              split_on
                  ? (vfo ? omni_rig_frequency_property(*vfo, true)
                         : target_property(conventional(true)))
                  : omni_rig_other_frequency_property(rx_property);
          // `Freq` is the selected VFO, not the receive VFO. Reading it as
          // RX regardless meant that on a rig publishing no per-VFO property
          // -- the FT-450D among them -- selecting the transmit VFO to set it
          // dragged the receive frequency along with it. It is trustworthy
          // only when there is one VFO in play.
          // Reading order: the VFO the radio named, then the VFO the split
          // convention implies, then the selected VFO -- and that last only
          // where one VFO is in play, because with split on it reports
          // whichever VFO the operator has selected and following it made
          // moving the transmit VFO move the receive frequency.
          const wchar_t* conventional_rx = target_property(conventional(false));
          auto rx = rx_property
                        ? frequency_property(rx_property)
                    : conventional_rx ? frequency_property(conventional_rx)
                        : (omni_rig_active_vfo_is_receive_frequency(
                               rx_property != nullptr, split_on)
                               ? frequency_property(L"Freq")
                               : std::nullopt);
          auto tx =
              tx_property ? frequency_property(tx_property) : std::nullopt;
          if (rx) {
            frequency = rx;
            next_state.rx_frequency = {RadioObservation::Known, *rx};
          } else if (radio_state_.rx_frequency.observation ==
                     RadioObservation::Known) {
            // No trustworthy reading this cycle. Hold what was last known
            // rather than reporting the receive frequency as unknown, which
            // would drop the RF axis, the spot band filter and the decoder's
            // frequency mapping every time the operator touched the other
            // VFO.
            next_state.rx_frequency = radio_state_.rx_frequency;
          }
          if (tx) next_state.tx_frequency = {RadioObservation::Known, *tx};
          const wchar_t* rx_label_property =
              rx_property ? rx_property : conventional_rx;
          if (rx_label_property) {
            next_state.rx_vfo = {
                RadioObservation::Known,
                omni_rig_vfo_label(rx_label_property).toStdString()};
          }
          if (tx_property) {
            next_state.tx_vfo = {RadioObservation::Known,
                                 omni_rig_vfo_label(tx_property).toStdString()};
          }
          if (split &&
              (*split == kOmniRigSplitOn || *split == kOmniRigSplitOff)) {
            next_state.split = {
                RadioObservation::Known,
                split_on ? RadioSplit::Enabled : RadioSplit::Disabled};
          }
          if (mode) {
            const auto mapped = omni_rig_mode(*mode);
            if (mapped != RadioMode::Unknown) {
              next_state.rx_mode = {RadioObservation::Known, mapped};
              const std::size_t slot_index =
                  static_cast<std::size_t>(std::clamp(omnirig_slot_, 1, 2) - 1);
              if (rx_property && rx_property[4] == L'A')
                omnirig_vfo_a_modes_[slot_index] = mapped;
              else if (rx_property && rx_property[4] == L'B')
                omnirig_vfo_b_modes_[slot_index] = mapped;
            }
          }
          if (tx_property != nullptr) {
            const std::size_t slot_index =
                static_cast<std::size_t>(std::clamp(omnirig_slot_, 1, 2) - 1);
            const auto tx_mode = tx_property[4] == L'A'
                                     ? omnirig_vfo_a_modes_[slot_index]
                                     : omnirig_vfo_b_modes_[slot_index];
            if (tx_mode != RadioMode::Unknown)
              next_state.tx_mode = {RadioObservation::Known, tx_mode};
          }
          if (write_target != OmniRigRxFrequencyTarget::None)
            next_state.capabilities.bits |=
                radio_capability_bit(RadioCapability::SetRxFrequency);
          if (writable && tx_property != nullptr) {
            const auto target_bit = tx_property[4] == L'A' ? 0x04L : 0x08L;
            if ((*writable & target_bit) != 0)
              next_state.capabilities.bits |=
                  radio_capability_bit(RadioCapability::SetTxFrequency);
          }
          constexpr long kModeBits =
              kOmniRigCwUpper | kOmniRigCwLower | kOmniRigSsbUpper |
              kOmniRigSsbLower | kOmniRigDigitalUpper | kOmniRigDigitalLower |
              kOmniRigAm | kOmniRigFm;
          if (writable && (*writable & kModeBits) != 0)
            next_state.capabilities.bits |=
                radio_capability_bit(RadioCapability::SetRxMode);
          if (writable && (*writable & kOmniRigSplitOn) != 0 &&
              (*writable & kOmniRigSplitOff) != 0)
            next_state.capabilities.bits |=
                radio_capability_bit(RadioCapability::SetSplit);
        }
      }
      VariantClear(&rig_value);
    }
  } else {
    omnirig_capability_refresh_clock_.invalidate();
  }
#endif
  if (radio_enabled_ && audio_input_radio_linked_ &&
      frequency_backend_index_ == 2 && cat4om_client_) {
    next_state = cat4om_client_->radioState();
  }
  if (radio_enabled_ && audio_input_radio_linked_ &&
      frequency_backend_index_ == 1 && hamlib_client_) {
    next_state = hamlib_client_->radioState();
  }
  if (frequency != omnirig_rx_dial_hz_ ||
      write_target != omnirig_rx_write_target_ || next_state != radio_state_) {
    omnirig_rx_dial_hz_ = frequency;
    omnirig_rx_write_target_ = write_target;
    radio_state_ = std::move(next_state);
    emit radioFrequencyChanged();
    emit radioFrequencyControlChanged();
  }
  reconcilePendingRxFrequency();
}

void AppSettings::reconcilePendingRxFrequency() {
  if (!pending_rx_rf_hz_ ||
      pending_frequency_backend_index_ != frequency_backend_index_) {
    return;
  }
  const auto reported = controlledRxRfHz();
  if (reported && *reported == *pending_rx_rf_hz_) {
    pending_rx_rf_hz_.reset();
    pending_frequency_backend_index_ = -1;
    radio_frequency_request_timer_.stop();
  }
}

void AppSettings::rememberPendingRxFrequency(const std::uint64_t frequency_hz) {
  pending_rx_rf_hz_ = frequency_hz;
  pending_frequency_backend_index_ = frequency_backend_index_;
  radio_frequency_request_timer_.start();
}

void AppSettings::invalidateDirectKeyingAcceptance(QString status) {
  direct_keying_validated_ = false;
  direct_keying_acceptance_sha256_.clear();
  direct_keying_acceptance_platform_.clear();
  direct_keying_acceptance_utc_seconds_ = 0;
  direct_keying_acceptance_status_ = std::move(status);
}

bool AppSettings::setControlledRxFrequency(const QString& value,
                                           const qulonglong unit_hz) {
  if (unit_hz != 1'000 && unit_hz != 1'000'000) {
    setStatusMessage(QStringLiteral("Choose a frequency in kHz or MHz."));
    return false;
  }
  const auto requested_rf = cwassistant::core::parse_frequency_value(
      value.toStdString(), static_cast<std::uint64_t>(unit_hz));
  if (!requested_rf) {
    setStatusMessage(unit_hz == 1'000
                         ? QStringLiteral("Enter a positive frequency in kHz, "
                                          "with at most three decimal places.")
                         : QStringLiteral("Enter a positive frequency in MHz, "
                                          "with at most six decimal places."));
    return false;
  }
  const auto dial_frequency = cwassistant::core::resolve_dial_frequency(
      *requested_rf, rx_transverter_offset_hz_);
  if (!dial_frequency) {
    setStatusMessage(
        QStringLiteral("That actual RF frequency cannot be represented with "
                       "the configured RX transverter offset."));
    return false;
  }
  if (!writeControlledRxDialFrequency(*dial_frequency)) {
    return false;
  }
  rememberPendingRxFrequency(*requested_rf);
  setStatusMessage(QStringLiteral(
      "RX frequency requested; provider readback remains authoritative. Split "
      "TX frequency and mode were not changed."));
  return true;
}

bool AppSettings::stepControlledRxFrequency(const int direction) {
  if (direction != -1 && direction != 1) {
    setStatusMessage(
        QStringLiteral("Frequency step direction must be down or up."));
    return false;
  }
  const auto current_rf = controlledRxRfHz();
  if (!current_rf || !radioFrequencyWritable()) {
    setStatusMessage(
        QStringLiteral("RX frequency control requires a linked, "
                       "online, writable radio provider."));
    return false;
  }
  const auto pending =
      pending_frequency_backend_index_ == frequency_backend_index_
          ? pending_rx_rf_hz_
          : std::nullopt;
  const auto requested_rf = cwassistant::core::step_rx_frequency(
      *current_rf, pending, static_cast<std::uint64_t>(radio_tuning_step_hz_),
      direction);
  if (!requested_rf) {
    setStatusMessage(
        direction < 0
            ? QStringLiteral(
                  "The requested RX step would reach or cross zero hertz.")
            : QStringLiteral("The requested RX step exceeds the supported "
                             "frequency range."));
    return false;
  }
  const auto dial_frequency = cwassistant::core::resolve_dial_frequency(
      *requested_rf, rx_transverter_offset_hz_);
  if (!dial_frequency || !writeControlledRxDialFrequency(*dial_frequency)) {
    if (!dial_frequency) {
      setStatusMessage(
          QStringLiteral("That RX step cannot be represented with "
                         "the configured transverter offset."));
    }
    return false;
  }
  rememberPendingRxFrequency(*requested_rf);
  setStatusMessage(
      QStringLiteral("RX stepped by %1 kHz; provider readback remains "
                     "authoritative and split TX was not changed.")
          .arg(radio_tuning_step_hz_ / 1'000));
  return true;
}

bool AppSettings::writeControlledRxDialFrequency(
    const std::uint64_t dial_frequency_hz) {
  if (!radioFrequencyWritable()) {
    setStatusMessage(
        QStringLiteral("RX frequency control requires a linked, "
                       "online, writable radio provider."));
    return false;
  }
  return writeRadioRxDialFrequency(dial_frequency_hz);
}

bool AppSettings::writeRadioRxDialFrequency(
    const std::uint64_t dial_frequency_hz) {
  if (!radio_enabled_ ||
      cwassistant::core::validate_radio_command(
          radio_state_, cwassistant::core::SetRxFrequency{dial_frequency_hz}) !=
          cwassistant::core::RadioCommandValidation::Valid) {
    return false;
  }
  if (frequency_backend_index_ == 0) {
#ifdef Q_OS_WIN
    if (!writeOmniRigRxFrequency(dial_frequency_hz)) {
      setStatusMessage(QStringLiteral(
          "OmniRig did not accept the RX-frequency request. Confirm that the "
          "radio is online, receiving, and exposes a writable RX VFO."));
      return false;
    }
    return true;
#else
    setStatusMessage(QStringLiteral(
        "OmniRig frequency control is available only on Windows."));
    return false;
#endif
  }
  if (frequency_backend_index_ == 2 && cat4om_client_ &&
      cat4om_client_->setRxFrequency(dial_frequency_hz)) {
    return true;
  }
  if (frequency_backend_index_ == 1 && hamlib_client_ &&
      hamlib_client_->setRxFrequency(dial_frequency_hz)) {
    return true;
  }
  setStatusMessage(QStringLiteral(
      "The radio provider did not accept the RX-frequency request."));
  return false;
}

bool AppSettings::setControlledTxFrequency(const QString& value,
                                           const qulonglong unit_hz) {
  if (unit_hz != 1'000 && unit_hz != 1'000'000) return false;
  const auto requested_rf = cwassistant::core::parse_frequency_value(
      value.toStdString(), static_cast<std::uint64_t>(unit_hz));
  if (!requested_rf) {
    setStatusMessage(QStringLiteral("Enter a valid positive TX frequency."));
    return false;
  }
  return requestControlledTxRfFrequency(*requested_rf);
}

bool AppSettings::setControlledTxFrequencyHz(const qulonglong value) {
  if (value == 0U || value > 99'000'000'000ULL) {
    setStatusMessage(QStringLiteral("Select a valid positive TX frequency."));
    return false;
  }
  return requestControlledTxRfFrequency(static_cast<std::uint64_t>(value));
}

bool AppSettings::requestControlledTxRfFrequency(
    const std::uint64_t rf_frequency_hz) {
  const auto dial = cwassistant::core::resolve_dial_frequency(
      rf_frequency_hz, tx_transverter_offset_hz_);
  if (!dial) {
    setStatusMessage(
        QStringLiteral("That TX frequency cannot be represented "
                       "with the configured transverter offset."));
    return false;
  }
  // A deliberate TX-frequency edit is the operator action that establishes
  // independent VFO operation. Never silently change split merely on connect.
  if (radio_state_.split.split != cwassistant::core::RadioSplit::Enabled &&
      !setControlledSplit(true)) {
    return false;
  }
  if (!writeControlledTxDialFrequency(*dial)) return false;
  setStatusMessage(QStringLiteral(
      "TX frequency requested on the provider's TX VFO; split remains enabled "
      "and provider readback is authoritative."));
  return true;
}

bool AppSettings::cycleControlledRxMode() {
  using cwassistant::core::RadioMode;
  const auto current = radio_state_.rx_mode.mode;
  const auto next = current == RadioMode::UpperSideband
                        ? RadioMode::LowerSideband
                    : current == RadioMode::LowerSideband ? RadioMode::Cw
                    : current == RadioMode::Cw            ? RadioMode::CwReverse
                                               : RadioMode::UpperSideband;
  return writeControlledMode(next, false);
}

bool AppSettings::toggleControlledTxMode() {
  using cwassistant::core::RadioMode;
  const auto next = radio_tx_mode_target_ == RadioMode::Cw
                        ? RadioMode::CwReverse
                        : RadioMode::Cw;
  radio_tx_mode_target_ = next;
  QSettings settings;
  settings.setValue(storageKey(QStringLiteral("radio/txModeTarget")),
                    radioTxModeTarget());
  emit radioFrequencyChanged();

  if (!radioTxModeWritable()) {
    setStatusMessage(
        QStringLiteral("TX target set to %1. The selected provider cannot "
                       "apply or confirm the TX-VFO mode.")
            .arg(radioTxModeTarget()));
    return true;
  }
  if (!writeControlledMode(next, true)) {
    setStatusMessage(QStringLiteral("TX target remains %1, but the provider "
                                    "did not accept the mode request.")
                         .arg(radioTxModeTarget()));
    return true;
  }
  return true;
}

bool AppSettings::syncControlledTxFrequencyToRx() {
  const auto rx_rf_hz = controlledRxRfHz();
  if (!rx_rf_hz || !radioTxFrequencySyncAvailable()) {
    setStatusMessage(QStringLiteral(
        "VFO frequency sync is unavailable from the selected radio provider."));
    return false;
  }
  return requestControlledTxRfFrequency(*rx_rf_hz);
}

bool AppSettings::setControlledSplit(const bool enabled) {
  const auto validation = cwassistant::core::validate_radio_command(
      radio_state_, cwassistant::core::SetSplit{enabled});
  if (validation != cwassistant::core::RadioCommandValidation::Valid) {
    setStatusMessage(QStringLiteral(
        "Split control is unavailable from the selected radio provider."));
    return false;
  }
  bool accepted = false;
  if (frequency_backend_index_ == 0) {
#ifdef Q_OS_WIN
    accepted = writeOmniRigSplit(enabled);
#endif
  } else if (frequency_backend_index_ == 2 && cat4om_client_) {
    accepted = cat4om_client_->setSplit(enabled);
  } else if (frequency_backend_index_ == 1 && hamlib_client_) {
    accepted = hamlib_client_->setSplit(enabled);
  }
  if (!accepted) {
    setStatusMessage(
        QStringLiteral("The provider did not accept the split request."));
    return false;
  }
  setStatusMessage(
      enabled
          ? QStringLiteral("Split requested; awaiting provider readback.")
          : QStringLiteral("Simplex requested; awaiting provider readback."));
  return true;
}

bool AppSettings::writeControlledTxDialFrequency(
    const std::uint64_t dial_frequency_hz) {
  if (cwassistant::core::validate_radio_command(
          radio_state_, cwassistant::core::SetTxFrequency{dial_frequency_hz}) !=
      cwassistant::core::RadioCommandValidation::Valid) {
    setStatusMessage(QStringLiteral("TX-frequency control is unavailable."));
    return false;
  }
  if (frequency_backend_index_ == 0) {
#ifdef Q_OS_WIN
    return writeOmniRigTxFrequency(dial_frequency_hz);
#else
    return false;
#endif
  }
  if (frequency_backend_index_ == 1 && hamlib_client_)
    return hamlib_client_->setTxFrequency(dial_frequency_hz);
  return frequency_backend_index_ == 2 && cat4om_client_ &&
         cat4om_client_->setTxFrequency(dial_frequency_hz);
}

bool AppSettings::writeControlledMode(const cwassistant::core::RadioMode mode,
                                      const bool tx) {
  const cwassistant::core::RadioCommand command =
      tx ? cwassistant::core::RadioCommand(cwassistant::core::SetTxMode{mode})
         : cwassistant::core::RadioCommand(cwassistant::core::SetRxMode{mode});
  if (cwassistant::core::validate_radio_command(radio_state_, command) !=
      cwassistant::core::RadioCommandValidation::Valid) {
    setStatusMessage(
        QStringLiteral("%1-mode control is unavailable from this provider.")
            .arg(tx ? QStringLiteral("TX") : QStringLiteral("RX")));
    return false;
  }
  bool accepted = false;
  if (frequency_backend_index_ == 0 && !tx) {
#ifdef Q_OS_WIN
    accepted = writeOmniRigMode(mode);
#endif
  } else if (frequency_backend_index_ == 2 && cat4om_client_) {
    const auto& vfo = tx ? radio_state_.tx_vfo : radio_state_.rx_vfo;
    accepted = cat4om_client_->setMode(
        mode, QString::fromStdString(vfo.identifier), tx);
  } else if (frequency_backend_index_ == 1 && hamlib_client_) {
    accepted =
        tx ? hamlib_client_->setTxMode(mode) : hamlib_client_->setRxMode(mode);
  }
  if (!accepted) {
    setStatusMessage(
        QStringLiteral("The provider did not accept the mode request."));
    return false;
  }
  setStatusMessage(
      QStringLiteral("%1 mode requested; awaiting provider readback.")
          .arg(tx ? QStringLiteral("TX") : QStringLiteral("RX")));
  return true;
}

const QString& AppSettings::keyingPort() const noexcept { return keying_port_; }
bool AppSettings::directKeyingEnabled() const noexcept {
  return direct_keying_enabled_;
}
bool AppSettings::directKeyingValidated() const noexcept {
  return direct_keying_validated_;
}
const QString& AppSettings::directKeyingAcceptanceStatus() const noexcept {
  return direct_keying_acceptance_status_;
}
int AppSettings::pttLineIndex() const noexcept { return ptt_line_index_; }
int AppSettings::keyLineIndex() const noexcept { return key_line_index_; }
bool AppSettings::pttActiveHigh() const noexcept { return ptt_active_high_; }
bool AppSettings::keyActiveHigh() const noexcept { return key_active_high_; }
int AppSettings::txSpeedMode() const noexcept { return tx_speed_mode_; }
int AppSettings::fixedTxWpm() const noexcept { return fixed_tx_wpm_; }
const QString& AppSettings::txMacro1() const noexcept { return tx_macro_1_; }
const QString& AppSettings::txMacro2() const noexcept { return tx_macro_2_; }
const QString& AppSettings::txMacro3() const noexcept { return tx_macro_3_; }
const QString& AppSettings::txMacro4() const noexcept { return tx_macro_4_; }
int AppSettings::targetFps() const noexcept { return target_fps_; }
int AppSettings::waterfallRate() const noexcept { return waterfall_rate_; }
int AppSettings::waterfallTimeSpanSeconds() const noexcept {
  return waterfall_time_span_seconds_;
}
int AppSettings::spectrumDisplayMode() const noexcept {
  return spectrum_display_mode_;
}
bool AppSettings::automaticRange() const noexcept { return automatic_range_; }
double AppSettings::lowerBoundDb() const noexcept { return lower_bound_db_; }
double AppSettings::upperBoundDb() const noexcept { return upper_bound_db_; }
double AppSettings::automaticRangeSpanDb() const noexcept {
  return automatic_range_span_db_;
}
bool AppSettings::waterfallNoiseSuppression() const noexcept {
  return waterfall_noise_suppression_;
}
double AppSettings::waterfallNoiseMarginDb() const noexcept {
  return waterfall_noise_margin_db_;
}
bool AppSettings::showCwGuide() const noexcept { return show_cw_guide_; }
double AppSettings::cwGuideCenterHz() const noexcept {
  return cw_guide_center_hz_;
}
double AppSettings::cwGuideWidthHz() const noexcept {
  return cw_guide_width_hz_;
}
int AppSettings::averagingFrames() const noexcept { return averaging_frames_; }
bool AppSettings::showGrid() const noexcept { return show_grid_; }
bool AppSettings::showSpectrumGestureHints() const noexcept {
  return show_spectrum_gesture_hints_;
}
int AppSettings::decodedSignalTimeoutSeconds() const noexcept {
  return decoded_signal_timeout_seconds_;
}
bool AppSettings::decodeWeakSignals() const noexcept {
  return decode_weak_signals_;
}
double AppSettings::minimumDecodeSnrDb() const noexcept {
  return minimum_decode_snr_db_;
}
bool AppSettings::callsignDatabaseCorrectionEnabled() const noexcept {
  return callsign_database_correction_enabled_;
}
const QString& AppSettings::keyingModel() const noexcept {
  return keying_model_;
}
int AppSettings::debugCaptureMaximumSeconds() const noexcept {
  return debug_capture_maximum_seconds_;
}
const QString& AppSettings::operatorRole() const noexcept {
  return operator_role_;
}

bool AppSettings::localDecoderEnabled() const noexcept {
  return local_decoder_enabled_;
}
const QString& AppSettings::localDecoderModelPath() const noexcept {
  return local_decoder_model_path_;
}
const QString& AppSettings::localDecoderMetadataPath() const noexcept {
  return local_decoder_metadata_path_;
}
bool AppSettings::localDecoderBackendAvailable() const noexcept {
#if defined(CWA_HAVE_ONNX_RUNTIME) && CWA_HAVE_ONNX_RUNTIME
  return true;
#else
  return false;
#endif
}
QString AppSettings::localDecoderStatus() const {
  if (!localDecoderBackendAvailable()) {
    return QStringLiteral(
        "Local model support is unavailable in this build. "
        "Deterministic decoding remains active.");
  }
  if (!local_decoder_enabled_) return QStringLiteral("Local model disabled.");
  if (local_decoder_model_path_.isEmpty() ||
      local_decoder_metadata_path_.isEmpty()) {
    return QStringLiteral("Select both a model and its metadata file.");
  }
  return QStringLiteral("Configuration ready to validate and load.");
}
bool AppSettings::localCallsignDatabaseEnabled() const noexcept {
  return local_callsign_database_enabled_;
}
const QString& AppSettings::localCallsignDatabasePath() const noexcept {
  return local_callsign_database_path_;
}
const QString& AppSettings::localCallsignDatabaseStatus() const noexcept {
  return local_callsign_database_status_;
}
int AppSettings::dxSpotsRetentionMinutes() const noexcept {
  return dx_spots_retention_minutes_;
}
int AppSettings::dxSpotsToleranceHz() const noexcept {
  return dx_spots_tolerance_hz_;
}
bool AppSettings::dxSpotsShowLabels() const noexcept {
  return dx_spots_show_labels_;
}
bool AppSettings::dxClusterEnabled() const noexcept {
  return dx_cluster_enabled_;
}
int AppSettings::dxClusterServerIndex() const noexcept {
  return dx_cluster_server_index_;
}
const QString& AppSettings::dxClusterCustomHost() const noexcept {
  return dx_cluster_custom_host_;
}
int AppSettings::dxClusterCustomPort() const noexcept {
  return dx_cluster_custom_port_;
}
int AppSettings::dxClusterLoginSsid() const noexcept {
  return dx_cluster_login_ssid_;
}
QString AppSettings::dxClusterLoginCallsign() const {
  // Composed, never stored. The station callsign is the identity and the SSID
  // only says which of this operator's connections this one is, so there is no
  // third value here to drift from either of them. No callsign means no login
  // at all rather than a bare SSID: a cluster cannot be joined anonymously.
  if (own_callsign_.isEmpty()) return {};
  if (dx_cluster_login_ssid_ <= 0) return own_callsign_;
  return own_callsign_ + QStringLiteral("-") +
         QString::number(dx_cluster_login_ssid_);
}
const QVariantList& AppSettings::dxClusterServers() const noexcept {
  return dx_cluster_servers_;
}
bool AppSettings::diagnosticsServerEnabled() const noexcept {
  return diagnostics_server_enabled_;
}
int AppSettings::diagnosticsServerPort() const noexcept {
  return diagnostics_server_port_;
}
const QString& AppSettings::diagnosticsServerToken() const noexcept {
  return diagnostics_server_token_;
}
const QStringList& AppSettings::diagnosticsServerAddresses() const noexcept {
  return diagnostics_server_addresses_;
}
const QStringList& AppSettings::diagnosticsAllowedPeers() const noexcept {
  return diagnostics_allowed_peers_;
}
const QVariantList& AppSettings::diagnosticsServerAvailableAddresses()
    const noexcept {
  return diagnostics_server_available_addresses_;
}
bool AppSettings::waterfallRenderingEnabled() const noexcept {
  return waterfall_rendering_enabled_;
}
const QString& AppSettings::statusMessage() const noexcept {
  return status_message_;
}

#define CWA_SETTER(Method, Member, Type)    \
  void AppSettings::Method(Type value) {    \
    if (assign_if_changed(Member, value)) { \
      emit settingsChanged();               \
    }                                       \
  }

void AppSettings::setFrequencyBackendIndex(const int value) {
  if (assign_if_changed(frequency_backend_index_, value)) {
    if (frequency_backend_index_ != 1 && hamlib_client_)
      hamlib_client_->disconnectFromServer();
    if (frequency_backend_index_ != 2 && cat4om_client_)
      cat4om_client_->disconnectFromServer();
    pending_rx_rf_hz_.reset();
    pending_frequency_backend_index_ = -1;
    radio_frequency_request_timer_.stop();
    emit settingsChanged();
  }
}
void AppSettings::setReceiverInputTypeIndex(const int value) {
  const int requested = std::clamp(value, 0, 1);
  if (requested == 1 && (!sdr_backend_available_ || sdrDeviceIndex() < 0)) {
    const bool changed = receiver_input_type_index_ != 0;
    receiver_input_type_index_ = 0;
    setStatusMessage(
        sdr_backend_available_
            ? QStringLiteral("Select a discovered SDR device first. Sound-card "
                             "audio remains selected.")
            : QStringLiteral("This build has no SoapySDR support. Sound-card "
                             "audio remains selected."));
    emit sdrSettingsChanged();
    if (changed) emit receiverInputTypeChanged();
    return;
  }
  if (assign_if_changed(receiver_input_type_index_, requested)) {
    emit receiverInputTypeChanged();
    emit settingsChanged();
  }
}
void AppSettings::setPreferredSourceMode(const int value) {
  const int requested = std::clamp(value, 0, 2);
  if (!assign_if_changed(preferred_source_mode_, requested)) return;
  // Written through here rather than waiting for apply(). The operator changes
  // source from the main window, which never presses Apply, so a value only
  // committed by apply() would be lost by exactly the restart it exists to
  // survive. One key is written, so an unapplied edit elsewhere in the
  // settings page is left untouched.
  QSettings settings;
  settings.setValue(storageKey(QStringLiteral("source/preferredMode")),
                    preferred_source_mode_);
  emit settingsChanged();
}
void AppSettings::setSdrCenterFrequencyHz(const qulonglong value) {
  const qulonglong previous = sdr_center_frequency_hz_;
  if (assign_if_changed(sdr_center_frequency_hz_, value)) {
    if (!sdr_follow_radio_vfo_) {
      const qint64 shifted_decoder = std::clamp<qint64>(
          static_cast<qint64>(sdr_decoder_center_frequency_hz_) +
              (static_cast<qint64>(value) - static_cast<qint64>(previous)),
          1LL, 99'000'000'000LL);
      sdr_decoder_center_frequency_hz_ =
          static_cast<qulonglong>(shifted_decoder);
    }
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}
void AppSettings::setSdrSampleRateHz(const int value) {
  if (assign_if_changed(sdr_sample_rate_hz_, value)) {
    emit sdrSettingsChanged();
    emit settingsChanged();
    if (sdr_follow_radio_vfo_) followSdrToRadioVfo();
  }
}
void AppSettings::setSdrBandwidthHz(const int value) {
  if (assign_if_changed(sdr_bandwidth_hz_, value)) {
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}
void AppSettings::persistSdrDecoderWindow() {
  QSettings settings;
  settings.setValue(storageKey(QStringLiteral("sdr/decoderCenterFrequencyHz")),
                    QVariant::fromValue(sdr_decoder_center_frequency_hz_));
  settings.setValue(storageKey(QStringLiteral("sdr/decoderBandwidthHz")),
                    sdr_decoder_bandwidth_hz_);
}
void AppSettings::setSdrDecoderCenterFrequencyHz(const qulonglong value) {
  const auto bounded = std::clamp<qulonglong>(value, 1ULL, 99'000'000'000ULL);
  if (assign_if_changed(sdr_decoder_center_frequency_hz_, bounded)) {
    // Written before the signals, so that a listener which corrects the
    // region -- the acquisition span clamp does -- writes its correction
    // after this and therefore last. The stored pair ends up being the one
    // that is actually in force.
    persistSdrDecoderWindow();
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}
void AppSettings::setSdrDecoderBandwidthHz(const int value) {
  const int bounded = std::clamp(value, kMinimumSdrDecoderBandwidthHz,
                                 kMaximumSdrDecoderBandwidthHz);
  if (assign_if_changed(sdr_decoder_bandwidth_hz_, bounded)) {
    persistSdrDecoderWindow();
    emit sdrSettingsChanged();
    emit settingsChanged();
    if (sdr_follow_radio_vfo_) followSdrToRadioVfo();
  }
}
void AppSettings::setSdrDecoderWindow(const qulonglong center_frequency_hz,
                                      const int bandwidth_hz) {
  const auto bounded_center =
      std::clamp<qulonglong>(center_frequency_hz, 1ULL, 99'000'000'000ULL);
  const int bounded_bandwidth =
      std::clamp(bandwidth_hz, kMinimumSdrDecoderBandwidthHz,
                 kMaximumSdrDecoderBandwidthHz);
  const bool center_changed =
      assign_if_changed(sdr_decoder_center_frequency_hz_, bounded_center);
  const bool bandwidth_changed =
      assign_if_changed(sdr_decoder_bandwidth_hz_, bounded_bandwidth);
  if (center_changed || bandwidth_changed) {
    // This is the end of a spectrum drag: the gesture reports once, on
    // release, not while the pointer moves, so one selection is one write and
    // there is nothing here to coalesce.
    persistSdrDecoderWindow();
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}

std::optional<std::uint64_t> AppSettings::observedRadioRxRfHz() const noexcept {
  if (!radio_enabled_ ||
      radio_state_.rx_frequency.observation !=
          cwassistant::core::RadioObservation::Known) {
    return std::nullopt;
  }
  const auto resolved = cwassistant::core::resolve_frequencies(
      {.rx_dial_hz = radio_state_.rx_frequency.hz,
       .tx_dial_hz = radio_state_.rx_frequency.hz,
       .split_enabled = false},
      {.rx_offset_hz = rx_transverter_offset_hz_,
       .tx_offset_hz = rx_transverter_offset_hz_});
  return resolved ? std::optional<std::uint64_t>(resolved->rx_rf_hz)
                  : std::nullopt;
}

void AppSettings::setSdrRadioWindow(const std::uint64_t rx_frequency_hz) {
  const qint64 available_offset = std::max<qint64>(
      0, static_cast<qint64>(sdr_sample_rate_hz_ / 2) -
             static_cast<qint64>(sdr_decoder_bandwidth_hz_ / 2) - 1'000);
  const qint64 offset =
      std::clamp(sdr_radio_lo_offset_hz_, -available_offset, available_offset);
  const qint64 acquisition_center =
      std::clamp<qint64>(static_cast<qint64>(rx_frequency_hz) + offset, 1LL,
                         99'000'000'000LL);
  const bool center_changed = assign_if_changed(
      sdr_center_frequency_hz_, static_cast<qulonglong>(acquisition_center));
  const bool decoder_changed = assign_if_changed(
      sdr_decoder_center_frequency_hz_,
      static_cast<qulonglong>(rx_frequency_hz));
  if (center_changed || decoder_changed) {
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}

void AppSettings::followSdrToRadioVfo() {
  if (!sdr_follow_radio_vfo_) return;
  const auto rx_frequency_hz = observedRadioRxRfHz();
  if (rx_frequency_hz) setSdrRadioWindow(*rx_frequency_hz);
}

bool AppSettings::requestSdrRxFrequencyHz(const qulonglong frequency_hz) {
  if (frequency_hz == 0U || frequency_hz > 99'000'000'000ULL) {
    setStatusMessage(QStringLiteral(
        "Select an SDR RX frequency between 1 Hz and 99 GHz."));
    return false;
  }
  if (!sdr_follow_radio_vfo_) {
    setSdrRadioWindow(static_cast<std::uint64_t>(frequency_hz));
    setStatusMessage(
        QStringLiteral("SDR RX frequency requested; CAT was not changed."));
    return true;
  }
  const auto dial_frequency = cwassistant::core::resolve_dial_frequency(
      static_cast<std::uint64_t>(frequency_hz), rx_transverter_offset_hz_);
  if (!dial_frequency || !writeRadioRxDialFrequency(*dial_frequency)) {
    setStatusMessage(QStringLiteral(
        "Radio Sync could not apply the requested RX frequency through the selected CAT provider."));
    return false;
  }
  rememberPendingRxFrequency(static_cast<std::uint64_t>(frequency_hz));
  setSdrRadioWindow(static_cast<std::uint64_t>(frequency_hz));
  setStatusMessage(QStringLiteral(
      "Synchronized RX frequency requested; CAT readback remains authoritative."));
  return true;
}

void AppSettings::stepSdrRxFrequency(const int direction) {
  if (direction != -1 && direction != 1) {
    setStatusMessage(QStringLiteral("SDR frequency step must be down or up."));
    return;
  }
  const auto current =
      static_cast<std::uint64_t>(sdr_decoder_center_frequency_hz_);
  const auto step = static_cast<std::uint64_t>(sdr_tuning_step_hz_);
  if ((direction < 0 && current <= step) ||
      (direction > 0 && current > 99'000'000'000ULL - step)) {
    setStatusMessage(QStringLiteral(
        "The requested SDR frequency step is outside the supported range."));
    return;
  }
  (void)requestSdrRxFrequencyHz(static_cast<qulonglong>(
      direction < 0 ? current - step : current + step));
}

void AppSettings::setSdrFollowRadioVfo(const bool value) {
  if (assign_if_changed(sdr_follow_radio_vfo_, value)) {
    emit sdrSettingsChanged();
    emit settingsChanged();
    if (value) followSdrToRadioVfo();
  }
}
void AppSettings::setSdrTuningStepHz(const int value) {
  const int bounded = std::clamp(value, 1, 10'000'000);
  if (assign_if_changed(sdr_tuning_step_hz_, bounded)) {
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}
void AppSettings::setSdrRadioLoOffsetHz(const qint64 value) {
  if (assign_if_changed(sdr_radio_lo_offset_hz_, value)) {
    emit sdrSettingsChanged();
    emit settingsChanged();
    if (sdr_follow_radio_vfo_) followSdrToRadioVfo();
  }
}
void AppSettings::setSdrAutomaticGain(const bool value) {
  if (assign_if_changed(sdr_automatic_gain_, value)) {
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}
void AppSettings::setSdrGainDb(const double value) {
  if (assign_if_changed(sdr_gain_db_, value)) {
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}
CWA_SETTER(setAudioDcRejection, audio_dc_rejection_, bool)
CWA_SETTER(setAudioAutomaticGain, audio_automatic_gain_, bool)
CWA_SETTER(setAudioGainDb, audio_gain_db_, double)
CWA_SETTER(setAudioAutomaticGainTargetDbfs, audio_automatic_gain_target_dbfs_,
           double)
CWA_SETTER(setAudioAutomaticBandwidth, audio_automatic_bandwidth_, bool)
CWA_SETTER(setAudioLowerFrequencyHz, audio_lower_frequency_hz_, double)
CWA_SETTER(setAudioUpperFrequencyHz, audio_upper_frequency_hz_, double)
CWA_SETTER(setAudioInputRadioLinked, audio_input_radio_linked_, bool)
CWA_SETTER(setRadioEnabled, radio_enabled_, bool)
CWA_SETTER(setOmniRigSlot, omnirig_slot_, int)
CWA_SETTER(setHamlibHost, hamlib_host_, const QString&)
CWA_SETTER(setHamlibPort, hamlib_port_, int)
CWA_SETTER(setHamlibRxVfo, hamlib_rx_vfo_, const QString&)
CWA_SETTER(setHamlibTxVfo, hamlib_tx_vfo_, const QString&)
CWA_SETTER(setHamlibWritable, hamlib_writable_, bool)
CWA_SETTER(setCat4omUrl, cat4om_url_, const QString&)
CWA_SETTER(setCat4omRadioId, cat4om_radio_id_, const QString&)
CWA_SETTER(setCat4omPassword, cat4om_password_, const QString&)
CWA_SETTER(setCatPort, cat_port_, const QString&)
void AppSettings::setCatBaudRate(const int value) {
  const auto bounded = static_cast<std::uint32_t>(std::max(value, 0));
  const int normalized = static_cast<int>(
      cwassistant::core::nearest_supported_serial_baud_rate(bounded));
  if (assign_if_changed(cat_baud_rate_, normalized)) emit settingsChanged();
}
void AppSettings::setCatDataBits(const int value) {
  if (assign_if_changed(cat_data_bits_, std::clamp(value, 5, 8)))
    emit settingsChanged();
}
void AppSettings::setCatParityIndex(const int value) {
  if (assign_if_changed(cat_parity_index_, std::clamp(value, 0, 2)))
    emit settingsChanged();
}
void AppSettings::setCatStopBits(const int value) {
  if (assign_if_changed(cat_stop_bits_, std::clamp(value, 1, 2)))
    emit settingsChanged();
}
CWA_SETTER(setCatFlowControlIndex, cat_flow_control_index_, int)
CWA_SETTER(setPollIntervalMs, poll_interval_ms_, int)
CWA_SETTER(setTimeoutMs, timeout_ms_, int)
CWA_SETTER(setSplitEnabled, split_enabled_, bool)
CWA_SETTER(setRxTransverterOffsetHz, rx_transverter_offset_hz_, qint64)
CWA_SETTER(setTxTransverterOffsetHz, tx_transverter_offset_hz_, qint64)
CWA_SETTER(setCwToneSidebandIndex, cw_tone_sideband_index_, int)
void AppSettings::setKeyingPort(const QString& value) {
  if (!assign_if_changed(keying_port_, value)) return;
  invalidateDirectKeyingAcceptance(QStringLiteral(
      "Keying configuration changed; run the physical loopback again."));
  emit settingsChanged();
}
void AppSettings::setDirectKeyingEnabled(const bool value) {
  if (!assign_if_changed(direct_keying_enabled_, value)) return;
  invalidateDirectKeyingAcceptance(
      value
          ? QStringLiteral("Direct keying enabled; run the physical loopback.")
          : QStringLiteral(
                "Direct keying disabled; prior acceptance was cleared."));
  emit settingsChanged();
}
void AppSettings::setPttLineIndex(const int value) {
  if (!assign_if_changed(ptt_line_index_, value)) return;
  invalidateDirectKeyingAcceptance(QStringLiteral(
      "Keying configuration changed; run the physical loopback again."));
  emit settingsChanged();
}
void AppSettings::setKeyLineIndex(const int value) {
  if (!assign_if_changed(key_line_index_, value)) return;
  invalidateDirectKeyingAcceptance(QStringLiteral(
      "Keying configuration changed; run the physical loopback again."));
  emit settingsChanged();
}
void AppSettings::setPttActiveHigh(const bool value) {
  if (!assign_if_changed(ptt_active_high_, value)) return;
  invalidateDirectKeyingAcceptance(QStringLiteral(
      "Keying configuration changed; run the physical loopback again."));
  emit settingsChanged();
}
void AppSettings::setKeyActiveHigh(const bool value) {
  if (!assign_if_changed(key_active_high_, value)) return;
  invalidateDirectKeyingAcceptance(QStringLiteral(
      "Keying configuration changed; run the physical loopback again."));
  emit settingsChanged();
}
void AppSettings::setTxSpeedMode(const int value) {
  if (assign_if_changed(tx_speed_mode_, std::clamp(value, 0, 1)))
    emit settingsChanged();
}
void AppSettings::setFixedTxWpm(const int value) {
  if (assign_if_changed(fixed_tx_wpm_, std::clamp(value, 5, 80)))
    emit settingsChanged();
}
void AppSettings::setTxMacro1(const QString& value) {
  if (assign_if_changed(tx_macro_1_, value.simplified().toUpper().left(64)))
    emit settingsChanged();
}
void AppSettings::setTxMacro2(const QString& value) {
  if (assign_if_changed(tx_macro_2_, value.simplified().toUpper().left(64)))
    emit settingsChanged();
}
void AppSettings::setTxMacro3(const QString& value) {
  if (assign_if_changed(tx_macro_3_, value.simplified().toUpper().left(64)))
    emit settingsChanged();
}
void AppSettings::setTxMacro4(const QString& value) {
  if (assign_if_changed(tx_macro_4_, value.simplified().toUpper().left(64)))
    emit settingsChanged();
}
CWA_SETTER(setTargetFps, target_fps_, int)
CWA_SETTER(setWaterfallRate, waterfall_rate_, int)
CWA_SETTER(setWaterfallTimeSpanSeconds, waterfall_time_span_seconds_, int)
CWA_SETTER(setSpectrumDisplayMode, spectrum_display_mode_, int)
CWA_SETTER(setAutomaticRange, automatic_range_, bool)
CWA_SETTER(setLowerBoundDb, lower_bound_db_, double)
CWA_SETTER(setUpperBoundDb, upper_bound_db_, double)
CWA_SETTER(setAutomaticRangeSpanDb, automatic_range_span_db_, double)
CWA_SETTER(setWaterfallNoiseSuppression, waterfall_noise_suppression_, bool)
CWA_SETTER(setWaterfallNoiseMarginDb, waterfall_noise_margin_db_, double)
CWA_SETTER(setShowCwGuide, show_cw_guide_, bool)
CWA_SETTER(setCwGuideCenterHz, cw_guide_center_hz_, double)
CWA_SETTER(setCwGuideWidthHz, cw_guide_width_hz_, double)
CWA_SETTER(setAveragingFrames, averaging_frames_, int)
CWA_SETTER(setShowGrid, show_grid_, bool)
CWA_SETTER(setWaterfallRenderingEnabled, waterfall_rendering_enabled_, bool)
CWA_SETTER(setShowSpectrumGestureHints, show_spectrum_gesture_hints_, bool)
CWA_SETTER(setDecodedSignalTimeoutSeconds, decoded_signal_timeout_seconds_, int)
CWA_SETTER(setDecodeWeakSignals, decode_weak_signals_, bool)
CWA_SETTER(setMinimumDecodeSnrDb, minimum_decode_snr_db_, double)
CWA_SETTER(setLocalDecoderEnabled, local_decoder_enabled_, bool)
CWA_SETTER(setCallsignDatabaseCorrectionEnabled,
           callsign_database_correction_enabled_, bool)
CWA_SETTER(setKeyingModel, keying_model_, const QString&)
CWA_SETTER(setDebugCaptureMaximumSeconds, debug_capture_maximum_seconds_, int)
CWA_SETTER(setOperatorRole, operator_role_, const QString&)

CWA_SETTER(setDxSpotsShowLabels, dx_spots_show_labels_, bool)

void AppSettings::setDxClusterEnabled(const bool value) {
  // A cluster login is the station callsign, and there is no anonymous one.
  // Turning the link on without a callsign configured is refused here rather
  // than discovered later as a connection that never completes, and the
  // message says where the callsign is set.
  if (value && own_callsign_.isEmpty()) {
    setStatusMessage(QStringLiteral(
        "Set the station callsign first; a cluster login is sent as your "
        "callsign and cannot be made anonymously."));
    emit settingsChanged();
    return;
  }
  if (assign_if_changed(dx_cluster_enabled_, value)) {
    emit settingsChanged();
  }
}

void AppSettings::setDxSpotsRetentionMinutes(const int value) {
  const int clamped = std::clamp(value, 1, 60);
  if (assign_if_changed(dx_spots_retention_minutes_, clamped)) {
    emit settingsChanged();
  }
}

void AppSettings::setDxSpotsToleranceHz(const int value) {
  const int clamped = std::clamp(value, 50, 1'000);
  if (assign_if_changed(dx_spots_tolerance_hz_, clamped)) {
    emit settingsChanged();
  }
}

void AppSettings::setDxClusterServerIndex(const int value) {
  // -1 is the custom entry and is always selectable. Any other value has to
  // name an entry that exists: a stored index pointing past the end of an
  // edited server list must not be carried forward as a connection attempt to
  // whatever now happens to sit at that position.
  const int server_count = static_cast<int>(dx_cluster_servers_.size());
  int bounded = value;
  if (bounded != kDxClusterCustomServerIndex &&
      (bounded < 0 || bounded >= server_count)) {
    bounded = server_count > 0 ? 0 : kDxClusterCustomServerIndex;
  }
  if (assign_if_changed(dx_cluster_server_index_, bounded)) {
    emit settingsChanged();
  }
}

void AppSettings::setDxClusterCustomHost(const QString& value) {
  if (assign_if_changed(dx_cluster_custom_host_,
                        value.trimmed().left(kMaximumDxClusterHostLength))) {
    emit settingsChanged();
  }
}

void AppSettings::setDxClusterCustomPort(const int value) {
  const int clamped = std::clamp(value, 1, 65'535);
  if (assign_if_changed(dx_cluster_custom_port_, clamped)) {
    emit settingsChanged();
  }
}

void AppSettings::setDxClusterLoginSsid(const int value) {
  // 0..99 is what a cluster accepts behind the hyphen, and it is the whole
  // range the settings page offers. A value outside it is clamped rather than
  // refused: the number decides nothing but which of the operator's own
  // connections this is.
  const int clamped = std::clamp(value, 0, 99);
  if (assign_if_changed(dx_cluster_login_ssid_, clamped)) {
    emit settingsChanged();
  }
}


bool AppSettings::diagnosticsServerExposureUnguarded() const {
  if (DiagnosticsServer::isAcceptableToken(diagnostics_server_token_)) {
    return false;
  }
  return !first_routable_diagnostics_address(diagnostics_server_addresses_)
              .isEmpty();
}

void AppSettings::setDiagnosticsServerEnabled(const bool value) {
  // Binding the station's internals to an address other machines can reach is
  // a deliberate act, and an unguarded one is refused here rather than
  // discovered later as a frequency, a decoded callsign or a transcript read
  // by whoever reached the port first. Loopback needs no token, because
  // nothing off this machine can reach it; every other address does. The
  // message names the address that made the token necessary, because a
  // refusal that does not say which of several chosen addresses caused it
  // reads as the switch being broken.
  if (value && diagnosticsServerExposureUnguarded()) {
    setStatusMessage(
        QStringLiteral(
            "Set a diagnostics token of at least %1 characters first. %2 can "
            "be reached from other machines, and this stream publishes "
            "frequencies, decoded callsigns, device identifiers and "
            "transcripts. Generate a token, or bind only to loopback.")
            .arg(DiagnosticsServer::kMinimumTokenLength)
            .arg(first_routable_diagnostics_address(
                diagnostics_server_addresses_)));
    emit settingsChanged();
    return;
  }
  if (assign_if_changed(diagnostics_server_enabled_, value)) {
    emit settingsChanged();
  }
}

void AppSettings::setDiagnosticsServerPort(const int value) {
  const int clamped =
      std::clamp(value, kMinimumDiagnosticsServerPort, 65'535);
  if (assign_if_changed(diagnostics_server_port_, clamped)) {
    emit settingsChanged();
  }
}

void AppSettings::setDiagnosticsServerToken(const QString& value) {
  const QString trimmed = value.trimmed().left(kMaximumDiagnosticsTokenLength);
  if (!assign_if_changed(diagnostics_server_token_, trimmed)) return;
  // The guard cannot be walked around by enabling the stream with a token and
  // then removing it: a token that no longer guards what is already being
  // published takes the service down with it.
  if (diagnostics_server_enabled_ && diagnosticsServerExposureUnguarded()) {
    diagnostics_server_enabled_ = false;
    setStatusMessage(QStringLiteral(
        "Diagnostics streaming switched off: the token no longer guards an "
        "address that other machines can reach."));
  }
  emit settingsChanged();
}

void AppSettings::setDiagnosticsServerAddresses(const QStringList& value) {
  if (!assign_if_changed(diagnostics_server_addresses_,
                         sanitize_diagnostics_addresses(value))) {
    return;
  }
  // The same guard from the other direction. Adding a routable address to a
  // service that had been running on loopback is the moment the token starts
  // to matter, and it must not be the moment the requirement is skipped.
  if (diagnostics_server_enabled_ && diagnosticsServerExposureUnguarded()) {
    diagnostics_server_enabled_ = false;
    setStatusMessage(
        QStringLiteral("Diagnostics streaming switched off: %1 can be reached "
                       "from other machines and no token is set.")
            .arg(first_routable_diagnostics_address(
                diagnostics_server_addresses_)));
  }
  emit settingsChanged();
}

void AppSettings::setDiagnosticsAllowedPeers(const QStringList& value) {
  if (!assign_if_changed(diagnostics_allowed_peers_,
                         sanitize_diagnostics_peers(value))) {
    return;
  }
  // No guard follows, and that is deliberate. An empty list is not a missing
  // list: it permits loopback only, so emptying it can only ever narrow what
  // the service admits, and narrowing is never a reason to refuse or to switch
  // anything off. The one exposure worth guarding -- a routable address with
  // no token -- is decided by the token and the addresses, above.
  emit settingsChanged();
}

QString AppSettings::localNetworkForAddress(const QString& address) const {
  return DiagnosticsServer::localSegmentForAddress(address);
}

void AppSettings::refreshNetworkAddresses() {
  QVariantList discovered = DiagnosticsServer::discoverLocalAddresses();
  if (discovered == diagnostics_server_available_addresses_) return;
  diagnostics_server_available_addresses_ = std::move(discovered);
  emit diagnosticsNetworkAddressesChanged();
}

QString AppSettings::generateDiagnosticsToken() {
  // The system source rather than the default engine: this value stands
  // between the station's internals and the network, so it must not come from
  // a generator whose sequence could be reproduced.
  constexpr int alphabet_size =
      static_cast<int>(sizeof(kDiagnosticsTokenAlphabet)) - 1;
  auto* generator = QRandomGenerator::system();
  QString token;
  token.reserve(kGeneratedDiagnosticsTokenLength);
  for (int index = 0; index < kGeneratedDiagnosticsTokenLength; ++index) {
    token.append(QLatin1Char(
        kDiagnosticsTokenAlphabet[generator->bounded(alphabet_size)]));
  }
  return token;
}

void AppSettings::setLocalCallsignDatabaseEnabled(const bool value) {
  if (!assign_if_changed(local_callsign_database_enabled_, value)) return;
  local_callsign_database_status_ =
      value
          ? (local_callsign_database_path_.isEmpty()
                 ? QStringLiteral(
                       "Select a local master.scp or Call History text file.")
                 : QStringLiteral("Loading the selected local callsign list."))
          : QStringLiteral("Disabled. No local callsign list is in use.");
  emit settingsChanged();
  emit localCallsignDatabaseChanged();
  emit localCallsignDatabaseConfigurationCommitted(
      local_callsign_database_enabled_, local_callsign_database_path_);
}

void AppSettings::setRadioTuningStepHz(const int value) {
  const int clamped = std::clamp(value, 1'000, 100'000);
  if (assign_if_changed(radio_tuning_step_hz_, clamped)) {
    emit settingsChanged();
  }
}

#undef CWA_SETTER

void AppSettings::setOwnCallsign(const QString& value) {
  const QString trimmed = value.trimmed();
  if (trimmed.isEmpty()) {
    if (assign_if_changed(own_callsign_, QString{})) {
      // The cluster link logs in as this callsign and has no other identity to
      // offer, so clearing it switches the link off rather than leaving a
      // connection configured that can never complete.
      dx_cluster_enabled_ = false;
      emit settingsChanged();
    }
    return;
  }
  const auto normalized =
      cwassistant::core::CallsignPolicy::normalize(trimmed.toStdString());
  if (!normalized.has_value()) {
    setStatusMessage(
        QStringLiteral("Enter a valid callsign containing letters and digits; "
                       "portable suffixes may use a single slash."));
    emit settingsChanged();
    return;
  }
  if (assign_if_changed(own_callsign_, QString::fromStdString(*normalized))) {
    setStatusMessage(
        QStringLiteral("Own callsign normalized and ready to save."));
    emit settingsChanged();
  }
}

namespace {

bool selectLocalFile(const QUrl& url, QString& destination) {
  if (!url.isLocalFile()) return false;
  const QFileInfo file(url.toLocalFile());
  if (!file.exists() || !file.isFile() || !file.isReadable()) return false;
  destination = file.canonicalFilePath();
  return !destination.isEmpty();
}

}  // namespace

bool AppSettings::selectLocalDecoderModel(const QUrl& url) {
  QString path;
  if (!selectLocalFile(url, path)) {
    setStatusMessage(QStringLiteral("Select a readable local model file."));
    return false;
  }
  if (assign_if_changed(local_decoder_model_path_, path))
    emit settingsChanged();
  setStatusMessage(QStringLiteral("Local model selected. Apply to save."));
  return true;
}

bool AppSettings::selectLocalDecoderMetadata(const QUrl& url) {
  QString path;
  if (!selectLocalFile(url, path)) {
    setStatusMessage(QStringLiteral("Select a readable local metadata file."));
    return false;
  }
  if (assign_if_changed(local_decoder_metadata_path_, path))
    emit settingsChanged();
  setStatusMessage(
      QStringLiteral("Local model metadata selected. Apply to save."));
  return true;
}

void AppSettings::clearLocalDecoderModel() {
  if (assign_if_changed(local_decoder_model_path_, QString{}))
    emit settingsChanged();
}

void AppSettings::clearLocalDecoderMetadata() {
  if (assign_if_changed(local_decoder_metadata_path_, QString{}))
    emit settingsChanged();
}

bool AppSettings::selectLocalCallsignDatabase(const QUrl& url) {
  QString path;
  if (!selectLocalFile(url, path)) {
    local_callsign_database_status_ =
        QStringLiteral("Select a readable local text callsign-list file.");
    emit localCallsignDatabaseChanged();
    setStatusMessage(local_callsign_database_status_);
    return false;
  }
  if (assign_if_changed(local_callsign_database_path_, path))
    emit settingsChanged();
  local_callsign_database_status_ = QStringLiteral(
      "Local file selected; suggestions remain separate from decoded text.");
  emit localCallsignDatabaseChanged();
  if (local_callsign_database_enabled_) {
    emit localCallsignDatabaseConfigurationCommitted(
        true, local_callsign_database_path_);
  }
  setStatusMessage(
      QStringLiteral("Local callsign-list file selected. Apply to save."));
  return true;
}

void AppSettings::clearLocalCallsignDatabase() {
  if (assign_if_changed(local_callsign_database_path_, QString{}))
    emit settingsChanged();
  local_callsign_database_status_ =
      local_callsign_database_enabled_
          ? QStringLiteral(
                "Select a local master.scp or Call History text file.")
          : QStringLiteral("Disabled. No local callsign list is in use.");
  emit localCallsignDatabaseChanged();
  emit localCallsignDatabaseConfigurationCommitted(
      local_callsign_database_enabled_, QString{});
}

bool AppSettings::reloadLocalCallsignDatabase() {
  if (!local_callsign_database_enabled_) {
    local_callsign_database_status_ = QStringLiteral(
        "Enable the local callsign suggestion source before reloading it.");
    emit localCallsignDatabaseChanged();
    return false;
  }
  if (local_callsign_database_path_.isEmpty()) {
    local_callsign_database_status_ =
        QStringLiteral("Select a local callsign-list file before reloading.");
    emit localCallsignDatabaseChanged();
    return false;
  }
  local_callsign_database_status_ =
      QStringLiteral("Reloading local callsign list.");
  emit localCallsignDatabaseChanged();
  emit localCallsignDatabaseConfigurationCommitted(
      true, local_callsign_database_path_);
  return true;
}

void AppSettings::selectReferenceRig(const int index) {
  const auto profiles = cwassistant::core::reference_rig_profiles();
  if (index < 0 || static_cast<std::size_t>(index) >= profiles.size()) {
    return;
  }
  reference_rig_index_ = index;
  applyReferenceDefaults(index);
  setStatusMessage(
      QStringLiteral("Reference defaults loaded; all fields remain editable."));
  emit settingsChanged();
}

void AppSettings::resetToReferenceDefaults() {
  applyReferenceDefaults(reference_rig_index_);
  setStatusMessage(
      QStringLiteral("Radio defaults restored. Select Apply to persist them."));
  emit settingsChanged();
}

void AppSettings::applyReferenceDefaults(const int index) {
  const auto profiles = cwassistant::core::reference_rig_profiles();
  if (index < 0 || static_cast<std::size_t>(index) >= profiles.size()) {
    return;
  }
  const auto& profile = profiles[static_cast<std::size_t>(index)];
  direct_keying_validated_ = false;
  cat_baud_rate_ = static_cast<int>(profile.cat.baud_rate);
  cat_data_bits_ = static_cast<int>(profile.cat.data_bits);
  cat_parity_index_ = static_cast<int>(profile.cat.parity);
  cat_stop_bits_ = static_cast<int>(profile.cat.stop_bits);
  cat_flow_control_index_ = static_cast<int>(profile.cat.flow_control);
  poll_interval_ms_ = static_cast<int>(profile.poll_interval_ms);
  timeout_ms_ = static_cast<int>(profile.timeout_ms);
  ptt_line_index_ = static_cast<int>(profile.ptt_line);
  key_line_index_ = static_cast<int>(profile.key_line);
  ptt_active_high_ = profile.keying.rts_active_high;
  key_active_high_ = profile.keying.dtr_active_high;
}

void AppSettings::refreshSerialPorts() {
  QStringList ports;
  for (const auto& port : QSerialPortInfo::availablePorts()) {
    ports.push_back(port.portName());
  }
  ports.removeDuplicates();
  ports.sort(Qt::CaseInsensitive);
  if (ports != serial_ports_) {
    serial_ports_ = ports;
    emit serialPortsChanged();
  }
  setStatusMessage(QStringLiteral(
      "Serial ports refreshed without opening or toggling them."));
}

void AppSettings::refreshAudioInputs() {
  QStringList names{QStringLiteral("System default input (recommended)")};
  QStringList ids{QString{}};
  QStringList device_names{QString{}};
  QList<bool> defaults{false};
  for (const auto& device : QMediaDevices::audioInputs()) {
    const QString id = QString::fromLatin1(device.id().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    if (id.isEmpty() || ids.contains(id)) {
      continue;
    }
    QString name = device.description().trimmed();
    if (name.isEmpty()) {
      name = QStringLiteral("Audio input %1").arg(ids.size());
    }
    names.push_back(name);
    ids.push_back(id);
    device_names.push_back(name);
    defaults.push_back(device.isDefault());
  }

  // Two interfaces the operating system describes with identical words are
  // unchoosable: a station with two of the same model sees one row of text
  // twice and cannot tell which of them is the radio. Number them, in
  // enumeration order, so the list offers something to point at -- and so the
  // report that refuses to guess between them has names to quote. The same
  // numbering is applied to the candidates listed by the ambiguity report in
  // replay/live_audio_worker.cpp; the two must agree or the report names rows
  // that are not in the list.
  bool ambiguous = false;
  for (qsizetype row = 1; row < names.size(); ++row) {
    int ordinal = 0;
    int total = 0;
    for (qsizetype other = 1; other < device_names.size(); ++other) {
      if (device_names.at(other) != device_names.at(row)) continue;
      ++total;
      if (other <= row) ordinal = total;
    }
    if (total < 2) continue;
    ambiguous = true;
    names[row] = QStringLiteral("%1 #%2").arg(names.at(row)).arg(ordinal);
  }
  audio_input_names_ambiguous_ = ambiguous;

  // The default marker goes on after the ordinal, so it never lands between a
  // name and its number, and never becomes part of the raw name that identity
  // is matched on.
  for (qsizetype row = 1; row < names.size(); ++row) {
    if (defaults.at(row)) names[row] += QStringLiteral(" (current default)");
  }

  if (!audio_input_id_.isEmpty() && !ids.contains(audio_input_id_)) {
    const QString unavailable_name =
        audio_input_name_.isEmpty()
            ? QStringLiteral("Previously selected input")
            : audio_input_name_;
    names.push_back(unavailable_name + QStringLiteral(" (unavailable)"));
    ids.push_back(audio_input_id_);
    // An absent device keeps the raw name it was saved with. That name is the
    // only handle left on it, and losing it here would cost the very recovery
    // this list is describing the failure of.
    device_names.push_back(audio_input_device_name_);
  }

  const int selected_index = ids.indexOf(audio_input_id_);
  if (selected_index > 0) {
    audio_input_name_ = names.at(selected_index);
    if (audio_input_name_.endsWith(QStringLiteral(" (unavailable)"))) {
      audio_input_name_.chop(QStringLiteral(" (unavailable)").size());
    } else {
      audio_input_device_name_ = device_names.at(selected_index);
    }
  }

  const bool listing_changed =
      names != audio_input_names_ || ids != audio_input_ids_;
  audio_input_names_ = std::move(names);
  audio_input_ids_ = std::move(ids);
  audio_input_device_names_ = std::move(device_names);
  if (listing_changed) {
    emit audioInputsChanged();
  }
}

void AppSettings::selectAudioInput(const int index) {
  if (index < 0 || index >= audio_input_ids_.size()) {
    return;
  }
  audio_input_id_ = audio_input_ids_.at(index);
  audio_input_name_ = index == 0 ? QStringLiteral("System default input")
                                 : audio_input_names_.at(index);
  // The raw description, not the decorated row text: the ordinal and the
  // default marker describe where a device sits in today's enumeration, and
  // both are wrong tomorrow. Only what the operating system calls the device
  // is worth storing as its identity.
  audio_input_device_name_ =
      index < audio_input_device_names_.size()
          ? audio_input_device_names_.at(index)
          : QString{};
  if (audio_input_name_.endsWith(QStringLiteral(" (unavailable)"))) {
    audio_input_name_.chop(QStringLiteral(" (unavailable)").size());
  }
  setStatusMessage(
      QStringLiteral("Audio input selected. Live capture remains "
                     "disarmed until started by the operator."));
  emit audioInputsChanged();
  emit settingsChanged();
}

void AppSettings::adoptRecoveredAudioInput(const QString& encoded_id) {
  if (encoded_id.isEmpty() || encoded_id == audio_input_id_) return;
  audio_input_id_ = encoded_id;
  refreshAudioInputs();
  // Written now rather than at the next Apply. The identifier was recovered
  // during a start the operator did not initiate a settings change for, and if
  // the application closes before anything else is applied the station would
  // meet the same unresolvable selection at the next restart -- which is the
  // failure this whole path exists to end.
  QSettings settings;
  settings.setValue(storageKey(QStringLiteral("audio/inputId")),
                    audio_input_id_);
  settings.setValue(storageKey(QStringLiteral("audio/inputName")),
                    audio_input_name_);
  settings.setValue(storageKey(QStringLiteral("audio/inputDeviceName")),
                    audio_input_device_name_);
  emit audioInputsChanged();
}

void AppSettings::refreshSdrDevices() {
  // Enumeration asks every vendor module what it can see, which takes long
  // enough to be noticed. Running it here, on the thread that draws, stopped
  // the event loop for its whole duration: the application looked frozen, and
  // no waiting indicator could even animate, because nothing was being
  // painted. It runs on a pooled thread now and the result is applied back
  // here, where the state it touches lives.
  if (sdr_discovery_running_) return;
  sdr_discovery_running_ = true;
  emit sdrDiscoveryRunningChanged();
  emit sdrSettingsChanged();
  QThreadPool::globalInstance()->start([this] {
    SdrReceiver receiver(makeSoapySdrReceiveBackend());
    SdrDiscoveryReport report = receiver.discover();
    // Back to the thread that owns this object before touching anything it
    // owns. Nothing below is safe to run anywhere else.
    QMetaObject::invokeMethod(
        this,
        [this, report = std::move(report)] {
          applySdrDiscoveryReport(report);
          sdr_discovery_running_ = false;
          emit sdrDiscoveryRunningChanged();
          emit sdrSettingsChanged();
          emit settingsChanged();
          // After the report has been applied and the waiting state cleared,
          // so anything acting on the answer reads settled properties rather
          // than a scan that still claims to be running. Does nothing unless a
          // startup restore asked for this enumeration.
          answerSdrStartupRestore(sdr_startup_restore_found_);
        },
        Qt::QueuedConnection);
  });
}

bool AppSettings::sdrDiscoveryRunning() const noexcept {
  return sdr_discovery_running_;
}

void AppSettings::restoreSdrSelectionAtStartup() {
  // Nothing was ever selected in this profile: there is nothing to bring back,
  // no reason to enumerate, and nothing to warn about either, because a
  // receiver that was never chosen cannot have gone missing. A station that
  // only uses sound-card audio pays nothing here.
  if (sdr_device_id_.isEmpty()) return;
  // A request already outstanding will be answered by the enumeration that is
  // running for it. Asking again would only scan twice for one answer.
  if (sdr_startup_restore_pending_) return;

  sdr_startup_restore_pending_ = true;
  sdr_startup_restore_found_ = false;
  // Captured before any report can be applied: applying one overwrites both
  // the saved name and the saved id with whatever was found, and the answer
  // has to name what was saved. The label falls back to the variant id so an
  // unnamed saved receiver is still named by something the operator can act
  // on; the signal itself carries the persisted name exactly as stored.
  sdr_startup_restore_device_name_ = sdr_device_name_;
  sdr_startup_restore_device_label_ =
      sdr_device_name_.isEmpty() ? sdr_device_id_ : sdr_device_name_;

  // With a backend to enumerate with, the answer follows the report; the scan
  // still runs off the thread that draws. Without one, no enumeration could
  // bring the receiver back, so the honest answer is already known and is
  // queued rather than emitted inline -- every caller is then answered the
  // same way, on the event loop, after this call has returned.
  if (kSdrBackendCompiledIn || sdr_backend_available_) refreshSdrDevices();
  else
    QMetaObject::invokeMethod(
        this, [this] { answerSdrStartupRestore(false); },
        Qt::QueuedConnection);
}

void AppSettings::answerSdrStartupRestore(const bool found) {
  if (!sdr_startup_restore_pending_) return;
  sdr_startup_restore_pending_ = false;
  if (!found) {
    setStatusMessage(
        QStringLiteral("Saved SDR receiver \"%1\" was not found. Reception "
                       "remains stopped; sound-card audio and file replay "
                       "remain available.")
            .arg(sdr_startup_restore_device_label_));
  }
  emit sdrSelectionRestored(found, sdr_startup_restore_device_name_);
}

void AppSettings::applySdrDiscoveryReport(const SdrDiscoveryReport& report) {

  const QString previous_variant_id = sdr_device_id_;
  const QString previous_device_name = sdr_device_name_;
  const QString previous_physical_id = sdr_physical_device_id_;
  QString previous_mode_id = sdr_device_mode_id_;
  if (previous_mode_id.isEmpty()) {
    if (previous_device_name.contains(QStringLiteral("Single Tuner"),
                                      Qt::CaseInsensitive)) {
      previous_mode_id = QStringLiteral("ST");
    } else if (previous_device_name.contains(QStringLiteral("Dual Tuner"),
                                             Qt::CaseInsensitive)) {
      previous_mode_id = QStringLiteral("DT");
    } else if (previous_device_name.contains(QStringLiteral("Master"),
                                             Qt::CaseInsensitive) &&
               previous_device_name.contains(QStringLiteral("8Mhz"),
                                             Qt::CaseInsensitive)) {
      previous_mode_id = QStringLiteral("MA8");
    } else if (previous_device_name.contains(QStringLiteral("Master"),
                                             Qt::CaseInsensitive)) {
      previous_mode_id = QStringLiteral("MA");
    } else if (previous_device_name.contains(QStringLiteral("Slave"),
                                             Qt::CaseInsensitive)) {
      previous_mode_id = QStringLiteral("SL");
    }
  }
  QStringList names;
  QStringList ids;
  sdr_discovered_modes_.clear();
  for (const auto& physical : groupSdrDevices(report.devices)) {
    const QString physical_id =
        QString::fromStdString(physical.id).trimmed();
    if (physical_id.isEmpty() || ids.contains(physical_id)) continue;
    QString physical_label = QString::fromStdString(physical.label).trimmed();
    if (physical_label.isEmpty())
      physical_label = QStringLiteral("Unnamed SDR device");
    const auto& first = physical.modes.front();
    const QString driver = QString::fromStdString(first.driver).trimmed();
    const QString serial = QString::fromStdString(first.serial).trimmed();
    if (!driver.isEmpty())
      physical_label += QStringLiteral("  •  %1").arg(driver);
    if (!serial.isEmpty())
      physical_label += QStringLiteral("  •  S/N %1").arg(serial);
    names.push_back(physical_label);
    ids.push_back(physical_id);
    for (const auto& mode : physical.modes) {
      const QString variant_id = QString::fromStdString(mode.id).trimmed();
      if (variant_id.isEmpty()) continue;
      QString mode_name = QString::fromStdString(mode.mode_label).trimmed();
      if (mode_name.isEmpty()) mode_name = QStringLiteral("Default");
      sdr_discovered_modes_.push_back(
          {.physical_id = physical_id,
           .variant_id = variant_id,
           .variant_name = QString::fromStdString(mode.label).trimmed(),
           .mode_id = QString::fromStdString(mode.mode_id).trimmed(),
           .mode_name = mode_name,
           .recommended = mode.recommended_mode});
    }
  }

  sdr_backend_available_ = report.backend_available;
  sdr_backend_version_ = QString::fromStdString(report.backend_version);
  sdr_module_names_.clear();
  for (const auto& module : report.modules) {
    QString name = QDir::cleanPath(QString::fromStdString(module).trimmed());
    const QString canonical_name = QFileInfo(name).canonicalFilePath();
    if (!canonical_name.isEmpty()) name = canonical_name;
    const bool already_listed =
        std::any_of(sdr_module_names_.cbegin(), sdr_module_names_.cend(),
                    [&name](const QString& existing) {
                      return existing.compare(name, Qt::CaseInsensitive) == 0;
                    });
    if (!name.isEmpty() && !already_listed) sdr_module_names_.push_back(name);
  }
  sdr_module_names_.sort(Qt::CaseInsensitive);
  sdr_device_names_ = std::move(names);
  sdr_device_ids_ = std::move(ids);
  sdr_physical_device_id_.clear();
  if (sdr_device_ids_.contains(previous_physical_id)) {
    sdr_physical_device_id_ = previous_physical_id;
  } else {
    for (const auto& mode : sdr_discovered_modes_) {
      const bool exact_variant = mode.variant_id == previous_variant_id;
      const bool legacy_variant =
          !previous_variant_id.isEmpty() &&
          previous_variant_id.startsWith(mode.physical_id + QLatin1Char(':'));
      const QString serial = mode.physical_id.section(QLatin1Char(':'), 1);
      const bool saved_name_match =
          !serial.isEmpty() && previous_device_name.contains(serial);
      if (exact_variant || legacy_variant || saved_name_match) {
        sdr_physical_device_id_ = mode.physical_id;
        break;
      }
    }
  }
  rebuildSdrDeviceModes(previous_variant_id, previous_mode_id);
  sdr_diagnostic_ = QString::fromStdString(report.diagnostic).trimmed();
  if (sdr_diagnostic_.isEmpty()) {
    sdr_diagnostic_ =
        !sdr_backend_available_
            ? QStringLiteral(
                  "SoapySDR is unavailable in this build. Install "
                  "SoapySDR and the receiver module, then install a "
                  "CW Buddy SDR-enabled build.")
            : (sdr_device_ids_.isEmpty()
                   ? QStringLiteral("SoapySDR is ready, but no receiver was "
                                    "found. Connect the SDR, install its Soapy "
                                    "module, then refresh.")
                   : QStringLiteral("SDR discovery complete. Reception remains "
                                    "stopped until explicitly started."));
  }
  const bool reset_receiver_source =
      receiver_input_type_index_ == 1 &&
      (!sdr_backend_available_ || sdrDeviceIndex() < 0);
  if (reset_receiver_source) {
    receiver_input_type_index_ = 0;
  }
  if (sdrDeviceIndex() >= 0) refreshSelectedSdrCapabilities();
  // Recorded here, where the matching above has just decided it, and left for
  // the enumeration that asked for it to report. The physical receiver being
  // present is what counts: rebuildSdrDeviceModes may have settled on another
  // operating mode of it, and that is still the receiver the operator saved.
  if (sdr_startup_restore_pending_)
    sdr_startup_restore_found_ = sdrDeviceIndex() >= 0;
  emit sdrSettingsChanged();
  if (reset_receiver_source) emit receiverInputTypeChanged();
}

void AppSettings::selectSdrDevice(const int index) {
  if (index < 0 || index >= sdr_device_ids_.size()) return;
  sdr_physical_device_id_ = sdr_device_ids_.at(index);
  rebuildSdrDeviceModes();
  refreshSelectedSdrCapabilities();
  setStatusMessage(
      QStringLiteral("SDR receiver selected. This receive-only source remains "
                     "stopped until started by the operator."));
  emit sdrSettingsChanged();
  emit settingsChanged();
}

void AppSettings::selectSdrOperatingMode(const int index) {
  if (index < 0 || index >= sdr_device_mode_ids_.size()) return;
  sdr_device_id_ = sdr_device_mode_ids_.at(index);
  sdr_device_mode_id_ = sdr_device_mode_keys_.at(index);
  const auto mode = std::find_if(
      sdr_discovered_modes_.cbegin(), sdr_discovered_modes_.cend(),
      [this](const SdrModeChoice& candidate) {
        return candidate.variant_id == sdr_device_id_;
      });
  sdr_device_name_ =
      mode == sdr_discovered_modes_.cend() ? QString{} : mode->variant_name;
  refreshSelectedSdrCapabilities();
  setStatusMessage(
      QStringLiteral("SDR operating mode selected. Reception remains stopped "
                     "until started by the operator."));
  emit sdrSettingsChanged();
  emit settingsChanged();
}

void AppSettings::rebuildSdrDeviceModes(const QString& preferred_variant_id,
                                        const QString& preferred_mode_id) {
  sdr_device_mode_names_.clear();
  sdr_device_mode_ids_.clear();
  sdr_device_mode_keys_.clear();
  QList<const SdrModeChoice*> available;
  for (const auto& mode : sdr_discovered_modes_) {
    if (mode.physical_id != sdr_physical_device_id_) continue;
    available.push_back(&mode);
    sdr_device_mode_names_.push_back(mode.mode_name);
    sdr_device_mode_ids_.push_back(mode.variant_id);
    sdr_device_mode_keys_.push_back(mode.mode_id);
  }
  if (available.isEmpty()) {
    sdr_device_id_.clear();
    sdr_device_mode_id_.clear();
    return;
  }
  int selected = -1;
  for (int index = 0; index < available.size(); ++index) {
    if (!preferred_variant_id.isEmpty() &&
        available.at(index)->variant_id == preferred_variant_id) {
      selected = index;
      break;
    }
  }
  const QString desired_mode = preferred_mode_id.isEmpty()
                                   ? sdr_device_mode_id_
                                   : preferred_mode_id;
  if (selected < 0 && !desired_mode.isEmpty()) {
    for (int index = 0; index < available.size(); ++index) {
      if (available.at(index)->mode_id == desired_mode) {
        selected = index;
        break;
      }
    }
  }
  if (selected < 0) {
    for (int index = 0; index < available.size(); ++index) {
      if (available.at(index)->recommended) {
        selected = index;
        break;
      }
    }
  }
  if (selected < 0) selected = 0;
  const auto& mode = *available.at(selected);
  sdr_device_id_ = mode.variant_id;
  sdr_device_mode_id_ = mode.mode_id;
  sdr_device_name_ = mode.variant_name;
}

void AppSettings::refreshSelectedSdrCapabilities() {
  sdr_sample_rate_options_.clear();
  sdr_bandwidth_options_.clear();
  sdr_antenna_names_.clear();
  sdr_automatic_gain_available_ = true;
  sdr_minimum_gain_db_ = -100.0;
  sdr_maximum_gain_db_ = 100.0;
  if (sdr_device_id_.isEmpty()) return;

  SdrReceiver receiver(makeSoapySdrReceiveBackend());
  (void)receiver.discover();
  const SdrDeviceCapabilities capabilities =
      receiver.probe(sdr_device_id_.toStdString());
  const auto append_values = [](const std::vector<double>& source,
                                QVariantList& destination) {
    for (const double value : source) {
      if (!std::isfinite(value) || value < 0.0 || value > 64'000'000.0)
        continue;
      const int rounded = static_cast<int>(std::llround(value));
      if (!destination.contains(rounded)) destination.push_back(rounded);
    }
  };
  append_values(capabilities.sample_rates_hz, sdr_sample_rate_options_);
  sdr_bandwidth_options_.push_back(0);
  append_values(capabilities.bandwidths_hz, sdr_bandwidth_options_);
  for (const auto& antenna : capabilities.antennas) {
    const QString name = QString::fromStdString(antenna).trimmed();
    if (!name.isEmpty() && !sdr_antenna_names_.contains(name))
      sdr_antenna_names_.push_back(name);
  }
  if (!sdr_antenna_names_.contains(sdr_antenna_)) {
    sdr_antenna_ = sdr_antenna_names_.isEmpty()
                       ? QString{}
                       : sdr_antenna_names_.constFirst();
  }
  sdr_automatic_gain_available_ = capabilities.automatic_gain_available;
  if (std::isfinite(capabilities.minimum_gain_db) &&
      std::isfinite(capabilities.maximum_gain_db) &&
      capabilities.minimum_gain_db <= capabilities.maximum_gain_db) {
    sdr_minimum_gain_db_ = capabilities.minimum_gain_db;
    sdr_maximum_gain_db_ = capabilities.maximum_gain_db;
    sdr_gain_db_ =
        std::clamp(sdr_gain_db_, sdr_minimum_gain_db_, sdr_maximum_gain_db_);
  }
  if (!sdr_automatic_gain_available_ && sdr_automatic_gain_)
    sdr_automatic_gain_ = false;
  const QString diagnostic =
      QString::fromStdString(capabilities.diagnostic).trimmed();
  if (!diagnostic.isEmpty()) sdr_diagnostic_ = diagnostic;
}

void AppSettings::selectSdrAntenna(const int index) {
  if (index < 0 || index >= sdr_antenna_names_.size()) return;
  if (assign_if_changed(sdr_antenna_, sdr_antenna_names_.at(index))) {
    emit sdrSettingsChanged();
    emit settingsChanged();
  }
}

void AppSettings::refreshAudioOutputs() {
  QStringList names{QStringLiteral("System default output (recommended)")};
  QStringList ids{QString{}};
  for (const auto& device : QMediaDevices::audioOutputs()) {
    const QString id = QString::fromLatin1(device.id().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    if (id.isEmpty() || ids.contains(id)) continue;
    QString name = device.description().trimmed();
    if (name.isEmpty())
      name = QStringLiteral("Audio output %1").arg(ids.size());
    if (device.isDefault()) name += QStringLiteral(" (current default)");
    names.push_back(name);
    ids.push_back(id);
  }
  if (!audio_output_id_.isEmpty() && !ids.contains(audio_output_id_)) {
    const QString unavailable_name =
        audio_output_name_.isEmpty()
            ? QStringLiteral("Previously selected output")
            : audio_output_name_;
    names.push_back(unavailable_name + QStringLiteral(" (unavailable)"));
    ids.push_back(audio_output_id_);
  }
  const int selected_index = ids.indexOf(audio_output_id_);
  if (selected_index > 0) {
    audio_output_name_ = names.at(selected_index);
    if (audio_output_name_.endsWith(QStringLiteral(" (unavailable)")))
      audio_output_name_.chop(QStringLiteral(" (unavailable)").size());
  }
  if (names != audio_output_names_ || ids != audio_output_ids_) {
    audio_output_names_ = std::move(names);
    audio_output_ids_ = std::move(ids);
    emit audioOutputsChanged();
  }
}

void AppSettings::selectAudioOutput(const int index) {
  if (index < 0 || index >= audio_output_ids_.size()) return;
  audio_output_id_ = audio_output_ids_.at(index);
  audio_output_name_ = index == 0 ? QStringLiteral("System default output")
                                  : audio_output_names_.at(index);
  if (audio_output_name_.endsWith(QStringLiteral(" (unavailable)")))
    audio_output_name_.chop(QStringLiteral(" (unavailable)").size());
  setStatusMessage(
      QStringLiteral("Monitor output selected. Monitoring remains "
                     "off until the operator enables it."));
  emit audioOutputsChanged();
  emit settingsChanged();
}

void AppSettings::refreshDetectedRadios() {
  QStringList names;
  QList<int> detected_slots;
#ifdef Q_OS_WIN
  if (ensureOmniRigAutomation()) {
    auto* automation = static_cast<IDispatch*>(omnirig_automation_);
    for (int slot = 1; slot <= 2; ++slot) {
      VARIANT rig_value;
      const auto rig_property = slot == 1 ? L"Rig1" : L"Rig2";
      if (!automation_property(automation, rig_property, &rig_value)) {
        continue;
      }
      IDispatch* rig =
          rig_value.vt == VT_DISPATCH ? rig_value.pdispVal : nullptr;
      if (rig == nullptr) {
        VariantClear(&rig_value);
        continue;
      }

      VARIANT type_value;
      VARIANT status_value;
      const bool has_type = automation_property(rig, L"RigType", &type_value);
      const bool has_status =
          automation_property(rig, L"Status", &status_value);
      const QString rig_type =
          has_type && type_value.vt == VT_BSTR
              ? QString::fromWCharArray(type_value.bstrVal).trimmed()
              : QString{};
      const long status =
          has_status && (status_value.vt == VT_I4 || status_value.vt == VT_INT)
              ? status_value.lVal
              : -1;
      if (!rig_type.isEmpty() && status == kOmniRigOnlineStatus) {
        names.push_back(
            QStringLiteral("OmniRig %1 — %2").arg(slot).arg(rig_type));
        detected_slots.push_back(slot);
      }
      if (has_type) {
        VariantClear(&type_value);
      }
      if (has_status) {
        VariantClear(&status_value);
      }
      VariantClear(&rig_value);
    }
  }
#endif
  const bool changed =
      names != detected_radio_names_ || detected_slots != detected_radio_slots_;
  detected_radio_names_ = std::move(names);
  detected_radio_slots_ = std::move(detected_slots);
  if (changed) {
    emit detectedRadiosChanged();
    emit settingsChanged();
  }
  setStatusMessage(
      detected_radio_names_.isEmpty()
          ? QStringLiteral("No positively identified online radio was found. "
                           "SWL and manual setup remain available.")
          : QStringLiteral("Online radios refreshed without probing arbitrary "
                           "serial ports."));
}

void AppSettings::selectDetectedRadio(const int index) {
  if (index < 0 || index >= detected_radio_slots_.size()) {
    return;
  }
  radio_enabled_ = true;
  frequency_backend_index_ = 0;
  omnirig_slot_ = detected_radio_slots_.at(index);
  setStatusMessage(
      QStringLiteral("Detected radio selected. Transmit remains disarmed."));
  emit settingsChanged();
}

bool AppSettings::apply() {
  receiver_input_type_index_ = std::clamp(receiver_input_type_index_, 0, 1);
  if (!sdr_backend_available_ || sdrDeviceIndex() < 0)
    receiver_input_type_index_ = 0;
  // Clamped, but never forced: an SDR preference outlives a receiver that is
  // absent right now, because discovery may not have run yet and the operator
  // has not changed their mind. What the application starts is decided from
  // the restore answer, not from this value alone.
  preferred_source_mode_ = std::clamp(preferred_source_mode_, 0, 2);
  sdr_center_frequency_hz_ =
      std::clamp<qulonglong>(sdr_center_frequency_hz_, 1ULL, 99'000'000'000ULL);
  sdr_sample_rate_hz_ = std::clamp(sdr_sample_rate_hz_, 25'000, 64'000'000);
  sdr_bandwidth_hz_ = std::clamp(sdr_bandwidth_hz_, 0, 64'000'000);
  sdr_decoder_bandwidth_hz_ =
      std::clamp(sdr_decoder_bandwidth_hz_, kMinimumSdrDecoderBandwidthHz,
                 kMaximumSdrDecoderBandwidthHz);
  sdr_decoder_center_frequency_hz_ = std::clamp<qulonglong>(
      sdr_decoder_center_frequency_hz_, 1ULL, 99'000'000'000ULL);
  sdr_radio_lo_offset_hz_ =
      std::clamp<qint64>(sdr_radio_lo_offset_hz_, -10'000'000LL, 10'000'000LL);
  sdr_gain_db_ = std::clamp(sdr_gain_db_, -100.0, 100.0);
  audio_gain_db_ = std::clamp(audio_gain_db_, -40.0, 40.0);
  audio_automatic_gain_target_dbfs_ =
      std::clamp(audio_automatic_gain_target_dbfs_, -40.0, -1.0);
  audio_lower_frequency_hz_ =
      std::clamp(audio_lower_frequency_hz_, 0.0, 96'000.0);
  audio_upper_frequency_hz_ =
      std::clamp(audio_upper_frequency_hz_, 50.0, 96'000.0);
  if (audio_upper_frequency_hz_ - audio_lower_frequency_hz_ < 50.0) {
    audio_upper_frequency_hz_ =
        std::min(96'000.0, audio_lower_frequency_hz_ + 50.0);
    audio_lower_frequency_hz_ =
        std::min(audio_lower_frequency_hz_, audio_upper_frequency_hz_ - 50.0);
  }
  frequency_backend_index_ = std::clamp(frequency_backend_index_, 0, 2);
  radio_tuning_step_hz_ = std::clamp(radio_tuning_step_hz_, 1'000, 100'000);
  omnirig_slot_ = std::clamp(omnirig_slot_, 1, 2);
  hamlib_host_ = hamlib_host_.trimmed();
  hamlib_port_ = std::clamp(hamlib_port_, 1, 65'535);
  hamlib_rx_vfo_ = hamlib_rx_vfo_.trimmed().toUpper();
  hamlib_tx_vfo_ = hamlib_tx_vfo_.trimmed().toUpper();
  cat_baud_rate_ =
      static_cast<int>(cwassistant::core::nearest_supported_serial_baud_rate(
          static_cast<std::uint32_t>(std::max(cat_baud_rate_, 0))));
  cat_data_bits_ = std::clamp(cat_data_bits_, 5, 8);
  cat_parity_index_ = std::clamp(cat_parity_index_, 0, 2);
  cat_stop_bits_ = std::clamp(cat_stop_bits_, 1, 2);
  cat_flow_control_index_ = std::clamp(cat_flow_control_index_, 0, 1);
  poll_interval_ms_ = std::clamp(poll_interval_ms_, 50, 10'000);
  timeout_ms_ = std::clamp(timeout_ms_, 100, 60'000);
  tx_macro_1_ = tx_macro_1_.simplified().toUpper().left(64);
  tx_macro_2_ = tx_macro_2_.simplified().toUpper().left(64);
  tx_macro_3_ = tx_macro_3_.simplified().toUpper().left(64);
  tx_macro_4_ = tx_macro_4_.simplified().toUpper().left(64);
  target_fps_ = std::clamp(target_fps_, 10, 120);
  waterfall_rate_ = std::clamp(waterfall_rate_, 1, 120);
  waterfall_time_span_seconds_ =
      std::clamp(waterfall_time_span_seconds_, 5, 30);
  spectrum_display_mode_ = std::clamp(spectrum_display_mode_, 0, 1);
  averaging_frames_ = std::clamp(averaging_frames_, 1, 32);
  automatic_range_span_db_ = std::clamp(automatic_range_span_db_, 30.0, 100.0);
  waterfall_noise_margin_db_ =
      std::clamp(waterfall_noise_margin_db_, 0.0, 30.0);
  cw_guide_center_hz_ = std::clamp(cw_guide_center_hz_, 0.0, 96'000.0);
  cw_guide_width_hz_ = std::clamp(cw_guide_width_hz_, 10.0, 5'000.0);
  decoded_signal_timeout_seconds_ =
      std::clamp(decoded_signal_timeout_seconds_, 5, 300);
  // Stored to exactly the precision the settings page can present. The
  // operator sets the threshold in tenths of a decibel, and a stored value
  // carrying more precision than that would read back as a different number
  // from the one that was just set.
  minimum_decode_snr_db_ =
      std::round(std::clamp(minimum_decode_snr_db_, 0.0, 40.0) * 10.0) / 10.0;
  dx_spots_retention_minutes_ = std::clamp(dx_spots_retention_minutes_, 1, 60);
  dx_spots_tolerance_hz_ = std::clamp(dx_spots_tolerance_hz_, 50, 1'000);
  dx_cluster_custom_host_ =
      dx_cluster_custom_host_.trimmed().left(kMaximumDxClusterHostLength);
  dx_cluster_custom_port_ = std::clamp(dx_cluster_custom_port_, 1, 65'535);
  dx_cluster_login_ssid_ = std::clamp(dx_cluster_login_ssid_, 0, 99);
  {
    const int server_count = static_cast<int>(dx_cluster_servers_.size());
    if (dx_cluster_server_index_ != kDxClusterCustomServerIndex &&
        (dx_cluster_server_index_ < 0 ||
         dx_cluster_server_index_ >= server_count)) {
      dx_cluster_server_index_ =
          server_count > 0 ? 0 : kDxClusterCustomServerIndex;
    }
  }
  // The link has no identity of its own. Without a station callsign there is
  // nothing to log in as, so the saved state is off rather than a connection
  // that would be attempted and refused on every start.
  if (own_callsign_.isEmpty()) dx_cluster_enabled_ = false;
  diagnostics_server_port_ =
      std::clamp(diagnostics_server_port_, kMinimumDiagnosticsServerPort,
                 65'535);
  diagnostics_server_token_ =
      diagnostics_server_token_.trimmed().left(kMaximumDiagnosticsTokenLength);
  diagnostics_server_addresses_ =
      sanitize_diagnostics_addresses(diagnostics_server_addresses_);
  diagnostics_allowed_peers_ =
      sanitize_diagnostics_peers(diagnostics_allowed_peers_);
  // The saved state may not describe an exposure the enable itself would have
  // refused. A settings file edited by hand, or a token cleared by a route
  // that did not run the guard, must not come back on the next start as a
  // station published to a routable address with nothing in front of it.
  if (diagnostics_server_enabled_ && diagnosticsServerExposureUnguarded()) {
    diagnostics_server_enabled_ = false;
  }
  if (upper_bound_db_ - lower_bound_db_ < 10.0) {
    upper_bound_db_ = lower_bound_db_ + 10.0;
  }

  if (radio_enabled_ && ptt_line_index_ == key_line_index_) {
    setStatusMessage(
        QStringLiteral("PTT and KEY must use different COM control lines."));
    emit settingsChanged();
    return false;
  }

  QSettings settings;
  settings.setValue(storageKey(QStringLiteral("configuration/schemaVersion")),
                    kSchemaVersion);
  settings.setValue(storageKey(QStringLiteral("configuration/displayName")),
                    profile_name_);
  settings.setValue(storageKey(QStringLiteral("audio/inputId")),
                    audio_input_id_);
  settings.setValue(storageKey(QStringLiteral("audio/inputName")),
                    audio_input_name_);
  settings.setValue(storageKey(QStringLiteral("audio/inputDeviceName")),
                    audio_input_device_name_);
  settings.setValue(storageKey(QStringLiteral("audio/outputId")),
                    audio_output_id_);
  settings.setValue(storageKey(QStringLiteral("audio/outputName")),
                    audio_output_name_);
  settings.setValue(storageKey(QStringLiteral("audio/dcRejection")),
                    audio_dc_rejection_);
  settings.setValue(storageKey(QStringLiteral("audio/automaticGain")),
                    audio_automatic_gain_);
  settings.setValue(storageKey(QStringLiteral("audio/gainDb")), audio_gain_db_);
  settings.setValue(storageKey(QStringLiteral("audio/automaticGainTargetDbfs")),
                    audio_automatic_gain_target_dbfs_);
  settings.setValue(storageKey(QStringLiteral("audio/automaticBandwidth")),
                    audio_automatic_bandwidth_);
  settings.setValue(storageKey(QStringLiteral("audio/lowerFrequencyHz")),
                    audio_lower_frequency_hz_);
  settings.setValue(storageKey(QStringLiteral("audio/upperFrequencyHz")),
                    audio_upper_frequency_hz_);
  settings.setValue(storageKey(QStringLiteral("audio/inputRadioLinked")),
                    audio_input_radio_linked_);
  settings.setValue(storageKey(QStringLiteral("receiver/inputType")),
                    receiver_input_type_index_);
  settings.setValue(storageKey(QStringLiteral("source/preferredMode")),
                    preferred_source_mode_);
  settings.setValue(storageKey(QStringLiteral("sdr/physicalDeviceId")),
                    sdr_physical_device_id_);
  settings.setValue(storageKey(QStringLiteral("sdr/deviceMode")),
                    sdr_device_mode_id_);
  settings.setValue(storageKey(QStringLiteral("sdr/deviceId")), sdr_device_id_);
  settings.setValue(storageKey(QStringLiteral("sdr/deviceName")),
                    sdr_device_name_);
  settings.setValue(storageKey(QStringLiteral("sdr/centerFrequencyHz")),
                    QVariant::fromValue(sdr_center_frequency_hz_));
  settings.setValue(storageKey(QStringLiteral("sdr/sampleRateHz")),
                    sdr_sample_rate_hz_);
  settings.setValue(storageKey(QStringLiteral("sdr/bandwidthHz")),
                    sdr_bandwidth_hz_);
  settings.setValue(storageKey(QStringLiteral("sdr/antenna")), sdr_antenna_);
  settings.setValue(storageKey(QStringLiteral("sdr/decoderCenterFrequencyHz")),
                    QVariant::fromValue(sdr_decoder_center_frequency_hz_));
  settings.setValue(storageKey(QStringLiteral("sdr/decoderBandwidthHz")),
                    sdr_decoder_bandwidth_hz_);
  settings.setValue(storageKey(QStringLiteral("sdr/followRadioVfo")),
                    sdr_follow_radio_vfo_);
  settings.setValue(storageKey(QStringLiteral("sdr/tuningStepHz")),
                    sdr_tuning_step_hz_);
  settings.setValue(storageKey(QStringLiteral("sdr/radioLoOffsetHz")),
                    sdr_radio_lo_offset_hz_);
  settings.setValue(storageKey(QStringLiteral("sdr/automaticGain")),
                    sdr_automatic_gain_);
  settings.setValue(storageKey(QStringLiteral("sdr/gainDb")), sdr_gain_db_);
  settings.setValue(storageKey(QStringLiteral("station/ownCallsign")),
                    own_callsign_);
  settings.setValue(storageKey(QStringLiteral("radio/referenceRigIndex")),
                    reference_rig_index_);
  settings.setValue(storageKey(QStringLiteral("radio/enabled")),
                    radio_enabled_);
  settings.setValue(storageKey(QStringLiteral("radio/frequencyBackendIndex")),
                    frequency_backend_index_);
  settings.setValue(storageKey(QStringLiteral("radio/tuningStepHz")),
                    radio_tuning_step_hz_);
  settings.setValue(storageKey(QStringLiteral("radio/txModeTarget")),
                    radioTxModeTarget());
  settings.setValue(storageKey(QStringLiteral("radio/omniRigSlot")),
                    omnirig_slot_);
  settings.setValue(storageKey(QStringLiteral("radio/hamlibHost")),
                    hamlib_host_);
  settings.setValue(storageKey(QStringLiteral("radio/hamlibPort")),
                    hamlib_port_);
  settings.setValue(storageKey(QStringLiteral("radio/hamlibRxVfo")),
                    hamlib_rx_vfo_);
  settings.setValue(storageKey(QStringLiteral("radio/hamlibTxVfo")),
                    hamlib_tx_vfo_);
  settings.setValue(storageKey(QStringLiteral("radio/hamlibWritable")),
                    hamlib_writable_);
  settings.setValue(storageKey(QStringLiteral("radio/cat4omUrl")),
                    cat4om_url_.trimmed());
  settings.setValue(storageKey(QStringLiteral("radio/cat4omRadioId")),
                    cat4om_radio_id_.trimmed());
  settings.setValue(storageKey(QStringLiteral("radio/catPort")),
                    cat_port_.trimmed());
  settings.setValue(storageKey(QStringLiteral("radio/catBaudRate")),
                    cat_baud_rate_);
  settings.setValue(storageKey(QStringLiteral("radio/catDataBits")),
                    cat_data_bits_);
  settings.setValue(storageKey(QStringLiteral("radio/catParityIndex")),
                    cat_parity_index_);
  settings.setValue(storageKey(QStringLiteral("radio/catStopBits")),
                    cat_stop_bits_);
  settings.setValue(storageKey(QStringLiteral("radio/catFlowControlIndex")),
                    cat_flow_control_index_);
  settings.setValue(storageKey(QStringLiteral("radio/pollIntervalMs")),
                    poll_interval_ms_);
  settings.setValue(storageKey(QStringLiteral("radio/timeoutMs")), timeout_ms_);
  settings.setValue(storageKey(QStringLiteral("radio/splitEnabled")),
                    split_enabled_);
  settings.setValue(storageKey(QStringLiteral("radio/rxTransverterOffsetHz")),
                    rx_transverter_offset_hz_);
  settings.setValue(storageKey(QStringLiteral("radio/txTransverterOffsetHz")),
                    tx_transverter_offset_hz_);
  settings.setValue(storageKey(QStringLiteral("radio/cwToneSidebandIndex")),
                    cw_tone_sideband_index_);
  settings.setValue(storageKey(QStringLiteral("keying/port")),
                    keying_port_.trimmed());
  settings.setValue(storageKey(QStringLiteral("keying/directEnabled")),
                    direct_keying_enabled_);
  settings.remove(storageKey(QStringLiteral("keying/directValidated")));
  settings.setValue(
      storageKey(QStringLiteral("keying/acceptanceConfigurationSha256")),
      direct_keying_acceptance_sha256_);
  settings.setValue(storageKey(QStringLiteral("keying/acceptancePlatform")),
                    direct_keying_acceptance_platform_);
  settings.setValue(storageKey(QStringLiteral("keying/acceptanceUtcSeconds")),
                    direct_keying_acceptance_utc_seconds_);
  settings.setValue(storageKey(QStringLiteral("keying/pttLineIndex")),
                    ptt_line_index_);
  settings.setValue(storageKey(QStringLiteral("keying/keyLineIndex")),
                    key_line_index_);
  settings.setValue(storageKey(QStringLiteral("keying/pttActiveHigh")),
                    ptt_active_high_);
  settings.setValue(storageKey(QStringLiteral("keying/keyActiveHigh")),
                    key_active_high_);
  settings.setValue(storageKey(QStringLiteral("keying/txSpeedMode")),
                    tx_speed_mode_);
  settings.setValue(storageKey(QStringLiteral("keying/fixedTxWpm")),
                    fixed_tx_wpm_);
  settings.setValue(storageKey(QStringLiteral("keying/txMacro1")), tx_macro_1_);
  settings.setValue(storageKey(QStringLiteral("keying/txMacro2")), tx_macro_2_);
  settings.setValue(storageKey(QStringLiteral("keying/txMacro3")), tx_macro_3_);
  settings.setValue(storageKey(QStringLiteral("keying/txMacro4")), tx_macro_4_);
  settings.setValue(storageKey(QStringLiteral("display/targetFps")),
                    target_fps_);
  settings.setValue(storageKey(QStringLiteral("display/waterfallRate")),
                    waterfall_rate_);
  settings.setValue(
      storageKey(QStringLiteral("display/waterfallTimeSpanSeconds")),
      waterfall_time_span_seconds_);
  settings.setValue(storageKey(QStringLiteral("display/spectrumDisplayMode")),
                    spectrum_display_mode_);
  settings.setValue(storageKey(QStringLiteral("display/automaticRange")),
                    automatic_range_);
  settings.setValue(storageKey(QStringLiteral("display/lowerBoundDb")),
                    lower_bound_db_);
  settings.setValue(storageKey(QStringLiteral("display/upperBoundDb")),
                    upper_bound_db_);
  settings.setValue(storageKey(QStringLiteral("display/automaticRangeSpanDb")),
                    automatic_range_span_db_);
  settings.setValue(
      storageKey(QStringLiteral("display/waterfallNoiseSuppression")),
      waterfall_noise_suppression_);
  settings.setValue(
      storageKey(QStringLiteral("display/waterfallNoiseMarginDb")),
      waterfall_noise_margin_db_);
  settings.setValue(storageKey(QStringLiteral("display/showCwGuide")),
                    show_cw_guide_);
  settings.setValue(storageKey(QStringLiteral("display/cwGuideCenterHz")),
                    cw_guide_center_hz_);
  settings.setValue(storageKey(QStringLiteral("display/cwGuideWidthHz")),
                    cw_guide_width_hz_);
  settings.setValue(storageKey(QStringLiteral("display/averagingFrames")),
                    averaging_frames_);
  settings.setValue(storageKey(QStringLiteral("display/showGrid")), show_grid_);
  settings.setValue(
      storageKey(QStringLiteral("display/waterfallRenderingEnabled")),
      waterfall_rendering_enabled_);
  settings.setValue(
      storageKey(QStringLiteral("display/showSpectrumGestureHints")),
      show_spectrum_gesture_hints_);
  settings.setValue(
      storageKey(QStringLiteral("display/decodedSignalTimeoutSeconds")),
      decoded_signal_timeout_seconds_);
  settings.setValue(storageKey(QStringLiteral("decoder/decodeWeakSignals")),
                    decode_weak_signals_);
  settings.setValue(storageKey(QStringLiteral("decoder/minimumDecodeSnrDb")),
                    minimum_decode_snr_db_);
  settings.setValue(storageKey(QStringLiteral("decoder/localEnabled")),
                    local_decoder_enabled_);
  settings.setValue(
      storageKey(QStringLiteral("decoder/callsignDatabaseCorrection")),
      callsign_database_correction_enabled_);
  settings.setValue(storageKey(QStringLiteral("decoder/keyingModel")),
                    keying_model_);
  settings.setValue(
      storageKey(QStringLiteral("diagnostics/debugCaptureMaximumSeconds")),
      debug_capture_maximum_seconds_);
  settings.setValue(storageKey(QStringLiteral("station/operatorRole")),
                    operator_role_);
  settings.setValue(storageKey(QStringLiteral("decoder/localModelPath")),
                    local_decoder_model_path_);
  settings.setValue(storageKey(QStringLiteral("decoder/localMetadataPath")),
                    local_decoder_metadata_path_);
  settings.setValue(
      storageKey(QStringLiteral("decoder/localCallsignDatabaseEnabled")),
      local_callsign_database_enabled_);
  settings.setValue(
      storageKey(QStringLiteral("decoder/localCallsignDatabasePath")),
      local_callsign_database_path_);
  // The keys the deleted HTTPS spot provider owned are removed rather than
  // merely no longer written, so a settings file from an earlier build cannot
  // keep describing a feed that no longer exists.
  settings.remove(storageKey(QStringLiteral("dxSpots/enabled")));
  settings.remove(storageKey(QStringLiteral("dxSpots/reverseBeacon")));
  settings.remove(storageKey(QStringLiteral("dxSpots/cluster")));
  settings.remove(storageKey(QStringLiteral("dxSpots/endpoint")));
  settings.remove(storageKey(QStringLiteral("dxSpots/refreshSeconds")));
  settings.setValue(storageKey(QStringLiteral("dxSpots/retentionMinutes")),
                    dx_spots_retention_minutes_);
  settings.setValue(storageKey(QStringLiteral("dxSpots/toleranceHz")),
                    dx_spots_tolerance_hz_);
  settings.setValue(storageKey(QStringLiteral("dxSpots/showLabels")),
                    dx_spots_show_labels_);
  settings.setValue(storageKey(QStringLiteral("dxcluster/enabled")),
                    dx_cluster_enabled_);
  settings.setValue(storageKey(QStringLiteral("dxcluster/serverIndex")),
                    dx_cluster_server_index_);
  settings.setValue(storageKey(QStringLiteral("dxcluster/customHost")),
                    dx_cluster_custom_host_);
  settings.setValue(storageKey(QStringLiteral("dxcluster/customPort")),
                    dx_cluster_custom_port_);
  settings.setValue(storageKey(QStringLiteral("dxcluster/loginSsid")),
                    dx_cluster_login_ssid_);
  settings.setValue(storageKey(QStringLiteral("diagnostics/serverEnabled")),
                    diagnostics_server_enabled_);
  settings.setValue(storageKey(QStringLiteral("diagnostics/serverPort")),
                    diagnostics_server_port_);
  settings.setValue(storageKey(QStringLiteral("diagnostics/serverToken")),
                    diagnostics_server_token_);
  settings.setValue(storageKey(QStringLiteral("diagnostics/serverAddresses")),
                    diagnostics_server_addresses_);
  settings.setValue(storageKey(QStringLiteral("diagnostics/allowedPeers")),
                    diagnostics_allowed_peers_);
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    setStatusMessage(QStringLiteral("Settings could not be written."));
    return false;
  }
  emit settingsChanged();
  emit localDecoderConfigurationCommitted(local_decoder_enabled_,
                                          local_decoder_model_path_,
                                          local_decoder_metadata_path_);
  setStatusMessage(
      QStringLiteral("Settings saved. Transmit remains disarmed."));
  refreshProfiles();
  return true;
}

void AppSettings::load() {
  QSettings settings;
  setup_complete_ =
      settings
          .value(storageKey(QStringLiteral("configuration/setupComplete")),
                 false)
          .toBool();
  audio_input_id_ =
      settings.value(storageKey(QStringLiteral("audio/inputId"))).toString();
  audio_input_name_ = settings
                          .value(storageKey(QStringLiteral("audio/inputName")),
                                 QStringLiteral("System default input"))
                          .toString();
  audio_input_device_name_ =
      settings.value(storageKey(QStringLiteral("audio/inputDeviceName")))
          .toString();
  if (audio_input_device_name_.isEmpty() && !audio_input_id_.isEmpty()) {
    // A profile written before the raw name was stored separately. The row
    // text is the only record of the device there, so strip the decorations
    // back off it rather than leaving the station with no name to recover by
    // -- which is exactly the position that made this worth fixing.
    audio_input_device_name_ = audio_input_name_;
    static const QStringList decorations{
        QStringLiteral(" (current default)"), QStringLiteral(" (unavailable)"),
        QStringLiteral(" (recommended)")};
    for (const auto& decoration : decorations) {
      if (audio_input_device_name_.endsWith(decoration)) {
        audio_input_device_name_.chop(decoration.size());
      }
    }
    audio_input_device_name_ = audio_input_device_name_.trimmed();
  }
  audio_output_id_ =
      settings.value(storageKey(QStringLiteral("audio/outputId"))).toString();
  audio_output_name_ =
      settings
          .value(storageKey(QStringLiteral("audio/outputName")),
                 QStringLiteral("System default output"))
          .toString();
  audio_dc_rejection_ =
      settings.value(storageKey(QStringLiteral("audio/dcRejection")), true)
          .toBool();
  audio_automatic_gain_ =
      settings.value(storageKey(QStringLiteral("audio/automaticGain")), false)
          .toBool();
  audio_gain_db_ =
      settings.value(storageKey(QStringLiteral("audio/gainDb")), 0.0)
          .toDouble();
  audio_automatic_gain_target_dbfs_ =
      settings
          .value(storageKey(QStringLiteral("audio/automaticGainTargetDbfs")),
                 -12.0)
          .toDouble();
  audio_automatic_bandwidth_ =
      settings
          .value(storageKey(QStringLiteral("audio/automaticBandwidth")), true)
          .toBool();
  audio_lower_frequency_hz_ =
      settings
          .value(storageKey(QStringLiteral("audio/lowerFrequencyHz")), 100.0)
          .toDouble();
  audio_upper_frequency_hz_ =
      settings
          .value(storageKey(QStringLiteral("audio/upperFrequencyHz")), 3'000.0)
          .toDouble();
  audio_input_radio_linked_ =
      settings
          .value(storageKey(QStringLiteral("audio/inputRadioLinked")), false)
          .toBool();
  receiver_input_type_index_ = std::clamp(
      settings.value(storageKey(QStringLiteral("receiver/inputType")), 0)
          .toInt(),
      0, 1);
  preferred_source_mode_ = std::clamp(
      settings.value(storageKey(QStringLiteral("source/preferredMode")), 0)
          .toInt(),
      0, 2);
  sdr_physical_device_id_ =
      settings.value(storageKey(QStringLiteral("sdr/physicalDeviceId")))
          .toString();
  sdr_device_mode_id_ =
      settings.value(storageKey(QStringLiteral("sdr/deviceMode"))).toString();
  sdr_device_id_ =
      settings.value(storageKey(QStringLiteral("sdr/deviceId"))).toString();
  sdr_device_name_ =
      settings.value(storageKey(QStringLiteral("sdr/deviceName"))).toString();
  sdr_center_frequency_hz_ = std::clamp<qulonglong>(
      settings
          .value(storageKey(QStringLiteral("sdr/centerFrequencyHz")),
                 QVariant::fromValue<qulonglong>(14'050'000ULL))
          .toULongLong(),
      1ULL, 99'000'000'000ULL);
  sdr_sample_rate_hz_ = std::clamp(
      settings.value(storageKey(QStringLiteral("sdr/sampleRateHz")), 250'000)
          .toInt(),
      25'000, 64'000'000);
  sdr_bandwidth_hz_ = std::clamp(
      settings.value(storageKey(QStringLiteral("sdr/bandwidthHz")), 0).toInt(),
      0, 64'000'000);
  sdr_antenna_ =
      settings.value(storageKey(QStringLiteral("sdr/antenna"))).toString();
  sdr_decoder_center_frequency_hz_ = std::clamp<qulonglong>(
      settings
          .value(storageKey(QStringLiteral("sdr/decoderCenterFrequencyHz")),
                 QVariant::fromValue<qulonglong>(sdr_center_frequency_hz_))
          .toULongLong(),
      1ULL, 99'000'000'000ULL);
  // A region saved by an earlier version could be wider than an operator can
  // now listen to, so it is narrowed on the way in rather than left to
  // surprise them later.
  sdr_decoder_bandwidth_hz_ = std::clamp(
      settings
          .value(storageKey(QStringLiteral("sdr/decoderBandwidthHz")), 24'000)
          .toInt(),
      kMinimumSdrDecoderBandwidthHz, kMaximumSdrDecoderBandwidthHz);
  sdr_follow_radio_vfo_ =
      settings.value(storageKey(QStringLiteral("sdr/followRadioVfo")), false)
          .toBool();
  sdr_tuning_step_hz_ = std::clamp(
      settings.value(storageKey(QStringLiteral("sdr/tuningStepHz")), 1'000)
          .toInt(),
      1, 10'000'000);
  sdr_radio_lo_offset_hz_ = std::clamp<qint64>(
      settings.value(storageKey(QStringLiteral("sdr/radioLoOffsetHz")), 0)
          .toLongLong(),
      -10'000'000LL, 10'000'000LL);
  sdr_automatic_gain_ =
      settings.value(storageKey(QStringLiteral("sdr/automaticGain")), true)
          .toBool();
  sdr_gain_db_ = std::clamp(
      settings.value(storageKey(QStringLiteral("sdr/gainDb")), 30.0).toDouble(),
      -100.0, 100.0);
  own_callsign_ =
      settings.value(storageKey(QStringLiteral("station/ownCallsign")))
          .toString();
  radio_enabled_ =
      settings
          .value(storageKey(QStringLiteral("radio/enabled")), setup_complete_)
          .toBool();
  const int saved_index =
      settings.value(storageKey(QStringLiteral("radio/referenceRigIndex")), 0)
          .toInt();
  const auto profile_count =
      static_cast<int>(cwassistant::core::reference_rig_profiles().size());
  reference_rig_index_ =
      std::clamp(saved_index, 0, std::max(0, profile_count - 1));
  applyReferenceDefaults(reference_rig_index_);
  frequency_backend_index_ =
      settings
          .value(storageKey(QStringLiteral("radio/frequencyBackendIndex")), 0)
          .toInt();
  radio_tuning_step_hz_ = std::clamp(
      settings.value(storageKey(QStringLiteral("radio/tuningStepHz")), 1'000)
          .toInt(),
      1'000, 100'000);
  radio_tx_mode_target_ = cwassistant::core::radio_mode_from_token(
      settings
          .value(storageKey(QStringLiteral("radio/txModeTarget")),
                 QStringLiteral("CW"))
          .toString()
          .toStdString());
  if (!cwassistant::core::radio_tx_mode_target_is_valid(
          radio_tx_mode_target_)) {
    radio_tx_mode_target_ = cwassistant::core::RadioMode::Cw;
  }
  omnirig_slot_ =
      settings.value(storageKey(QStringLiteral("radio/omniRigSlot")), 1)
          .toInt();
  hamlib_host_ = settings
                     .value(storageKey(QStringLiteral("radio/hamlibHost")),
                            QStringLiteral("127.0.0.1"))
                     .toString();
  hamlib_port_ =
      settings.value(storageKey(QStringLiteral("radio/hamlibPort")), 4'532)
          .toInt();
  hamlib_rx_vfo_ = settings
                       .value(storageKey(QStringLiteral("radio/hamlibRxVfo")),
                              QStringLiteral("VFOA"))
                       .toString();
  hamlib_tx_vfo_ = settings
                       .value(storageKey(QStringLiteral("radio/hamlibTxVfo")),
                              QStringLiteral("VFOB"))
                       .toString();
  hamlib_writable_ =
      settings.value(storageKey(QStringLiteral("radio/hamlibWritable")), false)
          .toBool();
  cat4om_url_ = settings
                    .value(storageKey(QStringLiteral("radio/cat4omUrl")),
                           QStringLiteral("ws://127.0.0.1:5001/"))
                    .toString();
  cat4om_radio_id_ =
      settings.value(storageKey(QStringLiteral("radio/cat4omRadioId")))
          .toString();
  cat4om_password_.clear();
  cat_port_ =
      settings.value(storageKey(QStringLiteral("radio/catPort"))).toString();
  cat_baud_rate_ = settings
                       .value(storageKey(QStringLiteral("radio/catBaudRate")),
                              cat_baud_rate_)
                       .toInt();
  cat_data_bits_ = settings
                       .value(storageKey(QStringLiteral("radio/catDataBits")),
                              cat_data_bits_)
                       .toInt();
  cat_parity_index_ =
      settings
          .value(storageKey(QStringLiteral("radio/catParityIndex")),
                 cat_parity_index_)
          .toInt();
  cat_stop_bits_ = settings
                       .value(storageKey(QStringLiteral("radio/catStopBits")),
                              cat_stop_bits_)
                       .toInt();
  cat_flow_control_index_ =
      settings
          .value(storageKey(QStringLiteral("radio/catFlowControlIndex")),
                 cat_flow_control_index_)
          .toInt();
  poll_interval_ms_ =
      settings
          .value(storageKey(QStringLiteral("radio/pollIntervalMs")),
                 poll_interval_ms_)
          .toInt();
  timeout_ms_ =
      settings.value(storageKey(QStringLiteral("radio/timeoutMs")), timeout_ms_)
          .toInt();
  split_enabled_ =
      settings.value(storageKey(QStringLiteral("radio/splitEnabled")), false)
          .toBool();
  rx_transverter_offset_hz_ =
      settings
          .value(storageKey(QStringLiteral("radio/rxTransverterOffsetHz")), 0)
          .toLongLong();
  tx_transverter_offset_hz_ =
      settings
          .value(storageKey(QStringLiteral("radio/txTransverterOffsetHz")), 0)
          .toLongLong();
  cw_tone_sideband_index_ = std::clamp(
      settings.value(storageKey(QStringLiteral("radio/cwToneSidebandIndex")), 0)
          .toInt(),
      0, 1);
  keying_port_ =
      settings.value(storageKey(QStringLiteral("keying/port"))).toString();
  direct_keying_enabled_ =
      settings.value(storageKey(QStringLiteral("keying/directEnabled")), false)
          .toBool();
  ptt_line_index_ =
      settings
          .value(storageKey(QStringLiteral("keying/pttLineIndex")),
                 ptt_line_index_)
          .toInt();
  key_line_index_ =
      settings
          .value(storageKey(QStringLiteral("keying/keyLineIndex")),
                 key_line_index_)
          .toInt();
  ptt_active_high_ =
      settings.value(storageKey(QStringLiteral("keying/pttActiveHigh")), true)
          .toBool();
  key_active_high_ =
      settings.value(storageKey(QStringLiteral("keying/keyActiveHigh")), true)
          .toBool();
  direct_keying_acceptance_sha256_ =
      settings
          .value(storageKey(
              QStringLiteral("keying/acceptanceConfigurationSha256")))
          .toString();
  direct_keying_acceptance_platform_ =
      settings.value(storageKey(QStringLiteral("keying/acceptancePlatform")))
          .toString();
  direct_keying_acceptance_utc_seconds_ =
      settings
          .value(storageKey(QStringLiteral("keying/acceptanceUtcSeconds")), 0)
          .toLongLong();
  const DirectKeyingConfig stored_keying_config{
      .port_name = keying_port_,
      .ptt_line =
          ptt_line_index_ == 0 ? DirectKeyingLine::Rts : DirectKeyingLine::Dtr,
      .key_line =
          key_line_index_ == 0 ? DirectKeyingLine::Rts : DirectKeyingLine::Dtr,
      .ptt_active_high = ptt_active_high_,
      .key_active_high = key_active_high_};
  const QString current_acceptance =
      directKeyingConfigurationSha256(stored_keying_config);
  direct_keying_validated_ =
      direct_keying_enabled_ && direct_keying_acceptance_utc_seconds_ > 0 &&
      direct_keying_acceptance_platform_ == acceptancePlatformToken() &&
      direct_keying_acceptance_sha256_ == current_acceptance;
  direct_keying_acceptance_status_ =
      direct_keying_validated_
          ? QStringLiteral(
                "Measured physical loopback passed for this exact "
                "keying configuration. Complete dummy-load "
                "acceptance before on-air use.")
          : QStringLiteral(
                "Physical loopback has not been measured for this "
                "exact keying configuration.");
  tx_speed_mode_ = std::clamp(
      settings.value(storageKey(QStringLiteral("keying/txSpeedMode")), 0)
          .toInt(),
      0, 1);
  fixed_tx_wpm_ = std::clamp(
      settings.value(storageKey(QStringLiteral("keying/fixedTxWpm")), 20)
          .toInt(),
      5, 80);
  tx_macro_1_ = settings
                    .value(storageKey(QStringLiteral("keying/txMacro1")),
                           QStringLiteral("TU"))
                    .toString()
                    .simplified()
                    .toUpper()
                    .left(64);
  tx_macro_2_ = settings
                    .value(storageKey(QStringLiteral("keying/txMacro2")),
                           QStringLiteral("AGN"))
                    .toString()
                    .simplified()
                    .toUpper()
                    .left(64);
  tx_macro_3_ = settings
                    .value(storageKey(QStringLiteral("keying/txMacro3")),
                           QStringLiteral("PSE K"))
                    .toString()
                    .simplified()
                    .toUpper()
                    .left(64);
  tx_macro_4_ = settings
                    .value(storageKey(QStringLiteral("keying/txMacro4")),
                           QStringLiteral("73"))
                    .toString()
                    .simplified()
                    .toUpper()
                    .left(64);
  target_fps_ =
      settings.value(storageKey(QStringLiteral("display/targetFps")), 60)
          .toInt();
  waterfall_rate_ =
      settings.value(storageKey(QStringLiteral("display/waterfallRate")), 60)
          .toInt();
  waterfall_time_span_seconds_ =
      settings
          .value(storageKey(QStringLiteral("display/waterfallTimeSpanSeconds")),
                 10)
          .toInt();
  spectrum_display_mode_ = std::clamp(
      settings
          .value(storageKey(QStringLiteral("display/spectrumDisplayMode")), 0)
          .toInt(),
      0, 1);
  automatic_range_ =
      settings.value(storageKey(QStringLiteral("display/automaticRange")), true)
          .toBool();
  lower_bound_db_ =
      settings.value(storageKey(QStringLiteral("display/lowerBoundDb")), -120.0)
          .toDouble();
  upper_bound_db_ =
      settings.value(storageKey(QStringLiteral("display/upperBoundDb")), -20.0)
          .toDouble();
  automatic_range_span_db_ =
      settings
          .value(storageKey(QStringLiteral("display/automaticRangeSpanDb")),
                 60.0)
          .toDouble();
  waterfall_noise_suppression_ =
      settings
          .value(
              storageKey(QStringLiteral("display/waterfallNoiseSuppression")),
              true)
          .toBool();
  waterfall_noise_margin_db_ =
      settings
          .value(storageKey(QStringLiteral("display/waterfallNoiseMarginDb")),
                 6.0)
          .toDouble();
  show_cw_guide_ =
      settings.value(storageKey(QStringLiteral("display/showCwGuide")), true)
          .toBool();
  cw_guide_center_hz_ =
      settings
          .value(storageKey(QStringLiteral("display/cwGuideCenterHz")), 700.0)
          .toDouble();
  cw_guide_width_hz_ =
      settings
          .value(storageKey(QStringLiteral("display/cwGuideWidthHz")), 200.0)
          .toDouble();
  averaging_frames_ =
      settings.value(storageKey(QStringLiteral("display/averagingFrames")), 3)
          .toInt();
  show_grid_ =
      settings.value(storageKey(QStringLiteral("display/showGrid")), true)
          .toBool();
  waterfall_rendering_enabled_ =
      settings
          .value(
              storageKey(QStringLiteral("display/waterfallRenderingEnabled")),
              true)
          .toBool();
  show_spectrum_gesture_hints_ =
      settings
          .value(storageKey(QStringLiteral("display/showSpectrumGestureHints")),
                 true)
          .toBool();
  decoded_signal_timeout_seconds_ =
      settings
          .value(
              storageKey(QStringLiteral("display/decodedSignalTimeoutSeconds")),
              30)
          .toInt();
  decode_weak_signals_ =
      settings
          .value(storageKey(QStringLiteral("decoder/decodeWeakSignals")), false)
          .toBool();
  minimum_decode_snr_db_ =
      settings
          .value(storageKey(QStringLiteral("decoder/minimumDecodeSnrDb")), 4.0)
          .toDouble();
  local_decoder_enabled_ =
      settings.value(storageKey(QStringLiteral("decoder/localEnabled")), false)
          .toBool();
  callsign_database_correction_enabled_ =
      settings
          .value(
              storageKey(QStringLiteral("decoder/callsignDatabaseCorrection")),
              false)
          .toBool();
  keying_model_ = settings
                      .value(storageKey(QStringLiteral("decoder/keyingModel")),
                             QStringLiteral("adaptive-threshold"))
                      .toString();
  debug_capture_maximum_seconds_ = std::clamp(
      settings
          .value(storageKey(
                     QStringLiteral("diagnostics/debugCaptureMaximumSeconds")),
                 300)
          .toInt(),
      30, 1'800);
  operator_role_ =
      settings
          .value(storageKey(QStringLiteral("station/operatorRole")),
                 QStringLiteral("monitor"))
          .toString();
  local_decoder_model_path_ =
      settings.value(storageKey(QStringLiteral("decoder/localModelPath")))
          .toString();
  local_decoder_metadata_path_ =
      settings.value(storageKey(QStringLiteral("decoder/localMetadataPath")))
          .toString();
  local_callsign_database_enabled_ =
      settings
          .value(storageKey(
                     QStringLiteral("decoder/localCallsignDatabaseEnabled")),
                 false)
          .toBool();
  local_callsign_database_path_ =
      settings
          .value(
              storageKey(QStringLiteral("decoder/localCallsignDatabasePath")))
          .toString();
  // dxSpots/enabled, reverseBeacon, cluster, endpoint and refreshSeconds
  // belonged to the deleted HTTPS spot provider and are deliberately not read:
  // a stale stored value must not be able to describe a transport that is no
  // longer built.
  dx_spots_retention_minutes_ = std::clamp(
      settings
          .value(storageKey(QStringLiteral("dxSpots/retentionMinutes")), 15)
          .toInt(),
      1, 60);
  dx_spots_tolerance_hz_ = std::clamp(
      settings.value(storageKey(QStringLiteral("dxSpots/toleranceHz")), 250)
          .toInt(),
      50, 1'000);
  dx_spots_show_labels_ =
      settings.value(storageKey(QStringLiteral("dxSpots/showLabels")), true)
          .toBool();
  // The server list is file data rather than profile data: read it once, and
  // keep whatever was read when a profile is switched underneath it.
  if (dx_cluster_servers_.isEmpty()) {
    dx_cluster_servers_ = DxClusterServerList::load().toVariantList();
    emit dxClusterServersChanged();
  }
  dx_cluster_custom_host_ =
      settings.value(storageKey(QStringLiteral("dxcluster/customHost")))
          .toString()
          .trimmed()
          .left(kMaximumDxClusterHostLength);
  dx_cluster_custom_port_ = std::clamp(
      settings.value(storageKey(QStringLiteral("dxcluster/customPort")), 7'300)
          .toInt(),
      1, 65'535);
  dx_cluster_login_ssid_ = std::clamp(
      settings.value(storageKey(QStringLiteral("dxcluster/loginSsid")), 0)
          .toInt(),
      0, 99);
  {
    // A stored index is only meaningful against the list that is actually
    // loaded. An edited or shortened file must not silently redirect the
    // operator to a different server than the one they chose, so an index that
    // no longer exists falls back to the first entry.
    const int server_count = static_cast<int>(dx_cluster_servers_.size());
    const int stored_index =
        settings.value(storageKey(QStringLiteral("dxcluster/serverIndex")), 0)
            .toInt();
    dx_cluster_server_index_ =
        (stored_index == kDxClusterCustomServerIndex ||
         (stored_index >= 0 && stored_index < server_count))
            ? stored_index
            : (server_count > 0 ? 0 : kDxClusterCustomServerIndex);
  }
  // Read after the station callsign, which is what the link logs in as. With
  // no callsign there is no login, so a stored "on" is not honoured.
  dx_cluster_enabled_ =
      settings.value(storageKey(QStringLiteral("dxcluster/enabled")), false)
          .toBool() &&
      !own_callsign_.isEmpty();
  diagnostics_server_port_ = std::clamp(
      settings
          .value(storageKey(QStringLiteral("diagnostics/serverPort")),
                 kDefaultDiagnosticsServerPort)
          .toInt(),
      kMinimumDiagnosticsServerPort, 65'535);
  diagnostics_server_token_ =
      settings.value(storageKey(QStringLiteral("diagnostics/serverToken")))
          .toString()
          .trimmed()
          .left(kMaximumDiagnosticsTokenLength);
  diagnostics_server_addresses_ = sanitize_diagnostics_addresses(
      settings
          .value(storageKey(QStringLiteral("diagnostics/serverAddresses")),
                 QStringList{QStringLiteral("127.0.0.1")})
          .toStringList());
  // No default: an absent key means the operator has never said who may
  // connect, and an empty list is exactly how the server is told that -- it
  // then admits loopback and nothing else.
  diagnostics_allowed_peers_ = sanitize_diagnostics_peers(
      settings.value(storageKey(QStringLiteral("diagnostics/allowedPeers")))
          .toStringList());
  // Read after the addresses and the token, because those are what decide
  // whether a stored "on" is a state this build would have allowed to be set
  // in the first place. An edited settings file cannot enable an exposure the
  // switch itself refuses.
  diagnostics_server_enabled_ =
      settings
          .value(storageKey(QStringLiteral("diagnostics/serverEnabled")), false)
          .toBool() &&
      !diagnosticsServerExposureUnguarded();
  local_callsign_database_status_ =
      !local_callsign_database_enabled_
          ? QStringLiteral("Disabled. No local callsign list is in use.")
          : (local_callsign_database_path_.isEmpty()
                 ? QStringLiteral(
                       "Select a local master.scp or Call History text file.")
                 : QStringLiteral("Saved local callsign list configured."));
}

bool AppSettings::completeSetup() {
  if (!apply()) {
    return false;
  }
  QSettings settings;
  settings.setValue(storageKey(QStringLiteral("configuration/setupComplete")),
                    true);
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    setStatusMessage(QStringLiteral(
        "Setup could not be completed because settings were not writable."));
    return false;
  }
  setup_complete_ = true;
  emit setupCompleteChanged();
  setStatusMessage(
      QStringLiteral("Station profile is ready. Transmit remains disarmed."));
  return true;
}

bool AppSettings::selectProfile(const QString& profile_name) {
  const QString key = normalizeProfileKey(profile_name);
  if (key.isEmpty()) {
    setStatusMessage(QStringLiteral("Select a valid station profile."));
    return false;
  }
  QSettings settings;
  if (!settings.contains(
          QStringLiteral("profiles/%1/configuration/schemaVersion").arg(key))) {
    setStatusMessage(
        QStringLiteral("The selected station profile does not exist."));
    return false;
  }
  profile_storage_key_ = key;
  profile_name_ =
      settings
          .value(
              QStringLiteral("profiles/%1/configuration/displayName").arg(key),
              profile_name)
          .toString();
  resetInMemorySettings();
  load();
  refreshAudioInputs();
  profile_selection_required_ = false;
  emit profileChanged();
  emit setupCompleteChanged();
  emit profileSelectionRequiredChanged();
  emit audioInputsChanged();
  emit settingsChanged();
  emit localDecoderConfigurationCommitted(local_decoder_enabled_,
                                          local_decoder_model_path_,
                                          local_decoder_metadata_path_);
  emit localCallsignDatabaseChanged();
  emit localCallsignDatabaseConfigurationCommitted(
      local_callsign_database_enabled_, local_callsign_database_path_);
  setStatusMessage(
      QStringLiteral("Station profile selected. Transmit remains disarmed."));
  return true;
}

bool AppSettings::createProfile(const QString& profile_name) {
  const QString trimmed = profile_name.trimmed();
  const QString key = normalizeProfileKey(trimmed);
  if (trimmed.isEmpty() || key.isEmpty()) {
    setStatusMessage(QStringLiteral("Enter a valid profile name."));
    return false;
  }
  QSettings settings;
  const QString schema_key =
      QStringLiteral("profiles/%1/configuration/schemaVersion").arg(key);
  if (settings.contains(schema_key)) {
    setStatusMessage(
        QStringLiteral("A profile with that name already exists."));
    return false;
  }
  profile_storage_key_ = key;
  profile_name_ = trimmed;
  resetInMemorySettings();
  settings.setValue(schema_key, kSchemaVersion);
  settings.setValue(storageKey(QStringLiteral("configuration/displayName")),
                    profile_name_);
  settings.setValue(storageKey(QStringLiteral("configuration/setupComplete")),
                    false);
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    setStatusMessage(QStringLiteral("The new profile could not be created."));
    return false;
  }
  refreshProfiles();
  profile_selection_required_ = false;
  emit profileChanged();
  emit setupCompleteChanged();
  emit profileSelectionRequiredChanged();
  emit audioInputsChanged();
  emit settingsChanged();
  emit localDecoderConfigurationCommitted(local_decoder_enabled_,
                                          local_decoder_model_path_,
                                          local_decoder_metadata_path_);
  emit localCallsignDatabaseChanged();
  emit localCallsignDatabaseConfigurationCommitted(
      local_callsign_database_enabled_, local_callsign_database_path_);
  setStatusMessage(
      QStringLiteral("New station profile created. Complete its setup."));
  return true;
}

QString AppSettings::storageKey(const QString& relative) const {
  return QStringLiteral("profiles/%1/%2").arg(profile_storage_key_, relative);
}

QString AppSettings::normalizeProfileKey(const QString& name) {
  QString result = name.trimmed().toLower();
  result.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]")),
                 QStringLiteral("-"));
  while (result.contains(QStringLiteral("--"))) {
    result.replace(QStringLiteral("--"), QStringLiteral("-"));
  }
  return result.left(64);
}

void AppSettings::refreshProfiles() {
  QSettings settings;
  settings.beginGroup(QStringLiteral("profiles"));
  const QStringList groups = settings.childGroups();
  settings.endGroup();
  QStringList profiles;
  for (const auto& group : groups) {
    profiles.push_back(
        settings
            .value(QStringLiteral("profiles/%1/configuration/displayName")
                       .arg(group),
                   group)
            .toString());
  }
  if (!profiles.contains(profile_name_, Qt::CaseInsensitive)) {
    profiles.push_back(profile_name_);
  }
  profiles.sort(Qt::CaseInsensitive);
  if (profiles != available_profiles_) {
    available_profiles_ = profiles;
    emit profilesChanged();
  }
}

void AppSettings::resetInMemorySettings() {
  setup_complete_ = false;
  audio_input_id_.clear();
  audio_input_name_ = QStringLiteral("System default input");
  audio_input_device_name_.clear();
  audio_dc_rejection_ = true;
  audio_automatic_gain_ = false;
  audio_gain_db_ = 0.0;
  audio_automatic_gain_target_dbfs_ = -12.0;
  audio_automatic_bandwidth_ = true;
  audio_lower_frequency_hz_ = 100.0;
  audio_upper_frequency_hz_ = 3'000.0;
  audio_input_radio_linked_ = false;
  receiver_input_type_index_ = 0;
  preferred_source_mode_ = 0;
  sdr_startup_restore_pending_ = false;
  sdr_startup_restore_found_ = false;
  sdr_startup_restore_device_name_.clear();
  sdr_startup_restore_device_label_.clear();
  sdr_physical_device_id_.clear();
  sdr_device_mode_id_.clear();
  sdr_device_id_.clear();
  sdr_device_name_.clear();
  sdr_center_frequency_hz_ = 14'050'000ULL;
  sdr_sample_rate_hz_ = 250'000;
  sdr_bandwidth_hz_ = 0;
  sdr_sample_rate_options_.clear();
  sdr_bandwidth_options_.clear();
  sdr_antenna_names_.clear();
  sdr_antenna_.clear();
  sdr_decoder_center_frequency_hz_ = 14'050'000ULL;
  sdr_decoder_bandwidth_hz_ = 24'000;
  sdr_follow_radio_vfo_ = false;
  sdr_tuning_step_hz_ = 1'000;
  sdr_radio_lo_offset_hz_ = 0;
  sdr_automatic_gain_ = true;
  sdr_automatic_gain_available_ = true;
  sdr_gain_db_ = 30.0;
  sdr_minimum_gain_db_ = -100.0;
  sdr_maximum_gain_db_ = 100.0;
  own_callsign_.clear();
  radio_enabled_ = false;
  reference_rig_index_ = 0;
  frequency_backend_index_ = 0;
  radio_tuning_step_hz_ = 1'000;
  radio_tx_mode_target_ = cwassistant::core::RadioMode::Cw;
  omnirig_slot_ = 1;
  hamlib_host_ = QStringLiteral("127.0.0.1");
  hamlib_port_ = 4'532;
  hamlib_rx_vfo_ = QStringLiteral("VFOA");
  hamlib_tx_vfo_ = QStringLiteral("VFOB");
  hamlib_writable_ = false;
  if (hamlib_client_) hamlib_client_->disconnectFromServer();
  cat4om_url_ = QStringLiteral("ws://127.0.0.1:5001/");
  cat4om_radio_id_.clear();
  cat4om_password_.clear();
  cat_port_.clear();
  keying_port_.clear();
  direct_keying_enabled_ = false;
  direct_keying_validated_ = false;
  tx_speed_mode_ = 0;
  fixed_tx_wpm_ = 20;
  tx_macro_1_ = QStringLiteral("TU");
  tx_macro_2_ = QStringLiteral("AGN");
  tx_macro_3_ = QStringLiteral("PSE K");
  tx_macro_4_ = QStringLiteral("73");
  split_enabled_ = false;
  rx_transverter_offset_hz_ = 0;
  tx_transverter_offset_hz_ = 0;
  cw_tone_sideband_index_ = 0;
  target_fps_ = 60;
  waterfall_rate_ = 60;
  waterfall_time_span_seconds_ = 10;
  spectrum_display_mode_ = 0;
  automatic_range_ = true;
  lower_bound_db_ = -120.0;
  upper_bound_db_ = -20.0;
  automatic_range_span_db_ = 60.0;
  waterfall_noise_suppression_ = true;
  waterfall_noise_margin_db_ = 6.0;
  show_cw_guide_ = true;
  cw_guide_center_hz_ = 700.0;
  cw_guide_width_hz_ = 200.0;
  averaging_frames_ = 3;
  show_grid_ = true;
  show_spectrum_gesture_hints_ = true;
  decoded_signal_timeout_seconds_ = 30;
  decode_weak_signals_ = false;
  minimum_decode_snr_db_ = 4.0;
  local_decoder_enabled_ = false;
  callsign_database_correction_enabled_ = false;
  keying_model_ = QStringLiteral("adaptive-threshold");
  debug_capture_maximum_seconds_ = 300;
  operator_role_ = QStringLiteral("monitor");
  local_decoder_model_path_.clear();
  local_decoder_metadata_path_.clear();
  local_callsign_database_enabled_ = false;
  local_callsign_database_path_.clear();
  local_callsign_database_status_ =
      QStringLiteral("Disabled. No local callsign list is in use.");
  dx_spots_retention_minutes_ = 15;
  dx_spots_tolerance_hz_ = 250;
  dx_spots_show_labels_ = true;
  // The loaded server list is deliberately left alone: it is file data shared
  // by every profile, not a per-station setting.
  dx_cluster_enabled_ = false;
  dx_cluster_server_index_ = dx_cluster_servers_.isEmpty()
                                 ? kDxClusterCustomServerIndex
                                 : 0;
  dx_cluster_custom_host_.clear();
  dx_cluster_custom_port_ = 7'300;
  dx_cluster_login_ssid_ = 0;
  diagnostics_server_enabled_ = false;
  diagnostics_server_port_ = kDefaultDiagnosticsServerPort;
  diagnostics_server_token_.clear();
  // The discovered address list, unlike the chosen one, is left alone: it
  // describes this machine's interfaces rather than this profile's choices,
  // and re-enumerating them is a system call that resetting a profile has no
  // reason to make.
  diagnostics_server_addresses_ = QStringList{QStringLiteral("127.0.0.1")};
  // Back to loopback only. A reset that kept a network on the allowed list
  // would leave a profile permitting readers its operator never named.
  diagnostics_allowed_peers_.clear();
  waterfall_rendering_enabled_ = true;
  applyReferenceDefaults(0);
}

void AppSettings::setStatusMessage(QString message) {
  if (assign_if_changed(status_message_, message)) {
    emit statusMessageChanged();
  }
}

void AppSettings::showOmniRigConfiguration() {
#ifdef Q_OS_WIN
  if (!ensureOmniRigAutomation()) {
    setStatusMessage(
        QStringLiteral("OmniRig is not installed or could not be started."));
    return;
  }
  auto* automation = static_cast<IDispatch*>(omnirig_automation_);

  OLECHAR* property_name = const_cast<OLECHAR*>(L"DialogVisible");
  DISPID property_id{};
  HRESULT result = automation->GetIDsOfNames(IID_NULL, &property_name, 1,
                                             LOCALE_USER_DEFAULT, &property_id);
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_BOOL;
  value.boolVal = VARIANT_TRUE;
  DISPID named_argument = DISPID_PROPERTYPUT;
  DISPPARAMS parameters{&value, &named_argument, 1, 1};
  if (SUCCEEDED(result)) {
    result = automation->Invoke(property_id, IID_NULL, LOCALE_USER_DEFAULT,
                                DISPATCH_PROPERTYPUT, &parameters, nullptr,
                                nullptr, nullptr);
  }
  setStatusMessage(
      SUCCEEDED(result)
          ? QStringLiteral("OmniRig configuration opened. Match its live CAT "
                           "values to this profile.")
          : QStringLiteral("OmniRig configuration could not be opened."));
#else
  setStatusMessage(
      QStringLiteral("OmniRig integration is available on "
                     "Windows; use Hamlib on this platform."));
#endif
}

void AppSettings::connectHamlib() {
  if (!hamlib_client_) return;
  HamlibRigctldClient::Configuration configuration;
  configuration.host = hamlib_host_.trimmed();
  configuration.port =
      static_cast<quint16>(std::clamp(hamlib_port_, 1, 65'535));
  configuration.rx_vfo = hamlib_rx_vfo_.trimmed().toUpper();
  configuration.tx_vfo = hamlib_tx_vfo_.trimmed().toUpper();
  configuration.writable = hamlib_writable_;
  configuration.poll_interval_ms = std::clamp(poll_interval_ms_, 50, 10'000);
  configuration.request_timeout_ms = std::clamp(timeout_ms_, 100, 60'000);
  hamlib_client_->connectToServer(std::move(configuration));
}

void AppSettings::disconnectHamlib() {
  if (hamlib_client_) hamlib_client_->disconnectFromServer();
}

bool AppSettings::runDirectKeyingLoopback(
    const bool radio_disconnected_confirmed) {
  if (!direct_keying_enabled_) {
    direct_keying_acceptance_status_ = QStringLiteral(
        "Enable direct keying before running the loopback test.");
    emit settingsChanged();
    return false;
  }
  if (!cat_port_.trimmed().isEmpty() &&
      keying_port_.trimmed() == cat_port_.trimmed()) {
    direct_keying_acceptance_status_ = QStringLiteral(
        "The CAT and keying ports must be different before testing.");
    emit settingsChanged();
    return false;
  }
  const DirectKeyingConfig configuration{
      .port_name = keying_port_.trimmed(),
      .ptt_line =
          ptt_line_index_ == 0 ? DirectKeyingLine::Rts : DirectKeyingLine::Dtr,
      .key_line =
          key_line_index_ == 0 ? DirectKeyingLine::Rts : DirectKeyingLine::Dtr,
      .ptt_active_high = ptt_active_high_,
      .key_active_high = key_active_high_};
  DirectKeyingAcceptanceProbe probe;
  const DirectKeyingProbeResult result =
      probe.run(configuration, radio_disconnected_confirmed);
  direct_keying_validated_ = result.passed;
  direct_keying_acceptance_status_ = result.detail;
  direct_keying_acceptance_sha256_ =
      result.passed ? directKeyingConfigurationSha256(configuration)
                    : QString{};
  direct_keying_acceptance_platform_ =
      result.passed ? acceptancePlatformToken() : QString{};
  direct_keying_acceptance_utc_seconds_ =
      result.passed ? QDateTime::currentSecsSinceEpoch() : 0;

  QSettings settings;
  settings.remove(storageKey(QStringLiteral("keying/directValidated")));
  settings.setValue(
      storageKey(QStringLiteral("keying/acceptanceConfigurationSha256")),
      direct_keying_acceptance_sha256_);
  settings.setValue(storageKey(QStringLiteral("keying/acceptancePlatform")),
                    direct_keying_acceptance_platform_);
  settings.setValue(storageKey(QStringLiteral("keying/acceptanceUtcSeconds")),
                    direct_keying_acceptance_utc_seconds_);
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    direct_keying_validated_ = false;
    direct_keying_acceptance_status_ = QStringLiteral(
        "Loopback result could not be stored; transmit remains unavailable.");
  }
  emit settingsChanged();
  return direct_keying_validated_;
}

#ifdef Q_OS_WIN
bool AppSettings::ensureOmniRigAutomation() {
  if (omnirig_automation_ != nullptr) {
    return true;
  }
  if (!com_initialization_attempted_) {
    const HRESULT init_result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    com_initialized_ = SUCCEEDED(init_result);
    com_initialization_attempted_ = true;
  }
  CLSID class_id{};
  HRESULT result = CLSIDFromProgID(L"OmniRig.OmniRigX", &class_id);
  IDispatch* automation = nullptr;
  if (SUCCEEDED(result)) {
    result =
        CoCreateInstance(class_id, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch,
                         reinterpret_cast<void**>(&automation));
  }
  if (FAILED(result) || automation == nullptr) {
    return false;
  }
  omnirig_automation_ = automation;
  return true;
}

bool AppSettings::writeOmniRigRxFrequency(
    const std::uint64_t dial_frequency_hz) {
  if (!ensureOmniRigAutomation()) {
    return false;
  }
  auto* automation = static_cast<IDispatch*>(omnirig_automation_);
  VARIANT rig_value;
  const auto rig_property = omnirig_slot_ == 2 ? L"Rig2" : L"Rig1";
  if (!automation_property(automation, rig_property, &rig_value)) {
    return false;
  }
  IDispatch* rig = rig_value.vt == VT_DISPATCH ? rig_value.pdispVal : nullptr;
  bool written = false;
  if (rig != nullptr) {
    const auto target = omni_rig_rx_write_target(rig);
    const wchar_t* property =
        target == cwassistant::core::OmniRigRxFrequencyTarget::FrequencyA
            ? L"FreqA"
        : target == cwassistant::core::OmniRigRxFrequencyTarget::FrequencyB
            ? L"FreqB"
        : target == cwassistant::core::OmniRigRxFrequencyTarget::Frequency
            ? L"Freq"
            : nullptr;
    written = property != nullptr &&
              automation_put_frequency(rig, property, dial_frequency_hz);
  }
  VariantClear(&rig_value);
  return written;
}

bool AppSettings::writeOmniRigTxFrequency(
    const std::uint64_t dial_frequency_hz) {
  if (!ensureOmniRigAutomation()) return false;
  auto* automation = static_cast<IDispatch*>(omnirig_automation_);
  VARIANT rig_value;
  const auto rig_property = omnirig_slot_ == 2 ? L"Rig2" : L"Rig1";
  if (!automation_property(automation, rig_property, &rig_value)) return false;
  IDispatch* rig = rig_value.vt == VT_DISPATCH ? rig_value.pdispVal : nullptr;
  bool written = false;
  if (rig != nullptr) {
    VARIANT vfo_value;
    VARIANT writable_value;
    const bool has_vfo = automation_property(rig, L"Vfo", &vfo_value);
    const bool has_writable =
        automation_property(rig, L"WriteableParams", &writable_value);
    const auto vfo = has_vfo ? automation_integer(vfo_value) : std::nullopt;
    const auto writable =
        has_writable ? automation_integer(writable_value) : std::nullopt;
    // Use the last authoritative logical TX-VFO identity first. A request that
    // enables split and immediately writes the pointed frequency can arrive
    // before OmniRig's Vfo property changes from its simplex value; deriving
    // TX from that transient value wrote the receive VFO on affected rigs.
    const wchar_t* property = nullptr;
    if (radio_state_.tx_vfo.observation ==
        cwassistant::core::RadioObservation::Known) {
      if (radio_state_.tx_vfo.identifier == "A") property = L"FreqA";
      if (radio_state_.tx_vfo.identifier == "B") property = L"FreqB";
    }
    if (property == nullptr && vfo)
      property = omni_rig_frequency_property(*vfo, true);
    const long required = property && property[4] == L'A' ? 0x04L : 0x08L;
    written = property && writable && ((*writable & required) != 0) &&
              automation_put_frequency(rig, property, dial_frequency_hz);
    if (has_vfo) VariantClear(&vfo_value);
    if (has_writable) VariantClear(&writable_value);
  }
  VariantClear(&rig_value);
  return written;
}

bool AppSettings::writeOmniRigMode(const cwassistant::core::RadioMode mode) {
  const auto requested = omni_rig_mode_value(mode);
  if (!requested || !ensureOmniRigAutomation()) return false;
  auto* automation = static_cast<IDispatch*>(omnirig_automation_);
  VARIANT rig_value;
  const auto rig_property = omnirig_slot_ == 2 ? L"Rig2" : L"Rig1";
  if (!automation_property(automation, rig_property, &rig_value)) return false;
  IDispatch* rig = rig_value.vt == VT_DISPATCH ? rig_value.pdispVal : nullptr;
  bool written = false;
  if (rig != nullptr) {
    VARIANT writable_value;
    const bool present =
        automation_property(rig, L"WriteableParams", &writable_value);
    const auto writable =
        present ? automation_integer(writable_value) : std::nullopt;
    written = writable && ((*writable & *requested) != 0) &&
              automation_put_integer(rig, L"Mode", *requested);
    if (present) VariantClear(&writable_value);
  }
  VariantClear(&rig_value);
  return written;
}

bool AppSettings::writeOmniRigSplit(const bool enabled) {
  if (!ensureOmniRigAutomation()) return false;
  auto* automation = static_cast<IDispatch*>(omnirig_automation_);
  VARIANT rig_value;
  const auto rig_property = omnirig_slot_ == 2 ? L"Rig2" : L"Rig1";
  if (!automation_property(automation, rig_property, &rig_value)) return false;
  IDispatch* rig = rig_value.vt == VT_DISPATCH ? rig_value.pdispVal : nullptr;
  bool written = false;
  if (rig != nullptr) {
    const long requested = enabled ? kOmniRigSplitOn : kOmniRigSplitOff;
    VARIANT writable_value;
    const bool present =
        automation_property(rig, L"WriteableParams", &writable_value);
    const auto writable =
        present ? automation_integer(writable_value) : std::nullopt;
    written = writable && ((*writable & requested) != 0) &&
              automation_put_integer(rig, L"Split", requested);
    if (present) VariantClear(&writable_value);
  }
  VariantClear(&rig_value);
  return written;
}
#endif

void AppSettings::testCat4omConnection() {
  cat4om_client_->connectToServer(QUrl(cat4om_url_.trimmed()), cat4om_radio_id_,
                                  {}, true);
}

void AppSettings::connectCat4omControl() {
  cat4om_client_->connectToServer(QUrl(cat4om_url_.trimmed()), cat4om_radio_id_,
                                  cat4om_password_, false);
  cat4om_password_.clear();
  emit settingsChanged();
}

void AppSettings::disconnectCat4om() { cat4om_client_->disconnectFromServer(); }

void AppSettings::requestCat4omOwnership() {
  if (!cat4om_client_->requestOwnership()) {
    setStatusMessage(cat4om_client_->statusText());
  }
}

}  // namespace cwassistant::desktop
