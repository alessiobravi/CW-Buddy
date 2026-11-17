# Product requirements baseline

Version: 0.1, 2026-08-30

## Operating scope

CW Buddy receives either demodulated audio or SDR IQ, discovers CW signals
within the visible passband, decodes independently of one another those tracked
signals that reach the configured decoding level, and shows candidates on a
spectrum/waterfall. The operator can select a decoded callsign and, after
confirmation, conduct a guided CW QSO.

The initial workflows are ordinary QSO, DX pileup, and contest. Workflows use
editable structured panels rather than unrestricted scripts at first. A panel
defines fields, exchange steps, macros, validation, and transitions; this makes
operator behavior inspectable and prevents arbitrary code from controlling TX.
Conversation profiles must be distinct: ordinary/general CW remains open-ended,
while every contest profile versions its own rule-derived exchange grammar and
decoding hints rather than sharing one assumed contest sequence.

The receiver workspace is to offer four first-level operating modes in a left
navigation rail: **Standard**, **PileUp Chaser**, **PileUp Slicer**, and
**Runner**. None of them is implemented yet and the application has no mode
selector, so an operator cannot choose one today; current behavior is what
Standard describes. Their behavior, adaptive-learning lifecycle,
frequency-control authority, and fail-closed rules are specified in
[`operating-modes.md`](operating-modes.md). A mode may change interpretation or
request an explicitly enabled VFO B position; it never grants transmission
authority. The conversation roles and conversation kinds that the core already
carries are a different mechanism and do not implement these modes.

## Functional requirements

### Receive and DSP

- Enumerate and select audio input devices.
- Configure sample rate, input channel, block size, latency preference, and
  maximum simultaneously decoded channels.
- Provide independently selectable DC rejection, bounded manual/automatic
  input gain, automatic-gain target, and automatic/manual processing bandwidth;
  keep these controls separate from visualization range scaling.
- Accept mono audio and complex IQ through the same timestamped block contract.
- Detect candidate tones using a shared spectral analysis stage.
- Keep spectral candidates private until configurable prominence, persistence,
  narrowband coherence, keyed-edge/spacing cadence, Morse-symbol validity,
  unknown-output, timing-quality, and character-confidence gates verify a CW
  trace. Track candidate, Morse-likely, verified, and lost states with
  inspectable rejection reasons. Qualification must use bounded recent
  evidence, require sustained entry, and continuously re-evaluate verified
  tracks with demotion hysteresis. Unverified candidates must not receive
  overlays, session rows, or a detected-signal count.
- Permit overlap-confirmed local character-model evidence to satisfy the
  symbol/timing/character portion of verification only after the same track
  independently passes spectral persistence, narrowband coherence, keyed-edge,
  and spacing-cadence gates. Require a structurally valid decoded callsign and
  the ordinary sustained-entry interval; model output must not create a track,
  keep silence active, replace raw text, or reach transmission control.
- Track frequency drift and maintain separate timing/decoder state per channel.
- Preserve soft tone/envelope/timing evidence and combine an explainable
  adaptive timing decoder with an optional compact causal learned likelihood
  model; decoding must continue when that model is unavailable.
- Keep the technique that decides keying replaceable and operator-selectable.
  Every technique must emit the same run description so that element assembly,
  verification, and callsign policy are independent of the choice; an
  explainable deterministic technique must remain available and selected by
  default, and selecting a technique must never be required for decoding to
  work.
- Run a low-latency causal pass plus bounded delayed refinement passes over a
  rolling narrowband buffer. Later passes may revise only visibly provisional
  text and must never block capture.
- For co-channel pileups, use sub-bin frequency/phase, WPM, element/spacing
  cadence, edge shape, and fading as probabilistic operator fingerprints. Joint
  separation and reconstruction/cancellation must retain the original mixture
  and expose ambiguity when the input is not identifiable.
- Permit conservative strongest-track reconstruction/cancellation before a
  weak-track retry; reject the retry unless both residual and decode scores
  improve.
- Schedule candidates by signal strength, arrival order, or explicit operator
  selection. Manual selection always has a configurable priority boost.
