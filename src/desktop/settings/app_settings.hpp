#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QMediaDevices>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include "cwassistant/core/frequency_plan.hpp"
#include "cwassistant/core/radio_control.hpp"

namespace cwassistant::desktop {

// Declared rather than included: only a reference to it is named here, and
// the receiver header pulls in the backend interface with it.
struct SdrDiscoveryReport;

class Cat4OmClient;
class HamlibRigctldClient;
class AppSettings final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QStringList referenceRigNames READ referenceRigNames CONSTANT)
  Q_PROPERTY(QString profileName READ profileName NOTIFY profileChanged)
  Q_PROPERTY(QStringList availableProfiles READ availableProfiles NOTIFY
                 profilesChanged)
  Q_PROPERTY(bool profileSelectionRequired READ profileSelectionRequired NOTIFY
                 profileSelectionRequiredChanged)
  Q_PROPERTY(bool setupComplete READ setupComplete NOTIFY setupCompleteChanged)
  Q_PROPERTY(QStringList serialPorts READ serialPorts NOTIFY serialPortsChanged)
  Q_PROPERTY(QStringList audioInputNames READ audioInputNames NOTIFY
                 audioInputsChanged)
  Q_PROPERTY(int audioInputIndex READ audioInputIndex NOTIFY audioInputsChanged)
  Q_PROPERTY(QString audioInputDisplayName READ audioInputDisplayName NOTIFY
                 audioInputsChanged)
  // True while two or more present inputs share one operating-system
  // description. The interface explains its ordinals only then, because an
  // explanation left on screen for the ordinary single-interface station is
  // noise, and withheld from the station that has two is a dead end.
  Q_PROPERTY(bool audioInputNamesAmbiguous READ audioInputNamesAmbiguous NOTIFY
                 audioInputsChanged)
  Q_PROPERTY(QStringList audioOutputNames READ audioOutputNames NOTIFY
                 audioOutputsChanged)
  Q_PROPERTY(
      int audioOutputIndex READ audioOutputIndex NOTIFY audioOutputsChanged)
  Q_PROPERTY(QString audioOutputDisplayName READ audioOutputDisplayName NOTIFY
                 audioOutputsChanged)
  Q_PROPERTY(
      QStringList receiverInputTypeNames READ receiverInputTypeNames CONSTANT)
  Q_PROPERTY(int receiverInputTypeIndex READ receiverInputTypeIndex WRITE
                 setReceiverInputTypeIndex NOTIFY receiverInputTypeChanged)
  // Which receiver source the operator was last working with: 0 sound-card
  // audio, 1 recorded file, 2 SDR. The same numbering ReplayController's
  // sourceMode uses, because this value is what is handed back to it.
  //
  // Persisted, so a restart comes back where the operator left off instead of
  // landing in audio mode with a perfectly good receiver already saved.
  // Deliberately separate from receiverInputTypeIndex above: that one says
  // which input the settings page is configuring, has no file entry, and is
  // forced back to audio whenever a saved SDR cannot be selected -- which is
  // exactly the moment this value still has to remember what was wanted.
  //
  // It records intent only. Nothing here decides what the application starts:
  // whether a saved receiver came back is answered by sdrSelectionRestored,
  // and the fallback that follows from a missing one belongs to the
  // application.
  Q_PROPERTY(int preferredSourceMode READ preferredSourceMode WRITE
                 setPreferredSourceMode NOTIFY settingsChanged)
  Q_PROPERTY(bool sdrBackendAvailable READ sdrBackendAvailable NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(QString sdrBackendVersion READ sdrBackendVersion NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(
      QStringList sdrModuleNames READ sdrModuleNames NOTIFY sdrSettingsChanged)
  Q_PROPERTY(
      QStringList sdrDeviceNames READ sdrDeviceNames NOTIFY sdrSettingsChanged)
  Q_PROPERTY(int sdrDeviceIndex READ sdrDeviceIndex NOTIFY sdrSettingsChanged)
  Q_PROPERTY(bool sdrDiscoveryRunning READ sdrDiscoveryRunning NOTIFY
                 sdrDiscoveryRunningChanged)
  Q_PROPERTY(QString sdrDeviceDisplayName READ sdrDeviceDisplayName NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(QStringList sdrOperatingModeNames READ sdrOperatingModeNames NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(int sdrOperatingModeIndex READ sdrOperatingModeIndex NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(QString sdrDiagnostic READ sdrDiagnostic NOTIFY sdrSettingsChanged)
  Q_PROPERTY(qulonglong sdrCenterFrequencyHz READ sdrCenterFrequencyHz WRITE
                 setSdrCenterFrequencyHz NOTIFY sdrSettingsChanged)
  Q_PROPERTY(int sdrSampleRateHz READ sdrSampleRateHz WRITE setSdrSampleRateHz
                 NOTIFY sdrSettingsChanged)
  Q_PROPERTY(int sdrBandwidthHz READ sdrBandwidthHz WRITE setSdrBandwidthHz
                 NOTIFY sdrSettingsChanged)
  Q_PROPERTY(QVariantList sdrSampleRateOptions READ sdrSampleRateOptions NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(QVariantList sdrBandwidthOptions READ sdrBandwidthOptions NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(QStringList sdrAntennaNames READ sdrAntennaNames NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(int sdrAntennaIndex READ sdrAntennaIndex NOTIFY sdrSettingsChanged)
  Q_PROPERTY(
      qulonglong sdrDecoderCenterFrequencyHz READ sdrDecoderCenterFrequencyHz
          WRITE setSdrDecoderCenterFrequencyHz NOTIFY sdrSettingsChanged)
  Q_PROPERTY(int sdrDecoderBandwidthHz READ sdrDecoderBandwidthHz WRITE
                 setSdrDecoderBandwidthHz NOTIFY sdrSettingsChanged)
  Q_PROPERTY(bool sdrFollowRadioVfo READ sdrFollowRadioVfo WRITE
                 setSdrFollowRadioVfo NOTIFY sdrSettingsChanged)
  Q_PROPERTY(int sdrTuningStepHz READ sdrTuningStepHz WRITE setSdrTuningStepHz
                 NOTIFY sdrSettingsChanged)
  Q_PROPERTY(QString sdrRadioSyncStatus READ sdrRadioSyncStatus NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(qint64 sdrRadioLoOffsetHz READ sdrRadioLoOffsetHz WRITE
                 setSdrRadioLoOffsetHz NOTIFY sdrSettingsChanged)
  Q_PROPERTY(bool sdrAutomaticGain READ sdrAutomaticGain WRITE
                 setSdrAutomaticGain NOTIFY sdrSettingsChanged)
  Q_PROPERTY(bool sdrAutomaticGainAvailable READ sdrAutomaticGainAvailable
                 NOTIFY sdrSettingsChanged)
  Q_PROPERTY(double sdrGainDb READ sdrGainDb WRITE setSdrGainDb NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(
      double sdrMinimumGainDb READ sdrMinimumGainDb NOTIFY sdrSettingsChanged)
  Q_PROPERTY(
      double sdrMaximumGainDb READ sdrMaximumGainDb NOTIFY sdrSettingsChanged)
  Q_PROPERTY(QString sdrWidePassbandSummary READ sdrWidePassbandSummary NOTIFY
                 sdrSettingsChanged)
  Q_PROPERTY(bool audioDcRejection READ audioDcRejection WRITE
                 setAudioDcRejection NOTIFY settingsChanged)
  Q_PROPERTY(bool audioAutomaticGain READ audioAutomaticGain WRITE
                 setAudioAutomaticGain NOTIFY settingsChanged)
  Q_PROPERTY(double audioGainDb READ audioGainDb WRITE setAudioGainDb NOTIFY
                 settingsChanged)
  Q_PROPERTY(
      double audioAutomaticGainTargetDbfs READ audioAutomaticGainTargetDbfs
          WRITE setAudioAutomaticGainTargetDbfs NOTIFY settingsChanged)
  Q_PROPERTY(bool audioAutomaticBandwidth READ audioAutomaticBandwidth WRITE
                 setAudioAutomaticBandwidth NOTIFY settingsChanged)
  Q_PROPERTY(double audioLowerFrequencyHz READ audioLowerFrequencyHz WRITE
                 setAudioLowerFrequencyHz NOTIFY settingsChanged)
  Q_PROPERTY(double audioUpperFrequencyHz READ audioUpperFrequencyHz WRITE
                 setAudioUpperFrequencyHz NOTIFY settingsChanged)
  Q_PROPERTY(bool audioInputRadioLinked READ audioInputRadioLinked WRITE
                 setAudioInputRadioLinked NOTIFY settingsChanged)
  Q_PROPERTY(QString ownCallsign READ ownCallsign WRITE setOwnCallsign NOTIFY
                 settingsChanged)
  Q_PROPERTY(bool omniRigAvailable READ omniRigAvailable CONSTANT)
  Q_PROPERTY(bool radioEnabled READ radioEnabled WRITE setRadioEnabled NOTIFY
                 settingsChanged)
  Q_PROPERTY(
      QString radioDisplayName READ radioDisplayName NOTIFY settingsChanged)
  Q_PROPERTY(QStringList detectedRadioNames READ detectedRadioNames NOTIFY
                 detectedRadiosChanged)
  Q_PROPERTY(
      int detectedRadioIndex READ detectedRadioIndex NOTIFY settingsChanged)
  Q_PROPERTY(
      int referenceRigIndex READ referenceRigIndex NOTIFY settingsChanged)
  Q_PROPERTY(int frequencyBackendIndex READ frequencyBackendIndex WRITE
                 setFrequencyBackendIndex NOTIFY settingsChanged)
  Q_PROPERTY(int radioTuningStepHz READ radioTuningStepHz WRITE
                 setRadioTuningStepHz NOTIFY settingsChanged)
  Q_PROPERTY(bool radioFrequencyWritable READ radioFrequencyWritable NOTIFY
                 radioFrequencyControlChanged)
  Q_PROPERTY(bool radioTxFrequencyWritable READ radioTxFrequencyWritable NOTIFY
                 radioFrequencyControlChanged)
  Q_PROPERTY(
      bool radioTxFrequencySyncAvailable READ radioTxFrequencySyncAvailable
          NOTIFY radioFrequencyControlChanged)
  Q_PROPERTY(
      bool radioPointedTxFrequencyAvailable READ
          radioPointedTxFrequencyAvailable NOTIFY radioFrequencyControlChanged)
  Q_PROPERTY(bool radioRxModeWritable READ radioRxModeWritable NOTIFY
                 radioFrequencyControlChanged)
  Q_PROPERTY(bool radioTxModeWritable READ radioTxModeWritable NOTIFY
                 radioFrequencyControlChanged)
  Q_PROPERTY(bool radioSplitWritable READ radioSplitWritable NOTIFY
                 radioFrequencyControlChanged)
  Q_PROPERTY(QString radioRxMode READ radioRxMode NOTIFY radioFrequencyChanged)
  Q_PROPERTY(QString radioTxMode READ radioTxMode NOTIFY radioFrequencyChanged)
  Q_PROPERTY(QString radioTxModeTarget READ radioTxModeTarget NOTIFY
                 radioFrequencyChanged)
  Q_PROPERTY(bool radioTxModeConfirmed READ radioTxModeConfirmed NOTIFY
                 radioFrequencyChanged)
  Q_PROPERTY(QString radioRxVfo READ radioRxVfo NOTIFY radioFrequencyChanged)
  Q_PROPERTY(QString radioTxVfo READ radioTxVfo NOTIFY radioFrequencyChanged)
  Q_PROPERTY(qulonglong radioTxVfoFrequencyHz READ radioTxVfoFrequencyHz NOTIFY
                 radioFrequencyChanged)
  Q_PROPERTY(
      bool radioSplitKnown READ radioSplitKnown NOTIFY radioFrequencyChanged)
  Q_PROPERTY(int omniRigSlot READ omniRigSlot WRITE setOmniRigSlot NOTIFY
                 settingsChanged)
  Q_PROPERTY(QString hamlibHost READ hamlibHost WRITE setHamlibHost NOTIFY
                 settingsChanged)
  Q_PROPERTY(
      int hamlibPort READ hamlibPort WRITE setHamlibPort NOTIFY settingsChanged)
  Q_PROPERTY(QString hamlibRxVfo READ hamlibRxVfo WRITE setHamlibRxVfo NOTIFY
                 settingsChanged)
  Q_PROPERTY(QString hamlibTxVfo READ hamlibTxVfo WRITE setHamlibTxVfo NOTIFY
                 settingsChanged)
  Q_PROPERTY(bool hamlibWritable READ hamlibWritable WRITE setHamlibWritable
                 NOTIFY settingsChanged)
  Q_PROPERTY(QString hamlibState READ hamlibState NOTIFY hamlibChanged)
  Q_PROPERTY(bool hamlibCanWrite READ hamlibCanWrite NOTIFY hamlibChanged)
  Q_PROPERTY(QString cat4omUrl READ cat4omUrl WRITE setCat4omUrl NOTIFY
                 settingsChanged)
  Q_PROPERTY(QString cat4omRadioId READ cat4omRadioId WRITE setCat4omRadioId
                 NOTIFY settingsChanged)
  Q_PROPERTY(QString cat4omPassword READ cat4omPassword WRITE setCat4omPassword
                 NOTIFY settingsChanged)
  Q_PROPERTY(QString cat4omState READ cat4omState NOTIFY cat4omChanged)
  Q_PROPERTY(QString cat4omFrequencySummary READ cat4omFrequencySummary NOTIFY
                 cat4omChanged)
  Q_PROPERTY(bool cat4omCanWrite READ cat4omCanWrite NOTIFY cat4omChanged)
  Q_PROPERTY(
      QString catPort READ catPort WRITE setCatPort NOTIFY settingsChanged)
  Q_PROPERTY(
      QVariantList supportedCatBaudRates READ supportedCatBaudRates CONSTANT)
  Q_PROPERTY(int catBaudRate READ catBaudRate WRITE setCatBaudRate NOTIFY
                 settingsChanged)
  Q_PROPERTY(int catDataBits READ catDataBits WRITE setCatDataBits NOTIFY
                 settingsChanged)
  Q_PROPERTY(int catParityIndex READ catParityIndex WRITE setCatParityIndex
                 NOTIFY settingsChanged)
  Q_PROPERTY(int catStopBits READ catStopBits WRITE setCatStopBits NOTIFY
                 settingsChanged)
  Q_PROPERTY(int catFlowControlIndex READ catFlowControlIndex WRITE
                 setCatFlowControlIndex NOTIFY settingsChanged)
  Q_PROPERTY(int pollIntervalMs READ pollIntervalMs WRITE setPollIntervalMs
                 NOTIFY settingsChanged)
  Q_PROPERTY(
      int timeoutMs READ timeoutMs WRITE setTimeoutMs NOTIFY settingsChanged)
  Q_PROPERTY(bool splitEnabled READ splitEnabled WRITE setSplitEnabled NOTIFY
                 settingsChanged)
  Q_PROPERTY(qint64 rxTransverterOffsetHz READ rxTransverterOffsetHz WRITE
                 setRxTransverterOffsetHz NOTIFY settingsChanged)
  Q_PROPERTY(qint64 txTransverterOffsetHz READ txTransverterOffsetHz WRITE
                 setTxTransverterOffsetHz NOTIFY settingsChanged)
  Q_PROPERTY(int cwToneSidebandIndex READ cwToneSidebandIndex WRITE
                 setCwToneSidebandIndex NOTIFY settingsChanged)
  Q_PROPERTY(QString keyingPort READ keyingPort WRITE setKeyingPort NOTIFY
                 settingsChanged)
  Q_PROPERTY(bool directKeyingEnabled READ directKeyingEnabled WRITE
                 setDirectKeyingEnabled NOTIFY settingsChanged)
  Q_PROPERTY(bool directKeyingValidated READ directKeyingValidated NOTIFY
                 settingsChanged)
  Q_PROPERTY(QString directKeyingAcceptanceStatus READ
                 directKeyingAcceptanceStatus NOTIFY settingsChanged)
  Q_PROPERTY(int pttLineIndex READ pttLineIndex WRITE setPttLineIndex NOTIFY
                 settingsChanged)
  Q_PROPERTY(int keyLineIndex READ keyLineIndex WRITE setKeyLineIndex NOTIFY
                 settingsChanged)
  Q_PROPERTY(bool pttActiveHigh READ pttActiveHigh WRITE setPttActiveHigh NOTIFY
                 settingsChanged)
  Q_PROPERTY(bool keyActiveHigh READ keyActiveHigh WRITE setKeyActiveHigh NOTIFY
                 settingsChanged)
  Q_PROPERTY(int txSpeedMode READ txSpeedMode WRITE setTxSpeedMode NOTIFY
                 settingsChanged)
  Q_PROPERTY(
      int fixedTxWpm READ fixedTxWpm WRITE setFixedTxWpm NOTIFY settingsChanged)
  Q_PROPERTY(
      QString txMacro1 READ txMacro1 WRITE setTxMacro1 NOTIFY settingsChanged)
  Q_PROPERTY(
      QString txMacro2 READ txMacro2 WRITE setTxMacro2 NOTIFY settingsChanged)
  Q_PROPERTY(
      QString txMacro3 READ txMacro3 WRITE setTxMacro3 NOTIFY settingsChanged)
  Q_PROPERTY(
      QString txMacro4 READ txMacro4 WRITE setTxMacro4 NOTIFY settingsChanged)
  Q_PROPERTY(
      int targetFps READ targetFps WRITE setTargetFps NOTIFY settingsChanged)
  Q_PROPERTY(int waterfallRate READ waterfallRate WRITE setWaterfallRate NOTIFY
                 settingsChanged)
  Q_PROPERTY(int waterfallTimeSpanSeconds READ waterfallTimeSpanSeconds WRITE
                 setWaterfallTimeSpanSeconds NOTIFY settingsChanged)
  Q_PROPERTY(int spectrumDisplayMode READ spectrumDisplayMode WRITE
                 setSpectrumDisplayMode NOTIFY settingsChanged)
  Q_PROPERTY(bool automaticRange READ automaticRange WRITE setAutomaticRange
                 NOTIFY settingsChanged)
  Q_PROPERTY(double lowerBoundDb READ lowerBoundDb WRITE setLowerBoundDb NOTIFY
                 settingsChanged)
  Q_PROPERTY(double upperBoundDb READ upperBoundDb WRITE setUpperBoundDb NOTIFY
                 settingsChanged)
  Q_PROPERTY(double automaticRangeSpanDb READ automaticRangeSpanDb WRITE
                 setAutomaticRangeSpanDb NOTIFY settingsChanged)
  Q_PROPERTY(bool waterfallNoiseSuppression READ waterfallNoiseSuppression WRITE
                 setWaterfallNoiseSuppression NOTIFY settingsChanged)
  Q_PROPERTY(double waterfallNoiseMarginDb READ waterfallNoiseMarginDb WRITE
                 setWaterfallNoiseMarginDb NOTIFY settingsChanged)
  Q_PROPERTY(bool showCwGuide READ showCwGuide WRITE setShowCwGuide NOTIFY
                 settingsChanged)
  Q_PROPERTY(double cwGuideCenterHz READ cwGuideCenterHz WRITE
                 setCwGuideCenterHz NOTIFY settingsChanged)
  Q_PROPERTY(double cwGuideWidthHz READ cwGuideWidthHz WRITE setCwGuideWidthHz
                 NOTIFY settingsChanged)
  Q_PROPERTY(int averagingFrames READ averagingFrames WRITE setAveragingFrames
                 NOTIFY settingsChanged)
  Q_PROPERTY(
      bool showGrid READ showGrid WRITE setShowGrid NOTIFY settingsChanged)
  Q_PROPERTY(bool showSpectrumGestureHints READ showSpectrumGestureHints WRITE
                 setShowSpectrumGestureHints NOTIFY settingsChanged)
  Q_PROPERTY(int decodedSignalTimeoutSeconds READ decodedSignalTimeoutSeconds
                 WRITE setDecodedSignalTimeoutSeconds NOTIFY settingsChanged)
  Q_PROPERTY(bool decodeWeakSignals READ decodeWeakSignals WRITE
                 setDecodeWeakSignals NOTIFY settingsChanged)
  Q_PROPERTY(double minimumDecodeSnrDb READ minimumDecodeSnrDb WRITE
                 setMinimumDecodeSnrDb NOTIFY settingsChanged)
  Q_PROPERTY(bool localDecoderEnabled READ localDecoderEnabled WRITE
                 setLocalDecoderEnabled NOTIFY settingsChanged)
  Q_PROPERTY(
      bool callsignDatabaseCorrectionEnabled READ
          callsignDatabaseCorrectionEnabled WRITE
              setCallsignDatabaseCorrectionEnabled NOTIFY settingsChanged)
  // Stored by name rather than by position so the preference survives another
  // technique being added to the list or the list being reordered.
  Q_PROPERTY(QString keyingModel READ keyingModel WRITE setKeyingModel NOTIFY
                 settingsChanged)
  Q_PROPERTY(int debugCaptureMaximumSeconds READ debugCaptureMaximumSeconds
                 WRITE setDebugCaptureMaximumSeconds NOTIFY settingsChanged)
  Q_PROPERTY(QString operatorRole READ operatorRole WRITE setOperatorRole NOTIFY
                 settingsChanged)
  Q_PROPERTY(QString localDecoderModelPath READ localDecoderModelPath NOTIFY
                 settingsChanged)
  Q_PROPERTY(QString localDecoderMetadataPath READ localDecoderMetadataPath
                 NOTIFY settingsChanged)
  Q_PROPERTY(bool localDecoderBackendAvailable READ localDecoderBackendAvailable
                 CONSTANT)
  Q_PROPERTY(
      QString localDecoderStatus READ localDecoderStatus NOTIFY settingsChanged)
  Q_PROPERTY(bool localCallsignDatabaseEnabled READ localCallsignDatabaseEnabled
                 WRITE setLocalCallsignDatabaseEnabled NOTIFY settingsChanged)
  Q_PROPERTY(QString localCallsignDatabasePath READ localCallsignDatabasePath
                 NOTIFY settingsChanged)
  Q_PROPERTY(
      QString localCallsignDatabaseStatus READ localCallsignDatabaseStatus
          NOTIFY localCallsignDatabaseChanged)
  // Spots published by other receivers. They are corroboration and never
  // authority: a spot may show that somebody else reported a station near a
  // frequency, and it may never replace, rewrite, or auto-fill a decoded
  // callsign. These describe how a spot is held and drawn once it has
  // arrived, whatever carried it; dxClusterEnabled below is the switch that
  // decides whether any arrive at all.
  Q_PROPERTY(int dxSpotsRetentionMinutes READ dxSpotsRetentionMinutes WRITE
                 setDxSpotsRetentionMinutes NOTIFY settingsChanged)
  Q_PROPERTY(int dxSpotsToleranceHz READ dxSpotsToleranceHz WRITE
                 setDxSpotsToleranceHz NOTIFY settingsChanged)
  Q_PROPERTY(bool dxSpotsShowLabels READ dxSpotsShowLabels WRITE
                 setDxSpotsShowLabels NOTIFY settingsChanged)
  // The live telnet cluster / reverse-beacon link, and the single switch for
  // the whole spot feature: it both joins the node and shows what the node
  // sends. There is no separate "show spots" toggle, because a joined node
  // whose spots were hidden would send a callsign to somebody else's machine
  // for nothing. Off by default: a cluster login puts the operator's callsign,
  // unencrypted, on that machine, and the settings page says what joining
  // sends before the switch can be reached.
  Q_PROPERTY(bool dxClusterEnabled READ dxClusterEnabled WRITE
                 setDxClusterEnabled NOTIFY settingsChanged)
  // Index into dxClusterServers. -1 selects the operator's own host and port
  // instead, so a node that is not on the shipped list is still reachable
  // without editing a file.
  Q_PROPERTY(int dxClusterServerIndex READ dxClusterServerIndex WRITE
                 setDxClusterServerIndex NOTIFY settingsChanged)
  Q_PROPERTY(QString dxClusterCustomHost READ dxClusterCustomHost WRITE
                 setDxClusterCustomHost NOTIFY settingsChanged)
  Q_PROPERTY(int dxClusterCustomPort READ dxClusterCustomPort WRITE
                 setDxClusterCustomPort NOTIFY settingsChanged)
  // There is no separate cluster login: the station callsign is the operator's
  // identity and it is already configured, so asking for it twice would only
  // create two values that can disagree on somebody else's server. A station
  // with no callsign set cannot join a cluster at all, which the settings page
  // says rather than failing quietly.
  //
  // What the operator may add to it is the SSID a cluster uses to tell one of
  // their connections from another. Logging in as CALL-1 while CALL-2 is
  // already on the node is ordinary practice; without it the second
  // connection displaces the first, and the operator loses the session they
  // were already using. It is a number, 0..99, and 0 means no SSID at all.
  // Deliberately not a second callsign field: the identity on the wire stays
  // the configured station callsign.
  Q_PROPERTY(int dxClusterLoginSsid READ dxClusterLoginSsid WRITE
                 setDxClusterLoginSsid NOTIFY settingsChanged)
  // The login exactly as it will be sent: the station callsign, with the SSID
  // appended when there is one, and empty when no callsign is set. Read-only,
  // because both halves are already settings and a third editable copy of
  // them could disagree with both.
  Q_PROPERTY(QString dxClusterLoginCallsign READ dxClusterLoginCallsign NOTIFY
                 settingsChanged)
  //
  // The offered servers, read from dictionaries/dx-cluster-servers.txt. Each
  // entry carries name, host, port, source and note.
  Q_PROPERTY(QVariantList dxClusterServers READ dxClusterServers NOTIFY
                 dxClusterServersChanged)
  // A line-delimited JSON stream of station diagnostics, offered on the
  // network addresses the operator chooses so a station can be watched while
  // it runs. Off unless asked for. See diagnostics/diagnostics_server.hpp for
  // the contract; the stream is emit-only and never accepts input, which is
  // the whole reason a process that holds transmit may offer it at all.
  //
  // What binding it to a routable address publishes is the station's
  // internals -- frequencies, decoded callsigns, device identifiers,
  // transcripts -- to anything that can reach that address. That is a
  // deliberate act, so the enable below refuses any non-loopback address
  // until a token worth having is set.
  Q_PROPERTY(bool diagnosticsServerEnabled READ diagnosticsServerEnabled WRITE
                 setDiagnosticsServerEnabled NOTIFY settingsChanged)
  // Unprivileged ports only. Binding a privileged port would need this
  // process to be started with privileges it has no other reason to hold, and
  // a diagnostics stream is not a reason to acquire them.
  Q_PROPERTY(int diagnosticsServerPort READ diagnosticsServerPort WRITE
                 setDiagnosticsServerPort NOTIFY settingsChanged)
  // The shared secret a client must present. Required for every address that
  // is not loopback, and generateDiagnosticsToken() below exists so that an
  // operator is not invited to type one themselves.
  Q_PROPERTY(QString diagnosticsServerToken READ diagnosticsServerToken WRITE
                 setDiagnosticsServerToken NOTIFY settingsChanged)
  // Exactly the addresses to bind, chosen from
  // diagnosticsServerAvailableAddresses rather than guessed at: only the
  // operator knows which of their networks is the one they meant. An empty
  // list binds nothing and needs no token; the listening indicator, not a
  // refusal here, is what tells the operator that a service they switched on
  // came up on no address.
  Q_PROPERTY(QStringList diagnosticsServerAddresses READ
                 diagnosticsServerAddresses WRITE setDiagnosticsServerAddresses
                     NOTIFY settingsChanged)
  // Which computers may connect at all: one entry per line, each a host
  // (`192.168.1.50`), a CIDR network (`192.168.1.0/24`, `2001:db8::/32`) or
  // the single word `any`. Handed to DiagnosticsServer::setAllowedPeers, which
  // owns the rule; nothing is decided here.
  //
  // Empty by default, and an empty list is meaningful rather than missing: it
  // permits loopback only. That is why enabling the service with an empty list
  // is not an error. An operator who binds a routable address and leaves this
  // empty gets a port that admits nobody, which is the safe outcome and is
  // what the settings page and the service state line say.
  //
  // An entry that cannot be read permits nothing, so a typo narrows access
  // rather than widening it, and the list editor marks such a row rather than
  // leaving the operator to discover it as a service that refuses everyone.
  Q_PROPERTY(QStringList diagnosticsAllowedPeers READ diagnosticsAllowedPeers
                 WRITE setDiagnosticsAllowedPeers NOTIFY settingsChanged)
  // What this machine could bind, as maps carrying `address`,
  // `interfaceName`, `loopback` and `description`.
  //
  // Empty until refreshNetworkAddresses() is called, and it stays as it was
  // read until it is called again. Enumerating interfaces is a system call,
  // and a QML binding reading this property would make one on every repaint,
  // so the interface asks for the list when it opens the page rather than
  // having it recomputed underneath a view.
  Q_PROPERTY(QVariantList diagnosticsServerAvailableAddresses READ
                 diagnosticsServerAvailableAddresses NOTIFY
                     diagnosticsNetworkAddressesChanged)
  // Whether the waterfall is produced at all. This is a resource setting, not
  // a display preference: with it off no waterfall row is computed, no history
  // is retained and no texture is uploaded, which is what makes a station left
  // running as a diagnostics server cheap enough to leave running. Switching
  // it off to tidy the window is not what it is for -- the spectrum display
  // mode already does that without giving up the history.
  Q_PROPERTY(bool waterfallRenderingEnabled READ waterfallRenderingEnabled
                 WRITE setWaterfallRenderingEnabled NOTIFY settingsChanged)
  Q_PROPERTY(
      QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

 public:
  explicit AppSettings(QString profile_name, bool profile_was_explicit,
                       QObject* parent = nullptr);
  ~AppSettings() override;

  [[nodiscard]] QStringList referenceRigNames() const;
  [[nodiscard]] const QString& profileName() const noexcept;
  [[nodiscard]] const QStringList& availableProfiles() const noexcept;
  [[nodiscard]] bool profileSelectionRequired() const noexcept;
  [[nodiscard]] bool setupComplete() const noexcept;
  [[nodiscard]] const QStringList& serialPorts() const noexcept;
  [[nodiscard]] const QStringList& audioInputNames() const noexcept;
  [[nodiscard]] int audioInputIndex() const noexcept;
  [[nodiscard]] QString audioInputDisplayName() const;
  [[nodiscard]] bool audioInputNamesAmbiguous() const noexcept;
  [[nodiscard]] const QString& audioInputId() const noexcept;
  // The operating system's own description of the chosen input, undecorated by
  // the ordinal and default markers the list shows. This is what the input can
  // be found by after its identifier changes, so it is stored raw.
  [[nodiscard]] const QString& audioInputDeviceName() const noexcept;
  // Records an input found by name after its saved identifier vanished, and
  // writes the recovered identifier straight to storage so the next start
  // matches on the identifier again rather than recovering a second time.
  void adoptRecoveredAudioInput(const QString& encoded_id);
  [[nodiscard]] const QStringList& audioOutputNames() const noexcept;
  [[nodiscard]] int audioOutputIndex() const noexcept;
  [[nodiscard]] QString audioOutputDisplayName() const;
  [[nodiscard]] const QString& audioOutputId() const noexcept;
  [[nodiscard]] QStringList receiverInputTypeNames() const;
  [[nodiscard]] int receiverInputTypeIndex() const noexcept;
  [[nodiscard]] int preferredSourceMode() const noexcept;
  [[nodiscard]] bool sdrBackendAvailable() const noexcept;
  [[nodiscard]] const QString& sdrBackendVersion() const noexcept;
  [[nodiscard]] const QStringList& sdrModuleNames() const noexcept;
  [[nodiscard]] const QStringList& sdrDeviceNames() const noexcept;
  [[nodiscard]] int sdrDeviceIndex() const noexcept;
  [[nodiscard]] const QString& sdrDeviceId() const noexcept;
  [[nodiscard]] QString sdrDeviceDisplayName() const;
  [[nodiscard]] const QStringList& sdrOperatingModeNames() const noexcept;
  [[nodiscard]] int sdrOperatingModeIndex() const noexcept;
  [[nodiscard]] const QString& sdrDiagnostic() const noexcept;
  [[nodiscard]] qulonglong sdrCenterFrequencyHz() const noexcept;
  [[nodiscard]] int sdrSampleRateHz() const noexcept;
  [[nodiscard]] int sdrBandwidthHz() const noexcept;
  [[nodiscard]] const QVariantList& sdrSampleRateOptions() const noexcept;
  [[nodiscard]] const QVariantList& sdrBandwidthOptions() const noexcept;
  [[nodiscard]] const QStringList& sdrAntennaNames() const noexcept;
  [[nodiscard]] int sdrAntennaIndex() const noexcept;
  [[nodiscard]] const QString& sdrAntenna() const noexcept;
  [[nodiscard]] qulonglong sdrDecoderCenterFrequencyHz() const noexcept;
  [[nodiscard]] int sdrDecoderBandwidthHz() const noexcept;
  // The bounds of the decode region, wherever it is set from -- the settings
  // list, a drag across the spectrum, or a value restored from disk.
  //
  // The ceiling is 24 kHz because the region is not only decoded, it is also
  // what an operator listens to: it is demodulated to audio for the monitor
  // and for the remote stream, and audio at 48 kHz carries 24 kHz of
  // bandwidth and no more. A wider region could be decoded and never heard,
  // which would quietly make one setting mean two different things depending
  // on which half of the application was asked. Kept here rather than written
  // at each of the four places that bound this value, because a limit that
  // exists in four copies is a limit that will eventually disagree with
  // itself.
  static constexpr int kMinimumSdrDecoderBandwidthHz = 2'000;
  static constexpr int kMaximumSdrDecoderBandwidthHz = 24'000;
  [[nodiscard]] bool sdrFollowRadioVfo() const noexcept;
  [[nodiscard]] int sdrTuningStepHz() const noexcept;
  [[nodiscard]] QString sdrRadioSyncStatus() const;
  [[nodiscard]] qint64 sdrRadioLoOffsetHz() const noexcept;
  [[nodiscard]] bool sdrAutomaticGain() const noexcept;
  [[nodiscard]] bool sdrAutomaticGainAvailable() const noexcept;
  [[nodiscard]] double sdrGainDb() const noexcept;
  [[nodiscard]] double sdrMinimumGainDb() const noexcept;
  [[nodiscard]] double sdrMaximumGainDb() const noexcept;
  [[nodiscard]] QString sdrWidePassbandSummary() const;
  [[nodiscard]] bool audioDcRejection() const noexcept;
  [[nodiscard]] bool audioAutomaticGain() const noexcept;
  [[nodiscard]] double audioGainDb() const noexcept;
  [[nodiscard]] double audioAutomaticGainTargetDbfs() const noexcept;
  [[nodiscard]] bool audioAutomaticBandwidth() const noexcept;
  [[nodiscard]] double audioLowerFrequencyHz() const noexcept;
  [[nodiscard]] double audioUpperFrequencyHz() const noexcept;
  [[nodiscard]] bool audioInputRadioLinked() const noexcept;
  [[nodiscard]] const QString& ownCallsign() const noexcept;
  [[nodiscard]] bool omniRigAvailable() const noexcept;
  [[nodiscard]] bool radioEnabled() const noexcept;
  [[nodiscard]] QString radioDisplayName() const;
  [[nodiscard]] const QStringList& detectedRadioNames() const noexcept;
  [[nodiscard]] int detectedRadioIndex() const noexcept;
  [[nodiscard]] int referenceRigIndex() const noexcept;
  [[nodiscard]] int frequencyBackendIndex() const noexcept;
  [[nodiscard]] int radioTuningStepHz() const noexcept;
  [[nodiscard]] bool radioFrequencyWritable() const noexcept;
  [[nodiscard]] bool radioTxFrequencyWritable() const noexcept;
  [[nodiscard]] bool radioTxFrequencySyncAvailable() const noexcept;
  [[nodiscard]] bool radioPointedTxFrequencyAvailable() const noexcept;
  // Says, in the status line, why pointing TX did nothing.
  //
  // The gesture is guarded because enabling split on a radio that does not
  // offer it is a command that should not be sent speculatively. Guarded and
  // silent, though, it reads as the feature being broken rather than as the
  // radio not offering it, so the refusal is now spoken.
  Q_INVOKABLE void reportPointedTxUnavailable();
  [[nodiscard]] bool radioRxModeWritable() const noexcept;
  [[nodiscard]] bool radioTxModeWritable() const noexcept;
  [[nodiscard]] bool radioSplitWritable() const noexcept;
  [[nodiscard]] QString radioRxMode() const;
  [[nodiscard]] QString radioTxMode() const;
  [[nodiscard]] QString radioTxModeTarget() const;
  [[nodiscard]] bool radioTxModeConfirmed() const noexcept;
  [[nodiscard]] QString radioRxVfo() const;
  [[nodiscard]] QString radioTxVfo() const;
  [[nodiscard]] qulonglong radioTxVfoFrequencyHz() const noexcept;
  [[nodiscard]] bool radioSplitKnown() const noexcept;
  [[nodiscard]] int omniRigSlot() const noexcept;
  [[nodiscard]] const QString& hamlibHost() const noexcept;
  [[nodiscard]] int hamlibPort() const noexcept;
  [[nodiscard]] const QString& hamlibRxVfo() const noexcept;
  [[nodiscard]] const QString& hamlibTxVfo() const noexcept;
  [[nodiscard]] bool hamlibWritable() const noexcept;
  [[nodiscard]] QString hamlibState() const;
  [[nodiscard]] bool hamlibCanWrite() const noexcept;
  [[nodiscard]] const QString& cat4omUrl() const noexcept;
  [[nodiscard]] const QString& cat4omRadioId() const noexcept;
  [[nodiscard]] const QString& cat4omPassword() const noexcept;
  [[nodiscard]] QString cat4omState() const;
  [[nodiscard]] QString cat4omFrequencySummary() const;
  [[nodiscard]] bool cat4omCanWrite() const noexcept;
  [[nodiscard]] const QString& catPort() const noexcept;
  [[nodiscard]] QVariantList supportedCatBaudRates() const;
  [[nodiscard]] int catBaudRate() const noexcept;
  [[nodiscard]] int catDataBits() const noexcept;
  [[nodiscard]] int catParityIndex() const noexcept;
  [[nodiscard]] int catStopBits() const noexcept;
  [[nodiscard]] int catFlowControlIndex() const noexcept;
  [[nodiscard]] int pollIntervalMs() const noexcept;
  [[nodiscard]] int timeoutMs() const noexcept;
  [[nodiscard]] bool splitEnabled() const noexcept;
  [[nodiscard]] qint64 rxTransverterOffsetHz() const noexcept;
  [[nodiscard]] qint64 txTransverterOffsetHz() const noexcept;
  [[nodiscard]] int cwToneSidebandIndex() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> controlledRxRfHz() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> controlledTxRfHz() const noexcept;
  [[nodiscard]] bool controlledSplitActive() const noexcept;
  [[nodiscard]] const QString& keyingPort() const noexcept;
  [[nodiscard]] bool directKeyingEnabled() const noexcept;
  [[nodiscard]] bool directKeyingValidated() const noexcept;
  [[nodiscard]] const QString& directKeyingAcceptanceStatus() const noexcept;
  [[nodiscard]] int pttLineIndex() const noexcept;
  [[nodiscard]] int keyLineIndex() const noexcept;
  [[nodiscard]] bool pttActiveHigh() const noexcept;
  [[nodiscard]] bool keyActiveHigh() const noexcept;
  [[nodiscard]] int txSpeedMode() const noexcept;
  [[nodiscard]] int fixedTxWpm() const noexcept;
  [[nodiscard]] const QString& txMacro1() const noexcept;
  [[nodiscard]] const QString& txMacro2() const noexcept;
  [[nodiscard]] const QString& txMacro3() const noexcept;
  [[nodiscard]] const QString& txMacro4() const noexcept;
  [[nodiscard]] int targetFps() const noexcept;
  [[nodiscard]] int waterfallRate() const noexcept;
  [[nodiscard]] int waterfallTimeSpanSeconds() const noexcept;
  [[nodiscard]] int spectrumDisplayMode() const noexcept;
  [[nodiscard]] bool automaticRange() const noexcept;
  [[nodiscard]] double lowerBoundDb() const noexcept;
  [[nodiscard]] double upperBoundDb() const noexcept;
  [[nodiscard]] double automaticRangeSpanDb() const noexcept;
  [[nodiscard]] bool waterfallNoiseSuppression() const noexcept;
  [[nodiscard]] double waterfallNoiseMarginDb() const noexcept;
  [[nodiscard]] bool showCwGuide() const noexcept;
  [[nodiscard]] double cwGuideCenterHz() const noexcept;
  [[nodiscard]] double cwGuideWidthHz() const noexcept;
  [[nodiscard]] int averagingFrames() const noexcept;
  [[nodiscard]] bool showGrid() const noexcept;
  [[nodiscard]] bool showSpectrumGestureHints() const noexcept;
  [[nodiscard]] int decodedSignalTimeoutSeconds() const noexcept;
  [[nodiscard]] bool decodeWeakSignals() const noexcept;
  [[nodiscard]] double minimumDecodeSnrDb() const noexcept;
  [[nodiscard]] bool localDecoderEnabled() const noexcept;
  [[nodiscard]] bool callsignDatabaseCorrectionEnabled() const noexcept;
  [[nodiscard]] const QString& keyingModel() const noexcept;
  [[nodiscard]] int debugCaptureMaximumSeconds() const noexcept;
  [[nodiscard]] const QString& operatorRole() const noexcept;
  [[nodiscard]] const QString& localDecoderModelPath() const noexcept;
  [[nodiscard]] const QString& localDecoderMetadataPath() const noexcept;
  [[nodiscard]] bool localDecoderBackendAvailable() const noexcept;
  [[nodiscard]] QString localDecoderStatus() const;
  [[nodiscard]] bool localCallsignDatabaseEnabled() const noexcept;
  [[nodiscard]] const QString& localCallsignDatabasePath() const noexcept;
  [[nodiscard]] const QString& localCallsignDatabaseStatus() const noexcept;
  [[nodiscard]] int dxSpotsRetentionMinutes() const noexcept;
  [[nodiscard]] int dxSpotsToleranceHz() const noexcept;
  [[nodiscard]] bool dxSpotsShowLabels() const noexcept;
  [[nodiscard]] bool dxClusterEnabled() const noexcept;
  [[nodiscard]] int dxClusterServerIndex() const noexcept;
  [[nodiscard]] const QString& dxClusterCustomHost() const noexcept;
  [[nodiscard]] int dxClusterCustomPort() const noexcept;
  [[nodiscard]] int dxClusterLoginSsid() const noexcept;
  [[nodiscard]] QString dxClusterLoginCallsign() const;
  [[nodiscard]] const QVariantList& dxClusterServers() const noexcept;
  [[nodiscard]] bool diagnosticsServerEnabled() const noexcept;
  [[nodiscard]] int diagnosticsServerPort() const noexcept;
  [[nodiscard]] const QString& diagnosticsServerToken() const noexcept;
  [[nodiscard]] const QStringList& diagnosticsServerAddresses() const noexcept;
  [[nodiscard]] const QStringList& diagnosticsAllowedPeers() const noexcept;
  [[nodiscard]] const QVariantList& diagnosticsServerAvailableAddresses()
      const noexcept;
  [[nodiscard]] bool waterfallRenderingEnabled() const noexcept;
  [[nodiscard]] const QString& statusMessage() const noexcept;

  void setFrequencyBackendIndex(int value);
  void setReceiverInputTypeIndex(int value);
  void setPreferredSourceMode(int value);
  void setSdrCenterFrequencyHz(qulonglong value);
  void setSdrSampleRateHz(int value);
  void setSdrBandwidthHz(int value);
  void setSdrDecoderCenterFrequencyHz(qulonglong value);
  void setSdrDecoderBandwidthHz(int value);
  void setSdrFollowRadioVfo(bool value);
  void setSdrTuningStepHz(int value);
  void setSdrRadioLoOffsetHz(qint64 value);
  void setSdrAutomaticGain(bool value);
  void setSdrGainDb(double value);
  void setRadioTuningStepHz(int value);
  void setAudioDcRejection(bool value);
  void setAudioAutomaticGain(bool value);
  void setAudioGainDb(double value);
  void setAudioAutomaticGainTargetDbfs(double value);
  void setAudioAutomaticBandwidth(bool value);
  void setAudioLowerFrequencyHz(double value);
  void setAudioUpperFrequencyHz(double value);
  void setAudioInputRadioLinked(bool value);
  void setOwnCallsign(const QString& value);
  void setRadioEnabled(bool value);
  void setOmniRigSlot(int value);
  void setHamlibHost(const QString& value);
  void setHamlibPort(int value);
  void setHamlibRxVfo(const QString& value);
  void setHamlibTxVfo(const QString& value);
  void setHamlibWritable(bool value);
  void setCat4omUrl(const QString& value);
  void setCat4omRadioId(const QString& value);
  void setCat4omPassword(const QString& value);
  void setCatPort(const QString& value);
  void setCatBaudRate(int value);
  void setCatDataBits(int value);
  void setCatParityIndex(int value);
  void setCatStopBits(int value);
  void setCatFlowControlIndex(int value);
  void setPollIntervalMs(int value);
  void setTimeoutMs(int value);
  void setSplitEnabled(bool value);
  void setRxTransverterOffsetHz(qint64 value);
  void setTxTransverterOffsetHz(qint64 value);
  void setCwToneSidebandIndex(int value);
  void setKeyingPort(const QString& value);
  void setDirectKeyingEnabled(bool value);
  void setPttLineIndex(int value);
  void setKeyLineIndex(int value);
  void setPttActiveHigh(bool value);
  void setKeyActiveHigh(bool value);
  void setTxSpeedMode(int value);
  void setFixedTxWpm(int value);
  void setTxMacro1(const QString& value);
  void setTxMacro2(const QString& value);
  void setTxMacro3(const QString& value);
  void setTxMacro4(const QString& value);
  void setTargetFps(int value);
  void setWaterfallRate(int value);
  void setWaterfallTimeSpanSeconds(int value);
  void setSpectrumDisplayMode(int value);
  void setAutomaticRange(bool value);
  void setLowerBoundDb(double value);
  void setUpperBoundDb(double value);
  void setAutomaticRangeSpanDb(double value);
  void setWaterfallNoiseSuppression(bool value);
  void setWaterfallNoiseMarginDb(double value);
  void setShowCwGuide(bool value);
  void setCwGuideCenterHz(double value);
  void setCwGuideWidthHz(double value);
  void setAveragingFrames(int value);
  void setShowGrid(bool value);
  void setShowSpectrumGestureHints(bool value);
  void setDecodedSignalTimeoutSeconds(int value);
  void setDecodeWeakSignals(bool value);
  void setMinimumDecodeSnrDb(double value);
  void setLocalDecoderEnabled(bool value);
  void setCallsignDatabaseCorrectionEnabled(bool value);
  void setKeyingModel(const QString& value);
  void setDebugCaptureMaximumSeconds(int value);
  void setOperatorRole(const QString& value);
  void setLocalCallsignDatabaseEnabled(bool value);
  void setDxSpotsRetentionMinutes(int value);
  void setDxSpotsToleranceHz(int value);
  void setDxSpotsShowLabels(bool value);
  void setDxClusterEnabled(bool value);
  void setDxClusterServerIndex(int value);
  void setDxClusterCustomHost(const QString& value);
  void setDxClusterCustomPort(int value);
  void setDxClusterLoginSsid(int value);
  void setDiagnosticsServerEnabled(bool value);
  void setDiagnosticsServerPort(int value);
  void setDiagnosticsServerToken(const QString& value);
  void setDiagnosticsServerAddresses(const QStringList& value);
  void setDiagnosticsAllowedPeers(const QStringList& value);
  void setWaterfallRenderingEnabled(bool value);

  Q_INVOKABLE void selectReferenceRig(int index);
  Q_INVOKABLE void resetToReferenceDefaults();
  Q_INVOKABLE void refreshSerialPorts();
  Q_INVOKABLE void refreshAudioInputs();
  Q_INVOKABLE void selectAudioInput(int index);
  Q_INVOKABLE void refreshAudioOutputs();
  Q_INVOKABLE void selectAudioOutput(int index);
  Q_INVOKABLE void refreshSdrDevices();
  // True from the moment enumeration begins until its result has been applied.
  // The interface shows a waiting state on it, which is only meaningful
  // because the scan no longer runs on the thread that draws.
  [[nodiscard]] bool sdrDiscoveryRunning() const noexcept;
  // Asks for one enumeration at startup, so a saved receiver is selectable
  // without the operator opening the settings page first. Called by the
  // application once the interface exists; the constructor and load() still
  // probe no hardware.
  //
  // Does nothing at all unless this profile actually saved a receiver, so a
  // station that only ever used sound-card audio pays nothing for it.
  //
  // When there was something to look for, it answers exactly once with
  // sdrSelectionRestored(found, savedDeviceName): after the enumeration has
  // been applied, or -- in a build carrying no SDR backend to enumerate with
  // -- as soon as the event loop runs, never inside this call.
  Q_INVOKABLE void restoreSdrSelectionAtStartup();
  Q_INVOKABLE void setSdrDecoderWindow(qulonglong center_frequency_hz,
                                       int bandwidth_hz);
  Q_INVOKABLE bool requestSdrRxFrequencyHz(qulonglong frequency_hz);
  Q_INVOKABLE void stepSdrRxFrequency(int direction);
  void followSdrToRadioVfo();
  Q_INVOKABLE void selectSdrDevice(int index);
  Q_INVOKABLE void selectSdrOperatingMode(int index);
  Q_INVOKABLE void selectSdrAntenna(int index);
  Q_INVOKABLE void refreshDetectedRadios();
  Q_INVOKABLE void selectDetectedRadio(int index);
  Q_INVOKABLE bool apply();
  Q_INVOKABLE bool selectLocalDecoderModel(const QUrl& url);
  Q_INVOKABLE bool selectLocalDecoderMetadata(const QUrl& url);
  Q_INVOKABLE void clearLocalDecoderModel();
  Q_INVOKABLE void clearLocalDecoderMetadata();
  Q_INVOKABLE bool selectLocalCallsignDatabase(const QUrl& url);
  Q_INVOKABLE void clearLocalCallsignDatabase();
  Q_INVOKABLE bool reloadLocalCallsignDatabase();
  Q_INVOKABLE bool completeSetup();
  Q_INVOKABLE bool selectProfile(const QString& profile_name);
  Q_INVOKABLE bool createProfile(const QString& profile_name);
  Q_INVOKABLE void showOmniRigConfiguration();
  Q_INVOKABLE void connectHamlib();
  Q_INVOKABLE void disconnectHamlib();
  Q_INVOKABLE bool runDirectKeyingLoopback(bool radio_disconnected_confirmed);
  Q_INVOKABLE void testCat4omConnection();
  Q_INVOKABLE void connectCat4omControl();
  Q_INVOKABLE void disconnectCat4om();
  Q_INVOKABLE void requestCat4omOwnership();
  Q_INVOKABLE bool setControlledRxFrequency(const QString& value,
                                            qulonglong unit_hz);
  Q_INVOKABLE bool stepControlledRxFrequency(int direction);
  Q_INVOKABLE bool setControlledTxFrequency(const QString& value,
                                            qulonglong unit_hz);
  Q_INVOKABLE bool setControlledTxFrequencyHz(qulonglong value);
  Q_INVOKABLE bool syncControlledTxFrequencyToRx();
  Q_INVOKABLE bool cycleControlledRxMode();
  Q_INVOKABLE bool toggleControlledTxMode();
  Q_INVOKABLE bool setControlledSplit(bool enabled);
  // Re-reads the machine's network interfaces into
  // diagnosticsServerAvailableAddresses. Called when the diagnostics page is
  // opened or its refresh is pressed, never from a property read.
  Q_INVOKABLE void refreshNetworkAddresses();
  // The network one of this machine's addresses sits on, as a CIDR rule --
  // `192.168.1.42` on a /24 answers `192.168.1.0/24` -- or an empty string
  // when this machine cannot say. Forwards to
  // DiagnosticsServer::localSegmentForAddress, which owns the answer.
  //
  // Offered so the allowed-peer editor can put the operator's own segment into
  // the list as a line they can then read, edit or delete. It returns the text
  // rather than adding anything: a rule deciding who may read the station has
  // to be one the operator can see, so nothing is written here.
  [[nodiscard]] Q_INVOKABLE QString localNetworkForAddress(
      const QString& address) const;
  // A fresh token from the system entropy source, long enough to satisfy
  // DiagnosticsServer::isAcceptableToken. Returned rather than stored, so the
  // operator sees what they are about to save. It exists because a field that
  // merely demands a token gets `password` typed into it.
  Q_INVOKABLE QString generateDiagnosticsToken();

 signals:
  void settingsChanged();
  void serialPortsChanged();
  void audioInputsChanged();
  void audioOutputsChanged();
  void receiverInputTypeChanged();
  void sdrSettingsChanged();
  void sdrDiscoveryRunningChanged();
  // Whether the receiver saved in this profile came back. `found` is true
  // when the saved variant id, or the same physical receiver under another
  // operating mode, is present in the enumeration that
  // restoreSdrSelectionAtStartup() asked for; `savedDeviceName` is the
  // persisted sdr/deviceName, so a warning can name the receiver the operator
  // recognises rather than an opaque driver id. Emitted once per requested
  // restore, and only for a requested one.
  //
  // This reports; it does not act. Choosing another source because the
  // receiver is missing is the application's decision, not this object's.
  void sdrSelectionRestored(bool found, const QString& savedDeviceName);
  void statusMessageChanged();
  void setupCompleteChanged();
  void profileChanged();
  void profilesChanged();
  void profileSelectionRequiredChanged();
  void cat4omChanged();
  void hamlibChanged();
  void radioFrequencyChanged();
  void radioFrequencyControlChanged();
  void detectedRadiosChanged();
  void localDecoderConfigurationCommitted(bool enabled,
                                          const QString& model_path,
                                          const QString& metadata_path);
  void localCallsignDatabaseChanged();
  void localCallsignDatabaseConfigurationCommitted(
      bool enabled, const QString& database_path);
  void dxClusterServersChanged();
  void diagnosticsNetworkAddressesChanged();

 private:
  void load();
  void applyReferenceDefaults(int index);
  void setStatusMessage(QString message);
  [[nodiscard]] QString storageKey(const QString& relative) const;
  [[nodiscard]] static QString normalizeProfileKey(const QString& name);
  void refreshProfiles();
  void resetInMemorySettings();
  // True when the chosen addresses reach past this machine and the token is
  // not one worth having. The single place the enable guard is decided, so the
  // setters, apply() and load() cannot come to disagree about what is allowed.
  [[nodiscard]] bool diagnosticsServerExposureUnguarded() const;
  void refreshSelectedSdrCapabilities();
  void rebuildSdrDeviceModes(const QString& preferred_variant_id = {},
                             const QString& preferred_mode_id = {});
  // Applies an enumeration result. Separated from the scan itself so the
  // scan can run on a pooled thread while this stays on the thread that owns
  // the state it writes.
  void applySdrDiscoveryReport(const SdrDiscoveryReport& report);
  // Delivers the one answer a requested startup restore is owed, naming the
  // receiver in the status line when it did not come back. Does nothing
  // unless a restore is outstanding, so every completion path may call it.
  void answerSdrStartupRestore(bool found);
  void refreshControlledFrequency();
  void reconcilePendingRxFrequency();
  void rememberPendingRxFrequency(std::uint64_t frequency_hz);
  [[nodiscard]] std::optional<std::uint64_t> observedRadioRxRfHz() const noexcept;
  // Commits the decode region to disk at the moment it is chosen, instead of
  // waiting for the settings dialog's Apply.
  //
  // The region is mostly set from the main window -- a CTRL+RIGHT drag across
  // the spectrum, a right-click that points it -- and that window has no Apply
  // button, so a value only written by apply() was lost by exactly the restart
  // it was supposed to survive: a dragged width worked until the application
  // closed and then snapped back to whatever Apply had last stored.
  //
  // Both keys go out together because the region is one thing. Writing only
  // the one that changed would leave a stored centre from one selection beside
  // a width from another, and the pair is read back as a pair.
  //
  // Exactly these two keys are written and nothing else, so a half-finished
  // edit elsewhere on the settings page stays uncommitted. This follows
  // setPreferredSourceMode, which already writes its single key through for
  // the same reason; it is deliberately not a general "persist on every
  // setter" rule.
  void persistSdrDecoderWindow();
  void setSdrRadioWindow(std::uint64_t rx_frequency_hz);
  bool writeRadioRxDialFrequency(std::uint64_t dial_frequency_hz);
  void invalidateDirectKeyingAcceptance(QString status);
  [[nodiscard]] std::optional<cwassistant::core::ResolvedFrequencies>
  resolvedControlledFrequencies() const noexcept;
  bool writeControlledRxDialFrequency(std::uint64_t dial_frequency_hz);
  bool requestControlledTxRfFrequency(std::uint64_t rf_frequency_hz);
  bool writeControlledTxDialFrequency(std::uint64_t dial_frequency_hz);
  bool writeControlledMode(cwassistant::core::RadioMode mode, bool tx);
#ifdef Q_OS_WIN
  [[nodiscard]] bool ensureOmniRigAutomation();
  bool writeOmniRigRxFrequency(std::uint64_t dial_frequency_hz);
  bool writeOmniRigTxFrequency(std::uint64_t dial_frequency_hz);
  bool writeOmniRigMode(cwassistant::core::RadioMode mode);
  bool writeOmniRigSplit(bool enabled);
#endif

  QString profile_name_;
  QString profile_storage_key_;
  QStringList available_profiles_;
  bool profile_selection_required_{false};
  bool setup_complete_{false};
  QStringList serial_ports_;
  QStringList audio_input_names_;
  QStringList audio_input_ids_;
  // The undecorated operating-system descriptions, index-aligned with the two
  // lists above, so the raw name of a selection can be recovered from its row.
  QStringList audio_input_device_names_;
  bool audio_input_names_ambiguous_{false};
  QString audio_input_id_;
  QString audio_input_name_;
  QString audio_input_device_name_;
  QStringList audio_output_names_;
  QStringList audio_output_ids_;
  QString audio_output_id_;
  QString audio_output_name_;
  int receiver_input_type_index_{0};
  int preferred_source_mode_{0};
  bool sdr_backend_available_{false};
  QString sdr_backend_version_;
  QStringList sdr_module_names_;
  QStringList sdr_device_names_;
  QStringList sdr_device_ids_;
  bool sdr_discovery_running_{false};
  QStringList sdr_device_mode_names_;
  QStringList sdr_device_mode_ids_;
  QStringList sdr_device_mode_keys_;
  struct SdrModeChoice {
    QString physical_id;
    QString variant_id;
    QString variant_name;
    QString mode_id;
    QString mode_name;
    bool recommended{false};
  };
  QList<SdrModeChoice> sdr_discovered_modes_;
  QString sdr_physical_device_id_;
  QString sdr_device_mode_id_;
  QString sdr_device_id_;
  QString sdr_device_name_;
  // Written only by restoreSdrSelectionAtStartup(), read only by the
  // enumeration it asks for. The persisted name and a label for it are kept
  // here because applying a discovery report overwrites sdr_device_name_ with
  // whatever was found, and a warning about a receiver that did not come back
  // still has to be able to name it.
  bool sdr_startup_restore_pending_{false};
  bool sdr_startup_restore_found_{false};
  QString sdr_startup_restore_device_name_;
  QString sdr_startup_restore_device_label_;
  QString sdr_diagnostic_{
      QStringLiteral("SDR discovery has not run. Open the SDR settings page or "
                     "press Refresh devices; live audio remains available.")};
  qulonglong sdr_center_frequency_hz_{14'050'000ULL};
  int sdr_sample_rate_hz_{250'000};
  int sdr_bandwidth_hz_{0};
  QVariantList sdr_sample_rate_options_;
  QVariantList sdr_bandwidth_options_;
  QStringList sdr_antenna_names_;
  QString sdr_antenna_;
  qulonglong sdr_decoder_center_frequency_hz_{14'050'000ULL};
  int sdr_decoder_bandwidth_hz_{24'000};
  bool sdr_follow_radio_vfo_{false};
  int sdr_tuning_step_hz_{1'000};
  qint64 sdr_radio_lo_offset_hz_{0};
  bool sdr_automatic_gain_{true};
  bool sdr_automatic_gain_available_{true};
  double sdr_gain_db_{30.0};
  double sdr_minimum_gain_db_{-100.0};
  double sdr_maximum_gain_db_{100.0};
  bool audio_dc_rejection_{true};
  bool audio_automatic_gain_{false};
  double audio_gain_db_{0.0};
  double audio_automatic_gain_target_dbfs_{-12.0};
  bool audio_automatic_bandwidth_{true};
  double audio_lower_frequency_hz_{100.0};
  double audio_upper_frequency_hz_{3'000.0};
  bool audio_input_radio_linked_{false};
  QString own_callsign_;
  bool radio_enabled_{false};
  QStringList detected_radio_names_;
  QList<int> detected_radio_slots_;
  int reference_rig_index_{0};
  int frequency_backend_index_{0};
  int radio_tuning_step_hz_{1'000};
  cwassistant::core::RadioMode radio_tx_mode_target_{
      cwassistant::core::RadioMode::Cw};
  int omnirig_slot_{1};
  QString hamlib_host_{QStringLiteral("127.0.0.1")};
  int hamlib_port_{4'532};
  QString hamlib_rx_vfo_{QStringLiteral("VFOA")};
  QString hamlib_tx_vfo_{QStringLiteral("VFOB")};
  bool hamlib_writable_{false};
  QString cat4om_url_{QStringLiteral("ws://127.0.0.1:5001/")};
  QString cat4om_radio_id_;
  QString cat4om_password_;
  QString cat_port_;
  int cat_baud_rate_{4'800};
  int cat_data_bits_{8};
  int cat_parity_index_{0};
  int cat_stop_bits_{1};
  int cat_flow_control_index_{0};
  int poll_interval_ms_{500};
  int timeout_ms_{4'000};
  bool split_enabled_{false};
  qint64 rx_transverter_offset_hz_{0};
  qint64 tx_transverter_offset_hz_{0};
  int cw_tone_sideband_index_{0};
  QString keying_port_;
  bool direct_keying_enabled_{false};
  bool direct_keying_validated_{false};
  QString direct_keying_acceptance_sha256_;
  QString direct_keying_acceptance_platform_;
  qint64 direct_keying_acceptance_utc_seconds_{0};
  QString direct_keying_acceptance_status_{
      QStringLiteral("Physical loopback has not been measured.")};
  int ptt_line_index_{0};
  int key_line_index_{1};
  bool ptt_active_high_{true};
  bool key_active_high_{true};
  int tx_speed_mode_{0};
  int fixed_tx_wpm_{20};
  QString tx_macro_1_{QStringLiteral("TU")};
  QString tx_macro_2_{QStringLiteral("AGN")};
  QString tx_macro_3_{QStringLiteral("PSE K")};
  QString tx_macro_4_{QStringLiteral("73")};
  int target_fps_{60};
  int waterfall_rate_{60};
  int waterfall_time_span_seconds_{10};
  int spectrum_display_mode_{0};
  bool automatic_range_{true};
  double lower_bound_db_{-120.0};
  double upper_bound_db_{-20.0};
  double automatic_range_span_db_{60.0};
  bool waterfall_noise_suppression_{true};
  double waterfall_noise_margin_db_{6.0};
  bool show_cw_guide_{true};
  double cw_guide_center_hz_{700.0};
  double cw_guide_width_hz_{200.0};
  int averaging_frames_{3};
  bool show_grid_{true};
  bool show_spectrum_gesture_hints_{true};
  int decoded_signal_timeout_seconds_{30};
  // Whether a tracked signal is decoded regardless of how weak it is. Off by
  // default: below the threshold the decoder receives fragments rather than
  // copy, which fills a transcript with nothing while spending a whole
  // decoder's processor time on each such track. A signal under the threshold
  // is still detected, followed and drawn in the spectrum either way; only its
  // decoding is withheld, so nothing disappears from the operator's display.
  bool decode_weak_signals_{false};
  // The default is measured rather than chosen. Across the capture corpus
  // the weakest track that carried a correctly recovered callsign
  // measured 19.5 dB, so 12 dB leaves over seven decibels of margin before the
  // threshold could cost the operator a station that was genuinely readable.
  double minimum_decode_snr_db_{4.0};
  bool local_decoder_enabled_{false};
  // Off by default. Two listed stations can differ by one character, so a
  // correction can name a station that was never heard; the operator opts in.
  bool callsign_database_correction_enabled_{false};
  QString keying_model_{QStringLiteral("adaptive-threshold")};
  int debug_capture_maximum_seconds_{300};
  QString operator_role_{QStringLiteral("monitor")};
  QString local_decoder_model_path_;
  QString local_decoder_metadata_path_;
  bool local_callsign_database_enabled_{false};
  QString local_callsign_database_path_;
  QString local_callsign_database_status_{
      QStringLiteral("Disabled. No local callsign list is in use.")};
  // How long a spot stays worth showing. Fifteen minutes is long enough for a
  // station to still be working the pile-up it was spotted in and short enough
  // that the display does not fill with stations that have long since gone.
  int dx_spots_retention_minutes_{15};
  // How far from a decoded signal a spot may sit and still be about the same
  // station. A CW signal is a few hundred hertz wide once drift and the
  // spotter's own tuning are counted, so 250 Hz corroborates without sweeping
  // in the neighbouring station.
  int dx_spots_tolerance_hz_{250};
  bool dx_spots_show_labels_{true};
  // Off by default. Nothing is connected, and no callsign leaves this machine,
  // until the operator turns this on having read what it sends.
  bool dx_cluster_enabled_{false};
  // The first offered server. -1 means the custom host and port below.
  int dx_cluster_server_index_{0};
  QString dx_cluster_custom_host_;
  // The DXSpider default. It is only a starting point for a typed-in node;
  // nothing is contacted until a host is supplied as well.
  int dx_cluster_custom_port_{7'300};
  // 0 means no SSID, which is what a single connection wants: the login is the
  // bare station callsign. 1..99 appends `-N`, so a second connection from the
  // same station joins beside the first instead of replacing it.
  int dx_cluster_login_ssid_{0};
  // Read from data rather than compiled in, so a node that has moved can be
  // corrected without a new build. Loaded once; the file is not per profile.
  QVariantList dx_cluster_servers_;
  // Off unless asked for. Nothing is bound, and no station internal is
  // readable from anywhere, until the operator turns this on having read what
  // it publishes.
  bool diagnostics_server_enabled_{false};
  // DiagnosticsServer::kDefaultPort. Repeated as a literal so this header need
  // not include the server; a static_assert in the translation unit fails if
  // the two ever disagree.
  //
  // Clear of the neighbourhoods a station already occupies: rigctld is 4532
  // and rotctld 4533, and this application can itself be a rigctld client;
  // cluster nodes sit on 23, 7300, 7373 and 8000, and reverse-beacon telnet
  // on 7000 and 7001. Registered range deliberately, not ephemeral: a
  // listener above 49152 can collide with the ports the operating system
  // hands this same process for its own outgoing connections.
  int diagnostics_server_port_{17'300};
  QString diagnostics_server_token_;
  // Loopback by default: the only default that is immediately useful and
  // still unreachable from another machine.
  QStringList diagnostics_server_addresses_{QStringLiteral("127.0.0.1")};
  // Empty by default, which the server reads as loopback only. Deliberately
  // not seeded with the machine's own segment: a rule admitting a whole
  // network must be one the operator wrote and can see in the list, never one
  // that appeared behind them.
  QStringList diagnostics_allowed_peers_;
  // Machine data rather than profile data, like the cluster server list above:
  // read on demand and left alone when a profile is switched underneath it.
  QVariantList diagnostics_server_available_addresses_;
  // On by default, because the waterfall is why most operators opened this.
  bool waterfall_rendering_enabled_{true};
  QString status_message_;
  void* omnirig_automation_{nullptr};
  bool com_initialized_{false};
  bool com_initialization_attempted_{false};
  std::unique_ptr<Cat4OmClient> cat4om_client_;
  std::unique_ptr<HamlibRigctldClient> hamlib_client_;
  std::unique_ptr<QMediaDevices> media_devices_;
  QTimer radio_frequency_timer_;
  QTimer radio_frequency_request_timer_;
  QElapsedTimer omnirig_capability_refresh_clock_;
  std::optional<std::uint64_t> pending_rx_rf_hz_;
  int pending_frequency_backend_index_{-1};
  std::optional<std::uint64_t> omnirig_rx_dial_hz_;
  cwassistant::core::RadioState radio_state_;
  cwassistant::core::OmniRigRxFrequencyTarget omnirig_rx_write_target_{
      cwassistant::core::OmniRigRxFrequencyTarget::None};
  // OmniRig exposes only the currently selected Mode, not ModeA/ModeB.
  // Remember each VFO only when it has actually been observed as RX; never
  // copy one VFO's mode into the other faceplate field.
  std::array<cwassistant::core::RadioMode, 2> omnirig_vfo_a_modes_{};
  std::array<cwassistant::core::RadioMode, 2> omnirig_vfo_b_modes_{};
};

}  // namespace cwassistant::desktop