- Decide by measured level which tracked signals are decoded rather than
  decoding everything that is tracked. A track whose own peak keying level
  stays below the configured decoding threshold is still acquired, followed,
  and drawn in the spectrum, but it is neither filtered to baseband nor fed to
  a decoder; it is not suspended, because suspension means association loss.
  The gate reads the strongest level a track has reached rather than the level
  of the moment, since a signal is weak while it is still being acquired, and
  it applies only after a fixed warm-up during which every track is filtered. A
  monitored or operator-selected track is always decoded, and an operator
  setting decodes weak signals unconditionally at the cost of the saved work.
- Display decoded text, estimated WPM, tone frequency, SNR, confidence, and
  callsign candidates without blocking capture.
- Display a callsign on a trace only after the CW track is verified. Classical
  stable text requires a structurally valid complete token terminated by a
  word gap plus exchange-role context or exact repetition. Separately labeled,
  overlap-confirmed model consensus may expose its structurally valid call
  without a language/context promotion. An unconfirmed lone call-shaped token
  is not sufficient.
- Distinguish raw evidence, provisional text, stable text, and context-derived
  suggestions. Store per-character pass/evidence provenance and calibrate
  confidence on held-out recordings.
- Provide an explicitly enabled, bounded diagnostic capture mode that can
  correlate raw/conditioned audio, spectra, private candidate decisions,
  decoded character evidence, timing/frequency references, and pipeline
  overruns. Require review and redact credentials/private identifiers before
  export.
- Optionally predict and validate partial decoded callsigns against a pinned,
  checksummed list. The main decoding workspace controls this in real time;
  suggestions must remain visibly separate from raw decoder output and expose
  confidence and list provenance. Bound acoustic-to-span substitutions,
  insertions, and deletions; abstain on ambiguity and never let list membership
  displace stronger acoustic evidence. Absence from a list is never proof of an
  invalid callsign.
- Permit an explicitly enabled managed offline list to discover provider
  metadata over HTTPS, check no more than daily unless requested by the
  operator, and install only a bounded, checksum-matching, structurally valid
  dataset by atomic replacement. Preserve the last valid cache after every
  network, validation, interruption, or write failure, and never bundle a
  provider dataset without redistribution permission.
- Keep callsign data behind a provider interface. Settings select provider
  roles, precedence, update policy, and credentials so fast offline completion,
  jurisdiction-limited official validation, and optional directory enrichment
  can be combined without hard-coding a logger or service.
- Permit a receive-only DX-cluster/RBN provider to rank callsign alternatives
  when checked actual-RF frequency, mode, spot age, and acoustic edit cost all
  agree. Display the provider, spotter, age, frequency delta, and rationale;
  never let a spot overwrite raw text, independently verify a CW stream, resolve
  an acoustically inseparable co-channel sender, post data, or authorize TX.
- Replay WAV and SigMF/IQ recordings deterministically.
- Provide configurable noise blanking, AGC, key-click suppression, audio mute,
  and a sharp continuously adjustable CW monitor filter without coupling these
  controls to decoder correctness.
- Provide monitor-off, complete-receiver, and selected-track audio modes with a
  chosen local output device and level. Selected-track audio must reuse the
  decoder's own carrier-following center/filter, re-pitch to the configured CW
  tone, remain bounded in latency, and become silent when its track is lost.
- Automatically calibrate or manually correct frequency offset and I/Q
  gain/phase imbalance for quadrature inputs, with visible diagnostics and a
  resettable calibration state.

### Display

- Spectrum and waterfall share the same FFT output used by channel detection.
- Provide profile-persisted **Audio spectrum** and **CW symbols** views. The
  former may use averaged FFT power; the latter uses only verified channels'
  acoustic keying envelopes on a neutral background with crisp sampling to
  expose dit/dah/gap timing. Full-passband and unverified noise belongs only in
  Audio spectrum. Switching views must not reset or influence decoding, and CW
  symbols must not fabricate text.
- Render the configured CW-width as two dashed vertical boundaries around
  authoritative VFO B/TX readback, with no filled guide band. Map absolute RF
  directly for SDR and use the checked sideband-aware RX/RF-to-audio mapping
  for sound-card input. Follow physical-radio VFO/frequency/split changes and
  hide unknown, replay, or off-screen TX state rather than retaining a stale
  position. Keep the guide visually distinct from the stable-width area of
  active verified signal traces;
  keep vertical decoded annotations in
  the spectrum region rather than over waterfall history.
- Keep verified-marker geometry stable while the decoder changes its internal
  carrier estimate and analysis-filter width; expose those choices as
  diagnostics, not as a jittering or resizing carrier footprint. Correct an
  initially biased presentation center from robust first-verification evidence,
  then follow only sustained coherent carrier motion with a deadband, slew
  limit, dispersion/drift gates, and an absolute immutable-identity bound.
  Move all frequency states together for a known receiver retune.
- Render a retained inactive observation without a filled area, using only a
  short identity-colored horizontal mark on the frequency axis. Keep its
  stream label readable at normal size and enlarge it further on hover.
  Retention must never assert current carrier/key-down state or generate a CW
  symbol without a current matched spectral observation.
- Clicking a trace opens its decoded-text session in the decoder pane. Keep an
  operator-opened session attached across a same-frequency/color tracker
  reacquisition; clicking a callsign opens the QSO confirmation panel.
- Treat alternating operators on one simplex carrier as one frequency session,
  not two artificial signal tracks. A completed `CALL1 DE CALL2` handover may
  attach both distinct, structurally plausible participants to that session;
  segment bounded transmission turns only after sustained silence or explicit
  end-of-input. A shorter channel-association loss must not split a slow word
  gap. Current-sender attribution requires explicit, unambiguous final handover
  evidence; separate sender cadence state may influence only an already
  acoustically compatible timing estimate.
- Contextual presentation may separate completed turns and reconstruct bounded
  word gaps only among acoustically competitive alternatives with identical
  non-whitespace characters. It must not alter raw/consensus text, confirmed
  callsigns, verification, or transmission decisions.
- Follow appended decoder text unless the operator is selecting earlier text,
  emphasize a confirmed remote callsign, and raise a bounded visual alert when
  stable text contains an exact complete-token match for the active profile's
  own callsign. This receive alert must not initiate or arm transmission.
- Card dismissal and card reordering must use independent pointer targets;
  closing a session leaves its channel DSP active and the marker available for
  reopening. Action buttons expose contextual hover help, and spectrum hover
  explains the distinct left/right/Ctrl pointer actions.
- Keep per-card monitor and guarded-TX actions in a stable action row and
  operable during transcript, signal-activity, and model refreshes. Opening a
  card must not enable monitoring; a missing callsign must not be represented
  by placeholder text or exposed as a callsign TX target.
- Keep the transcript border fixed outside the scrolling content, with
  explicit style-independent edge padding. Place guarded TX at the left of the
  lower action row and expose per-stream listening as a matching full-size
  **Monitor** button with a speaker-state icon.
- Auto-collapse the bottom live-control panel when it is not being used, while
  retaining a visible reveal header and an operator-controlled pinned state.
- Left-clicking an identified stream opens its decoder card. Right-clicking an
  unmarked spectrum/waterfall frequency starts a neutral temporary decoder
  region without moving the independent TX-slice guide or retuning the radio. The
  region follows qualified carrier movement through normal bounded tracking
  and expires after the configured stream timeout unless promoted by ordinary
  CW verification.
- Permit exact RX-frequency entry by activating the live VFO readout and
  provide stable down/up controls at the waterfall edges using a configurable,
  persisted step (1 kHz default). Enable them only for a linked provider with
  valid state and explicit frequency-write capability. Map displayed actual RF
  back to the provider's dial domain with checked transverter-offset arithmetic,
  target only the receive VFO, preserve split TX and mode, and leave provider
  readback authoritative. Reject invalid, unavailable, or ambiguous requests
  visibly.
- Permit capability-gated Ctrl+left-click on a live spectrum/waterfall to map
  the pointed coordinate to checked exact RF and request it on VFO B/TX through
  the provider-neutral command boundary, enabling split when required and
  supported. Move the TX guide only after authoritative readback; ordinary
  left-click remains decoder-open and right-click remains manual-probe. This
  gesture must not arm TX, assert PTT/KEY, or bypass existing radio safety.
- Keep provider observations and operator control targets as distinct
  provider-neutral state for every radio backend. VFO A/RX and VFO B/TX retain
  independent frequency and mode state. The operator TX-mode target is always
  an explicit persisted CW or CW-R value; matching known provider readback is
  required before it is marked confirmed. Missing capability or readback must
  neither make the target unusable nor fabricate a hardware mode. Each adapter
  applies and confirms only the operations it advertises, and a future TX
  arming gate must reject an unconfirmed required mode.
- Provide an explicit VFO frequency-sync action that copies checked RX actual
  RF to the independent TX endpoint through advertised TX-frequency and split
  capabilities. Apply the TX transverter offset independently and never copy
  RX mode; the TX target remains CW/CW-R.
- Permit an explicit operator-selected decoder probe at a pointed audio
  frequency. It opens a clearly identified manual session but does not assign
  a verified color/callsign, increment detected-signal counts, or expose text
  until normal acoustic evidence qualifies it; probes are bounded in number
  and frequency motion and remain cancellable. Once every ordinary gate passes,
  promote the probe once into the normal verified/detected-stream lifecycle.
  Clicking historical waterfall pixels starts listening now and does not imply
  historical audio replay.
- Anchor decoded observations to checked absolute RF Hz and stable track IDs,
  not screen coordinates. Continued decoding updates an observation in place;
  configurable loss and expiry timers remove stale overlays and list entries
  consistently while permitting short-gap reacquisition. Preserve a verified
  frequency's visual color identity for at least five minutes, independently
  of the shorter marker/session expiry, and carry it through a known VFO
  retune; a later pass at that carrier must reuse the color.
- Bound drift extrapolation across missing evidence, bridge ordinary Morse word
  gaps with short presentation hysteresis, and preserve bounded decoded text
  when the same retained identity receives a new internal tracker ID. Clear
  the confirmed callsign on source-track replacement until the replacement's
  own acoustic evidence establishes the station identity.
- Rendering is independently rate-limited (initial target 30 or 60 FPS) and may
  drop display frames. DSP sample blocks must not be dropped to keep UI current.
- Zoom, dynamic range, palette, averaging, CW filter width, and tone pitch are
  configurable.
- Configure target render FPS and waterfall line/scroll rate independently.
- Present frequently adjusted visualization values as labeled sliders with
  live numeric readouts in a responsive multi-row layout; do not require
  repeated number-box editing for normal spectrum operation.
- Keep the displayed waterfall history on a constant configurable time axis.
  Pane resizing, startup fill, and missing source intervals must not stretch or
  collapse dit/dah timing; line rate changes temporal resolution, not duration.
- Generate genuine waterfall timing frames with bounded overlapping analysis;
  never duplicate a spectrum row merely to satisfy a requested line rate.
- Provide a toggleable TX-slice guide driven by authoritative VFO B/TX
  readback and labeled frequency ticks on the X axis. The guide is a visual
  reference only and must never select, constrain, reset, or otherwise drive
  decoding.
  Configure automatic/manual range, lower and upper dB bounds, averaging, peak
  hold/decay, palette, color gain, black level, contrast/gamma, grid, label
  density/font, spectrum height, zoom, and pan. Invalid combinations are
  clamped and defaults may be selected from measured machine performance.
- Hovering for a configurable delay, or pressing and holding, opens a callsign
  detail card without starting a QSO. Available details include normalized call,
  confidence, decoded context, SNR, tone/absolute frequency, WPM, first/last
  heard time, queue position, CQ state, worked-before/log status, DXCC/prefix,
  country, and ignore status. Enrichment may arrive asynchronously.
- Use a functional 2D display only. A 3D spectrum and ornamental GPU effects are
  explicit non-goals; rendering features must improve operation, diagnostics,
  accessibility, or performance.
- Provide an expandable operator workspace rather than a fixed decoder layout:
  spectrum, waterfall, band map, active calls, frequency-sliced decoder list,
  watch list, QSO workflow, logging, and diagnostics are independent dockable
  panels.
- Track calls across frequency changes and inactivity, show CQ/running/searching
  state, and color decoded tokens by semantic type and confidence using an
  accessible palette.
- Support keyboard and pointer tuning at an exact frequency, direct selection of
  a tracked station, next/previous signal and band navigation, and visual panning
  without unnecessary CAT retunes.

### Radio and transmission

- Store multiple named transceiver profiles and switch only while TX is idle.
- Each profile specifies Hamlib model, CAT serial settings, and an independent
  serial keying profile.
- Every frequency-control adapter exposes RX and TX VFO state and split
  capability through the shared core contract. Unsupported split requests fail
  explicitly; they never silently collapse to simplex.
- Store independent signed RX and TX transverter offsets in hertz. UI and logs
  distinguish radio dial frequency from calculated actual RF frequency, reject
  overflow/zero results, and show both before satellite transmission.
- Support selecting RTS or DTR and active polarity independently for PTT and
  KEY. A profile may use one or two physical serial ports.
- Require a measured disconnected-line physical loopback before direct keying
  can be armed. The operator must first confirm that the radio is physically
  disconnected; the application then observes the configured output/input
  pairs electrically and stores only configuration-bound evidence. Clear it
  whenever enablement, port, line assignment, polarity, platform, or
  fingerprint changes.
  Loopback readiness is not a replacement for dummy-load acceptance.
- Require authoritative provider readback of the exact TX frequency, CW/CW-R
  target mode, and known split state before arming. Snapshot that state and
  disarm while idle or emergency-release while active if it changes.
- Start disarmed on every launch and after every device reconnect.
- Require operator confirmation of the exact selected callsign before the first
  transmission in a QSO.
- Permit contest/profile macros and optional suggested or automatic replies only
  after the active conversation profile and required exchange fields validate.
  Automatic reply is separately enabled and armed, previews the exact message,
  remains cancellable, and cannot bypass exact-call confirmation, maximum-key-
  down, emergency release, or any other transmit guard.
- Permit operator-authored free text only after normalization to the supported
  Morse alphabet and exact visible-preview confirmation. Derive a bounded
  standard-timing plan at the selected WPM before any adapter request.
- Show authoritative elapsed/remaining time and bounded progress while sending
  a message or tuning. Profile quick macros and editable report/exchange fields
  only prepare the same exact-confirmation preview and never auto-send.
- Provide an operator-only TUNE toggle after explicit TX arming. TUNE asserts
  KEY/tone without accepting decoder input, releases on the second press or
  emergency stop, and has an independent hard maximum of 15 seconds.
- Show ON AIR only from authoritative guarded KEY state, never from a queued
  message, decoder suggestion, CAT frequency, or optimistic UI transition.
- In split-pileup search-and-pounce mode, permit an explicit checked retune that
  places the runner inside the configured 700 Hz guide and lays out the pileup
  on its receive-frequency side. Quiet-slot and latest-completed-QSO frequency
  hints remain advisory, expire with evidence, abstain on ambiguity, and require
  explicit operator confirmation before changing TX frequency.
- Release KEY then PTT on timeout, adapter error, device removal, workflow
  failure, or emergency stop.
- Never perform serial discovery by toggling RTS/DTR on unknown ports.
- A callsign on the persistent ignore list is ineligible for display, queueing,
  QSO confirmation, and transmission. TX safety rechecks the policy at request,
  confirmation, and keying time. Initial rules are normalized exact matches;
  wildcard/prefix rules are out of scope until their ambiguity is designed.
- Initial reference radios are Yaesu FT-450D and FT-818/FT-818ND. Their supplied
  direct-serial defaults remain part of the profile schema, but a provider UI
  exposes them only when that provider actually owns a local serial transport.
  Baud is selected from 1200, 2400, 4800, 9600, 19200, 38400, 57600, or 115200;
  data bits, parity, stop bits, and flow control are explicit bounded choices.
- On Windows, OmniRig Rig 1/Rig 2 is the first frequency-control integration and
  its native configuration is reachable from the application Settings pane.
  Hamlib rigctld provides a platform-neutral frequency-control path, requires
  VFO mode, accepts loopback endpoints only, and exposes no PTT/KEY operation.
  A remote rigctld connection requires a locally terminated authenticated,
  encrypted tunnel.
- CAT4OM is a network frequency-control backend using its native major-version
  compatible JSON WebSocket Control channel. It consumes pushed state, treats
  VFO names as opaque identifiers, honors group ownership, and never exposes
  CAT4OM PTT/CW operations around the local transmit-safety boundary.
- Provider setup is capability-specific in Settings and the station wizard.
  OmniRig exposes its slot and native setup only; rigctld exposes its loopback
  endpoint, VFO mapping and write permission; CAT4OM exposes its Control service
  identity. None duplicates serial settings owned by another process.

### Configuration and instances

- First launch of every station profile opens a guided wizard covering radio,
  CAT alternatives, direct key/PTT, receive source, display defaults, logging,
  and a non-transmitting configuration review.
- If multiple profiles are present and none was explicitly selected, show a
  startup profile chooser with create/select actions. A command-line profile
  override remains available for shortcuts, headless servers, and automation.
- Every named station profile isolates radio, keying, audio, SDR, UI, logging,
  workflow, and remote settings. Multiple processes may run concurrently with
  different profiles.
- Acquire OS-level resource ownership locks before opening physical audio, SDR,
  or serial devices. A second profile receives a clear conflict diagnostic and
  may not steal an active device.
- Profiles define ordered station-equipment rules over canonical ADIF bands.
  Each rule can name the radio, transverter/converter chain, and antenna; the
  first matching actual-RF band wins. Cross-band QSOs preserve distinct TX and
  RX equipment descriptions.

### Logging

- Produce ADIF 3.1.7-compatible QSO records.
- Treat ADIF conformance as a release gate and follow the current specification,
  field dependencies, enumerations, deprecation policy, official resources,
  schemas, and test fixtures as detailed in the conformance policy.
- For split and satellite QSOs, calculate exact actual-RF transmit and receive
  frequencies from dial values and signed transverter offsets. Export consistent
  `FREQ`, `FREQ_RX`, `BAND`, `BAND_RX`, `PROP_MODE=SAT`, `SAT_NAME`, and
  `SAT_MODE` values as applicable.
- Derive logging-station `MY_RIG` and `MY_ANTENNA` from the station profile and
  calculated actual-RF bands. Never populate contacted-station `RIG` from local
  configuration. Cross-band descriptions identify TX and RX chains explicitly.
- First integration: Log4OM 2 inbound ADIF message over configurable UDP host
  and port. TCP and other logger protocols are separate future adapters.
- Queue unsent records locally and make retries visible; never silently report
  a QSO as logged.

### Operational compatibility

- Provide a frequency band map, verified callsign list, operator watch list,
  editable regional band plans, and an option to decode only designated CW
  segments.
- Validate candidate callsigns using configurable strictness, allocation and
  syntax rules, watch/ignore state, and optional contest master-call data.
- Export verified spots through a compatible read-only DX-cluster TCP service,
  optionally restrict output to CQ callers, and default legacy plaintext
  services to loopback unless the operator deliberately exposes them.
- Export timestamped spectrum frames by configurable UDP for logger/contest
  integrations while keeping this adapter independent of the renderer.
- Record and replay audio or I/Q in interoperable WAV/RF64 form with UTC,
  operator, station, center-frequency, channel-mapping, and software metadata;
  rotate safely, loop playback, inspect metadata, and preserve deterministic
  decoder replay.
- Support automatic receive startup per profile, a decode-disabled monitoring
  mode, multiple receivers through profiles/instances, and visible health
  indicators for CAT, input bandwidth, CPU pressure, overruns, and calibration.

### SDR

- Use SoapySDR as the device-neutral API.
- Bundle the redistributable RTL-SDR receive runtime with official portable
  packages, declare distribution dependencies where appropriate, and prove the
  module factory loads before publication.
- Support SDRplay 3 through an externally installed compatible vendor API and
  module. Keep proprietary vendor drivers out of CW Buddy packages, preserve
  their system discovery paths, and show actionable installation diagnostics.
- Keep the adapter receive-only: device discovery, center frequency, sample
  rate, hardware RF bandwidth, antenna/input, RX gain and CF32 stream reads must
  not expose SDR transmit, PTT, or KEY.
- Present SDR and decoder center-frequency editing in VFO-style kHz while
  preserving exact checked integer-Hz values at the settings and driver
  boundaries.
- Preserve the complete acquired passband for an operator overview while a
  separately tunable, anti-aliased and decimated 6–96 kHz window bounds carrier
  detection and per-stream decoding. Overview frame production must remain
  bounded at multi-megasample input rates. **Not met:** the implemented window
  is bounded to 2–24 kHz. The ceiling follows from the selected-region monitor
  being 48 kHz mono audio, which carries 24 kHz of bandwidth and no more, so
  widening the window past 24 kHz requires either a higher monitor rate or a
  decode window that the operator cannot listen to. Tracked as `SDR-006`.
- Support zoom and pan as presentation-only viewport operations. Display zoom
  must not silently change acquisition bandwidth or decoder workload; the
  selected decoder window remains visibly identified.
- Allow optional decoder-window following from authoritative RX-VFO readback
  through the common radio-provider boundary, with a bounded signed SDR LO
  offset. Unknown or stale radio state must not cause a speculative SDR retune.
- Group several configurations exported for one driver/serial as operating
  modes of one physical receiver, while retaining the exact provider identity
  required to open the selected mode. Device ordering must not define identity.
- Provide an operational SDR faceplate independent of the CAT radio faceplate.
  It must expose exact RX-frequency editing and stepping plus only
  capability-backed mode, input, IQ-rate/decimation and RF-bandwidth controls.
  A center-only live change must use backend retuning without reopening the
  stream; changes to device, mode, stream format or route may restart it.
- Optional bidirectional SDR/radio synchronization must separate an operator
  command from provider readback, apply the configured LO offset consistently,
  suppress stale asynchronous echoes, never loop writes, and never address the
  TX VFO, PTT or KEY. Moving only the decoder window remains local to DSP.
- Keep RX-source selection independent from radio control. Selecting direct SDR
  must not hide or disconnect a configured CAT radio that supplies VFO state
  and a separately guarded TX/keying endpoint for full-duplex operation.
- Validate fixed IQ blocks, preserve absolute RF, report device overflow and
  pipeline overrun separately, and stop safely on malformed/non-finite input.
- Never route raw IQ to an audio output. Full-passband listening applies only
  to audio sources; an SDR monitor must select one or more independently
  filtered CW streams and convert only those streams to bounded audio.

### Network SDR

- Present a searchable receiver directory with receiver name, type, country,
  location/grid, frequency coverage, availability, and connection capability.
- Filter by required frequency/band, geography, protocol, and availability.
- Cache directory results and refresh conservatively according to each
  provider's terms and rate limits. Do not scrape an undocumented directory.
- First direct-stream protocol: KiwiSDR over WebSockets, supporting server-tuned
  audio and, where offered and permitted, IQ/waterfall data.
- Classic browser-only WebSDR entries open in the system browser or use a user-
  selected virtual audio device. Direct streaming is enabled only for a
  documented or explicitly authorized server interface.
- Treat OpenWebRX and other protocols as separate adapters; directory type does
  not imply stream compatibility.
- Network sources are receive-only. They never assert PTT/KEY, authorize TX, or
  silently retune a local radio. Linking a remote receive frequency to a local
  transmit rig requires an explicit operator action and frequency confirmation.
- Send an operator identity when required, honor receiver capacity/timeouts,
  use one connection per selected receiver, and reconnect with bounded backoff.

### Remote station operation

- The application runs as Standalone, Station Server, or Remote Client from a
  versioned startup profile. Server mode can run headless with a local status
  and emergency-stop console available.
- The station server exclusively owns hardware, CAT, keying, decoder state,
  logger connections, transmission timing, watchdogs, and audit records.
- Use a versioned secure WebSocket control/event protocol with verified TLS.
  Plaintext is loopback-test-only; public raw port forwarding is unsupported.
- Pair clients to a station identity and assign observer/operator/administrator
  roles. Support credential revocation and rotation without reinstalling.
- Grant one authenticated operator a short station-wide exclusive control
  lease covering every coupled RX/TX device and shared route in the station
  profile. Permit multiple authenticated receive-only clients concurrently.
  Heartbeat loss expires the lease, cancels queued TX, and releases KEY/PTT.
- Transmit complete CW messages timed by the server. Never transport paddle,
  dot/dash, KEY, or PTT edges as remote timing commands.
- Every TX request carries an idempotency ID, exact confirmed callsign, rig,
  message, WPM, and weighting and passes all local safety/policy checks.
- Stream Opus receive audio, spectrum/waterfall frames, decoded events, and
  state independently. Provide bandwidth/latency profiles and bounded queues.
- Raw IQ streaming is opt-in, capacity-controlled, and disabled by default.
- On reconnect send a full epoch/sequence snapshot, then deltas. Never restore
  an old control lease, armed state, queued TX, or confirmation implicitly.
- Record authenticated client actions, lease transitions, CAT changes, TX
  decisions, faults, and emergency stops in a bounded/exportable audit log.

## Non-functional requirements

- C++20 with CMake; warning-clean on MSVC, Clang, and GCC.
- Windows 11 or newer on x64 is the first release target. macOS Sonoma 14 or
  newer is supported on Apple silicon and Intel x64; Debian and Ubuntu are the
  supported Linux families. Earlier Windows and macOS releases are unsupported.
- Core and DSP have no dependency on Qt and are usable by tests and tools.
- No allocation, locks, logging, filesystem access, or UI calls in an audio
  callback.
- Bounded queues with explicit overflow counters; no unbounded sample queues.
- Configuration is versioned and migrations are tested.
- Hardware-free unit and replay tests are required for every DSP change.
- User-visible diagnostics expose underruns, overruns, queue depths, active
  workers, processing latency, and dropped display frames.

## Performance acceptance targets to measure

Final defaults require benchmark recordings and a reference Win64 machine. The
initial engineering targets are:

- No capture overruns during a 60-minute replay/live soak test.
- UI remains interactive while the configured decoder-channel limit is active.
- End-of-character decoded latency below 250 ms beyond the Morse timing itself.
- TX line release within 50 ms of an emergency-stop request where the OS and
  serial driver permit it.

The channel limit, FFT size, overlap, display rate, and worker count will be
selected by startup calibration, with user overrides. A safe starting worker
count is `max(1, hardware_concurrency - 2)`, capped by the configured channel
limit.

## Keying scope

These were open questions and are now fixed requirements, testable as written.

- **Speed range is 8 to 60 WPM.** The decoder evaluates nine anchors spanning
  that range and its configured bounds match it, so every accuracy figure the
  project quotes is measured across it. A signal outside the range may still
  decode; nothing outside it is a supported claim.
- **Farnsworth sending is supported on receive.** Character speed and overall
  speed are estimated separately, and a gap stretched beyond standard spacing
  does not change the character speed reported.
- **The character set is ASCII.** A to Z, 0 to 9, and
  `. , ? ' / ! ( ) & : ; = + - " $ @`, with `_` additionally decoded. Accented
  and other international characters are deliberately out of scope; the
  alphabet is a data file, so an operator may extend it locally for receive,
  but no international character is a supported claim.
- **Seven prosigns may be transmitted:** `<AR>`, `<AS>`, `<BK>`, `<CT>`,
  `<KN>`, `<SK>`, `<SOS>`. Each is keyed as one symbol, with no character gap
  between its letters. The table is closed and lives in the application rather
  than a data file, because what may be transmitted is a safety boundary. A
  bracketed token that does not name one of these is refused during
  normalization, so it can never reach a staged message.
- **A distress call is transmittable.** Sending one is legal and appropriate in
  a genuine emergency. It is not given a special exclusion, because the risk
  that matters -- transmitting one unintentionally -- is already carried by the
  gates every message passes: an armed station, exact callsign confirmation, a
  message preview, an explicit send action, and a decoder that can never
  initiate a transmission.
- **Break-in is semi break-in.** The station transmits and releases to receive
  between overs. Full break-in, receiving between individual elements, is out
  of the initial scope; it requires rig-side QSK support and decoding through
  the station's own keying, and no interface should be shaped in a way that
  forecloses it.

## Decisions still required

- Minimum supported Debian/Ubuntu versions and reference Windows 11 x64 hardware.

The WPM range, character set, prosigns and break-in scope were once listed here
and are settled in [Keying scope](#keying-scope) above.
