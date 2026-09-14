# Configuration reference

Settings are stored separately for every named station profile. All supplied
radio values are starting points and remain editable.

## Audio page

Select the operating-system audio input carrying receiver audio. The list is
populated from native Windows, macOS, or Linux audio services and refreshes when
devices are added or removed.

- **System default input (recommended):** follows the current OS default rather
  than binding the profile to one device identifier.
- **Named input:** binds the profile to that specific device.
- **Unavailable:** preserves a disconnected device selection visibly instead of
  silently changing the profile to another input. Reconnect it or select a new
  device, then use **Refresh audio inputs** if necessary.

Audio selection applies equally to a CAT-controlled radio and receive-only SWL
operation. Use **Start live RX** in the Receiver workspace to begin capture.
The application requests 48 kHz mono float when supported and otherwise uses
the device's preferred PCM format, converts it, and downmixes it to mono. Live
audio and WAV replay are explicit, separate receiver modes.

Enable **This input carries RX audio from the configured radio** only when that
physical association is true. This per-profile confirmation is required before
the receiver may combine live audio tones with a controlled radio frequency;
leaving it disabled keeps every marker explicitly labeled **AF**. It is disabled
by default so a system microphone or another receiver is never assigned a
guessed RF frequency.

### SDR LO offset and where frequencies are measured from

A direct SDR is tuned a little away from the frequency you are listening to, so
that the receiver's own local-oscillator spur does not sit on top of the signal.
The offset moves the *acquisition* centre; the decode window stays on your
frequency, and the difference is removed again before anything is decoded. Set
it in Settings -> SDR. Every frequency CW Buddy reports -- the axis, a decoded
stream, a spot -- is absolute RF measured after that offset has been taken back
out, so it should agree with your radio's dial and with the cluster.

How far the decode window may sit from the acquisition centre is bounded by the
sample rate: the window and its guard have to fit inside the acquired passband.
A larger offset, or a decode region dragged wider, can push against that bound,
and the window is then pulled just inside it rather than moved somewhere else. A
window that is not in the acquired passband at all -- a setting restored from a
different band -- falls back to the centre of what is actually being received,
because nothing about it can be recovered.

If reported frequencies ever disagree with your radio, the live diagnostics
record now names every reference they are built from: the acquisition centre
asked for beside the one the receiver reports on the samples it delivers, the
decode window requested beside the one in force, and the slice detection was
actually read at. Those five numbers localise a disagreement without guesswork.


## SDR page

- **Receiver source** stores whether the profile normally starts from
  sound-card audio or a directly connected SDR. Selecting it never starts the
  device.
- **SoapySDR backend**, **Installed modules**, and **Discovery** distinguish an
  SDR-disabled build, a missing receiver module, no attached device, and a
  successful scan. Opening this page requests one deferred scan. **Refresh
  devices** repeats discovery without opening an RX stream.
- **SDR center frequency** and **Decoder window center** use VFO-style kHz;
  `7021.43` represents 7.02143 MHz exactly. Their internal and driver boundary
  remains whole-hertz integer RF through 99 GHz, subject to the selected
  hardware.
- **IQ sample rate** requests 25 kS/s through 64 MS/s. The adapter selects the
  nearest rate advertised by the receiver and reports the actual value after
  start. The selector is populated from the selected device when possible.
  This is the effective IQ output rate: drivers such as SDRplay may use a
  higher internal converter rate and their supported hardware decimation to
  supply it. The default is 250 kS/s; increase it only when the wider overview
  is operationally useful.
- **Hardware RF bandwidth** requests the receiver's analogue or baseband
  filter width independently of the IQ sample rate. **Automatic** leaves that
  choice to the driver. Unsupported values are mapped to the nearest advertised
  width and actual hardware readback remains authoritative.
- **Antenna / tuner input** lists the inputs exposed by the selected receiver
  operating mode. A disabled selector means the driver exposes no choice.
- **Decoder window center** and **Decoder bandwidth** select the bounded RF
  region sent to CW detection and the independent stream decoders. The full
  acquired passband remains visible. The default window is 24 kHz and choices
  range from 6 to 96 kHz; use the narrowest width that contains the stations of
  interest to reduce CPU use and decoding latency.
- **SDR LO offset** adds a signed offset to the SDR hardware centre for
  transverter or independently tuned receiver arrangements. CW Buddy bounds it
  so the decoder window stays inside the acquired passband. The operational
  **Radio Sync** control is on the main SDR faceplate rather than this settings
  page.
- Selecting direct SDR changes only the RX sample source. It does not hide or
  disconnect Radio Control: a separately configured CAT radio may continue to
  provide authoritative VFO state and the guarded TX/keying endpoint for
  full-duplex operation.
- **Automatic gain** requests the receiver's hardware gain mode. A receiver
  without that capability fails explicitly until manual gain is selected.
  **Manual gain** is requested in dB and the actual readback remains
  authoritative.

Official packages compile direct SDR support on and provide the RTL-SDR module.
Windows packages also provide the SoapySDRPlay3 bridge; the independently
installed SDRplay Hardware API 3.15/service remains required. macOS and Linux
require both that vendor API/service and a compatible external SoapySDRPlay3
module. Custom builds use `CWA_ENABLE_SOAPY_SDR=ON` and matching development/
runtime modules. When a backend or device dependency is absent, discovery
reports the module load failure, the controls fail closed, and normal audio/WAV
reception is unchanged. The SDR boundary is RX-only.
CW Buddy does not probe SDR hardware during application or profile startup.
Opening Settings > SDR requests one scan after the page renders; use **Refresh
devices** to repeat it after a hot-plug or reconnect.

An RSPduo appears once by physical serial number. Its separate operating-mode
selector contains **ST** (Single Tuner, recommended), **DT** (two synchronized
receive channels), **MA** (master role with a 6 MHz device sample clock), and
**MA8** (the same master role at 8 MHz) when the driver advertises them. A
driver-controlled **SL** slave role may also appear. CW Buddy currently owns
one IQ source, spectrum, and decoder pipeline and therefore consumes RX channel
0; DT does not yet provide a second receiver pane. Use the antenna/tuner-input
selector on the main **SDR Radio Control** for routine port changes.
Close SDRUno, SDRconnect, or any other owner before probing or starting the RSP.

The main SDR faceplate also edits and steps the tuned RX frequency, changes the
selected mode, tuner/input, effective IQ rate and hardware RF bandwidth, and
enables bidirectional **Radio Sync**. SDRplay decimation is selected through the
effective IQ rate because the SoapySDR interface does not advertise it as an
independent control. Frequency-only changes retune the running stream; route or
format changes may reopen it. Physical device, operating mode, input, both
centres and bandwidths, gain, LO offset, tuning step, and Radio Sync state are
stored independently in every named profile.

The status bar exposes bounded-queue input overruns. Standard device
sample-rate, RF-bandwidth, antenna/input and gain controls are available when
advertised. Driver-specific options, additional channels, buffer-size,
calibration, and level-meter controls remain under implementation.

### Audio conditioning and bandwidth

- **Remove input DC offset** is enabled by default. It subtracts the constant
  component before each FFT and prevents a sound-card bias from appearing as a
  permanent peak at the left edge.
- **Automatic gain** is optional software DSP gain and disabled by default. It
  does not change the operating-system mixer or a receiver's hardware gain. When enabled,
  **Automatic target** selects the desired peak level from -40 to -1 dBFS. Gain
  changes are bounded to ±40 dB and smoothed between FFT frames.
- When automatic gain is disabled, **Manual gain** applies the exact selected
  value from -40 to +40 dB. Start at 0 dB and increase only if the receiver
  level is genuinely low.
- **Automatic from audio sample rate** is the default processing bandwidth. It
  selects 100–3000 Hz where the input Nyquist limit permits. This is a stable,
  CW-oriented range derived from the source format; it does not chase an
  individual signal.
- Disable automatic bandwidth to set **Lower frequency** and **Upper
  frequency** manually. Values outside the source Nyquist limit are safely
  clipped when spectrum bins are produced.

Example for a receiver whose CW pitch is 700 Hz:

```text
DC rejection: enabled
Automatic gain: disabled
Manual gain: 0 dB
Automatic bandwidth: disabled
Lower frequency: 300 Hz
Upper frequency: 1500 Hz
```

The **Display level range** on the Display page controls only how dBFS values
are mapped to the trace and waterfall colors. Its automatic mode is not audio
gain and cannot create or remove a spectral peak.

## Live spectrum controls

The compact panel immediately below the spectrum mirrors the operational
settings that need adjustment while listening. It collapses automatically to
its header when the pointer leaves, expands on hover, and can be held open with
**Pin**:

- **Signal**: DC rejection, automatic/manual software gain, gain target, and
  automatic/manual bandwidth.
- **Display**: Audio spectrum/CW symbols view, automatic/manual dBFS levels, automatic span, waterfall noise
  suppression, suppression margin, measured noise floor, FPS, line density,
constant history seconds, and the CW frequency guide.

Changes are applied immediately to active live audio and WAV replay. Select
**Save profile** to persist them. The default 60 dB automatic span anchors the
palette well above the measured floor. Audio-spectrum noise suppression
compares every bin with a slow baseline and nearby side frequencies, darkening broad receiver
texture while retaining locally prominent narrowband signals. Raw spectrum bins are retained
for future detection and decoding.

Numeric visualization controls are labeled sliders with live value readouts.
The receiver workspace arranges them over multiple responsive rows, and the
Settings page uses the same controls for precise profile editing.

Hovering the spectrum or waterfall displays the pointer contract. The gestures,
in full:

| Gesture | Action |
| --- | --- |
| Left click | Open the decoder card for an already detected stream. |
| Alt + left click | Open a manual decode at that frequency, for a signal detection has not picked up. |
| Ctrl + left click | Request the pointed exact RF on VFO B/TX through the configured provider, enabling split when needed. Capability-gated; it never arms or starts transmission, and provider readback remains authoritative. |
| Right click | Point the receive decode region at that frequency (direct SDR only). |
| Ctrl + right click or drag | Define the decode region: a drag sets centre and width, a click moves the centre and keeps the width. Direct SDR only. |
| Wheel button click | Retune the receiver so that frequency becomes the centre of the acquired spectrum. Direct SDR only. |
| Wheel button drag | Pan the visible view. |
| Wheel | Zoom about the pointer. |
| **Full span** button | Return to the complete acquired passband. |

Each gesture does one thing. Choosing where to listen does not also create a
stream, and no gesture tunes the transmitter except the one that says so. The
decode region is bounded to 2–96 kHz, sized to the nearest 100 Hz, and changes
only the receive and decoding region.
Zoom reset uses the explicit **Full span** button so a double-click cannot also
trigger normal stream selection.
Action buttons in the receiver, settings, setup, and profile views also expose
contextual hover help, including why an action is disabled where applicable.

**Waterfall history** selects a constant 5–30 second vertical time window. The
default is 10 seconds. **Lines / second** changes temporal sampling density, not
the displayed duration. Higher values request overlapping FFT hops, so the
additional rows contain new timing observations instead of copies; 60–120
lines/s makes high-speed dit/dah edges easier to inspect at a higher CPU/render
cost. Resizing and initial fill retain the chosen duration. Capture timestamp
gaps are rendered as dark rows instead of being compressed. Set **Avg** to 1–2
for the sharpest element boundaries; higher averaging deliberately smooths time.

**View** selects **Audio spectrum** or **CW symbols** and is saved per profile.
Audio spectrum uses the configured FFT power averaging. CW symbols uses the
same timestamps but draws only active verified channels' carrier-on states as
sharp three-bin marks on a neutral background; carrier-off intervals remain
blank gaps. Full-passband and unverified receiver noise is intentionally absent
and remains available in Audio spectrum. A retained marker is not active unless
the detector currently matches its peak, so residual noise during the identity
hold cannot draw symbol rows after the 750 ms word-gap bridge. **Margin** controls Audio spectrum
suppression, not this keyed raster. CW symbols is acoustic keying evidence
rather than decoded characters. Switching is immediate and does not clear
tracking, timing hypotheses, or decoded sessions.

**TX slice guide** draws two dashed red vertical boundaries around authoritative
VFO B/TX readback, using the configured width and no fill. Absolute SDR RF is
used directly; sound-card audio maps the TX/RX RF difference around the
configured CW reference tone according to sideband. Unknown, replay, or
off-screen state hides the guide. It does not select a decoder, limit channel
detection, arm TX, key the radio, or change decoder bandwidth. Seven X-axis labels show the actual
displayed audio or RF frequency.

The decoder scans the complete processed bandwidth selected under **Signal**.
It acquires sub-bin local spectral peaks privately. A peak must exceed its local
near shoulder and hertz-scaled far references, repeat in at least three
spectral observations across normal key-up gaps, and exhibit at least six keyed
transitions,
three spacing observations, and narrowband coherence. It must then decode at
least three known symbols with no more than 30% unknown output in the bounded
recent evidence window and meet the independent cadence (0.42), pure timing
(0.55), and blended mean-character-confidence (0.40) floors before it becomes a
published CW track with a stable color. Each track
maintains separate soft key evidence, timing, provisional text, stable text,
WPM, SNR, verification/rejection reason, and bounded per-character evidence.
Key evidence is calculated from original input samples
through phase-continuous 60, 120, and 240 Hz narrowband paths at 500
updates/second. The key decision itself is a likelihood ratio
between an estimated mark level and an estimated space level, taken in the
linear power domain. A responsive per-frame estimate follows fading and manual
weighting; a bounded 512 ms history may anchor it to robust low/high populations
only when they have adequate support, separation, and bimodality. Each level
also carries its own measured scatter, and
because a mark carries signal plus noise while a space carries noise alone the
decision settles nearer the mark than half way between them, which is what
stops noise excursions from producing marks on a weak signal. The ratio is
handed to the timing decoder in the form its own logistic inverts exactly, so
the probability that decoder works from is calibrated rather than a second
shaping of an already shaped number; on a channel whose two levels do not
separate, the ratio falls to zero and reports no information rather than an
implied key-up. The smoothing applied before it scales with the element length
rather than being a fixed constant, and impulsive noise is rejected by a
minimum run duration rather than by widening the decision band. Gaps are then
classified against the element length, with the boundary between an element
gap and a character gap placed nearer the character gap than half way. A gap
is not measured in clean conditions: a noise excursion inside one registers as
a mark and eats into it from both ends, so measured gaps run short, and a
boundary placed half way between the two nominal lengths breaks apart
characters that were never spaced. Raising the audio sample rate or the internal
evidence rate does not improve copy and measurably degrades it: element timing
is already oversampled at these settings, while a higher sample rate coarsens
the fixed-size FFT and a higher evidence rate shortens the integration behind
each measurement. Acquisition starts at 120 Hz. The width then follows the keying bandwidth the
signal needs, roughly 3.5 times its element rate, so 120 Hz serves speeds to
about 41 WPM and 240 Hz beyond; a drifting carrier is widened regardless. The
narrowest 60 Hz path is used only below about 15 WPM, because its settling time
is a quarter of a 20 WPM element and rounds real elements together. None of
this changes the visual guide. **Avg**,
display bounds, waterfall suppression, and the visual guide do not alter it.
Detection is deliberately separated from presentation: it reads the unaveraged
spectrum and applies its own smoothing over a fixed time constant, and it runs
on its own fixed cadence rather than once per displayed frame. Changing **Avg**
therefore produces an identical decode, and so does any **Lines / second**
setting at or above that cadence. A line rate below it supplies the detector
with fewer observations and can delay acquisition, so keep **Lines / second**
at 60 or higher while decoding matters. Changing a display control also no
longer restarts decoding: only a change to the audio reaching the detector —
processing bandwidth, DC rejection, or gain — resets tracks and transcripts.
The verified stream marker also remains at a stable 120 Hz presentation width;
adaptive filter changes are diagnostic and do not resize its clickable area.
When retained but inactive, its filled area clears and only a short horizontal
identity-color mark remains on the frequency axis. Its frequency/callsign label
uses an 18 px font and magnifies to 32 px on hover.
Up to 24 tracks are retained; nearby peaks inside the initial 45 Hz separation
are treated as one track, numerical peaks more than 96 dB below the strongest
current bin are excluded, and decoded tracks remain visible for the
Display-page **Decoded signal timeout** (default 30 seconds) after their
signal disappears. Silence does not demote an already-verified observation;
after its marker expires, the frequency-to-color assignment remains leased for
at least five minutes and is reused if that carrier returns. Decoder filter
width/evidence rate and these other
starting limits are not yet exposed as profile controls. Click a colored marker
to open only that decoded session in its larger scrollable/selectable wrapped text
window. It follows appended text unless a selection is active, bolds the
confirmed remote call, and highlights an exact own-callsign match while flashing
the card five times. Close with **×** without stopping decode, reopen from the
marker, then drag a card's handle to reorder it. With the handle focused,
Up/Down provides the keyboard equivalent.
If the retained carrier is reacquired with
a new internal ID, the open session follows the same frequency/color identity.
The decoder pane preserves a bounded 2,048-character presentation transcript
across that replacement, but clears the confirmed callsign until the new
acoustic source establishes it independently. Callsign extraction
requires a complete stable word, rejects noise-like separated digit runs while
retaining contiguous multi-digit special-event calls, and
requires decoded `DE`/`CQ`/`TU`/`UP` context or exact repetition before
automatically naming a stream.
At a sustained-silence or explicit end-of-input boundary, the decoder retains
up to sixteen completed transmission turns and separates them with `|` in the
presentation transcript. Context may select only an acoustically competitive
path with the same non-whitespace characters and may repair only a bounded set
of word gaps. Raw and phase-consensus text are unchanged. **CURRENT SENDER**
and its cadence appear only after explicit two-call or calling-station handover
evidence; ambiguous and conflicting evidence leave them blank. Up to eight
identified senders retain independent cadence summaries, used only as a
bounded prior when a later live timing estimate already agrees.
Right-clicking an unmarked spectrum/waterfall position creates a neutral
manual probe at that center and opens its card without moving the independent
TX-slice guide. Measured carrier evidence can move its DSP and presentation centers
through the ordinary bounded stream tracker. It is not included in the detected
count, exposes no decoded content before ordinary verification, reuses only
another manual center within 12 Hz, and expires after the configured decoded
stream timeout if it cannot verify. Successful verification promotes the same
session normally.
Left-click opens an existing detected stream and never creates a manual probe.
Unverified candidates expire after 750 ms and never appear in the signal count.
A callsign label additionally requires stable text and a completed word gap;
partial, provisional, and unsupported one-off candidates remain hidden.

Timing acquisition evaluates nine fixed starting hypotheses at 8, 12, 16, 20,
25, 32, 40, 50, and 60 WPM. The current leader is provisional for at least 2.5
seconds of signal evidence and locks only after enough decoded symbols and score
separation. After a locked track has been silent for 2.5 seconds, its stable
text is retained and timing acquisition restarts for the next transmission.
These acquisition thresholds are internal measured defaults in this build;
profile controls will be added only with benchmark-backed safe ranges.
The parallel acoustic lattice is evaluated at most every 500 ms and at a
completed gap, retains at most four alternatives within 1.0 cost of the best
path, and requires at least 0.40 timing evidence before appending consensus.
The consensus is append-only and is the card's preferred deterministic text
once available; the literal acquisition path remains its fallback.

The current deterministic qualification target is acquisition within six
simulated seconds for clean, 30 WPM, and weak/fading/drifting CW, zero published
tracks for steady carriers, speech-like amplitude modulation, irregular
impulses, and pumping broadband noise, and less than 0.20 processing seconds
per simulated second. The timing corpus also caps its conservative decoder
state estimate at 256 KiB. These are regression limits for the included corpus,
not universal RF accuracy claims; real recording coverage remains backlog work.

## Decoder page

### Keying model

**Keying model** chooses how the decoder decides where the key goes down and
comes back up. Everything after that decision is shared, so the choice affects
only that one stage.

| | when to use it |
|---|---|
| **Adaptive threshold** (default) | Hand and bug sending, and anything where the sender's timing wanders. Decides key-up and key-down from the envelope moment by moment, and shows text soonest. |
| **Semi-Markov (HSMM)** | Machine-sent, heavily weighted, or Farnsworth-spaced sending. Weighs each mark and gap against the lengths Morse expects, at the cost of about one character of delay. |

Neither is better in general, which is why both are offered. Measured at 20 WPM
and 20 dB, the duration model roughly halves character error on heavy weighting
(0.058 against 0.133) and improves Farnsworth spacing (0.075 against 0.100),
while the threshold is three to four times better under 10% timing jitter
(0.075 against 0.283). Across the general accuracy surface and on receiver
recordings the two are level, so pick on the sender rather than expecting one to
be better everywhere.

Changing the model restarts the decoders but keeps every track, so a station can
be compared under both while it is still sending. The setting is stored by name,
so it survives future additions to the list.

### Weak signals

**Decode every tracked signal** decides whether a very weak signal is decoded
or merely watched. It is off by default, and while it is off a signal has to
reach **Decode only above … dB above the noise floor** before the decoder is
given it. The threshold accepts 0.0 to 40.0 dB in tenths of a decibel and
defaults to 4.0 dB; the field is disabled while the toggle is on, because the
threshold no longer applies then.

Nothing disappears from the display either way. A signal under the threshold is
still detected, still followed, and still drawn in the spectrum with its own
marker; only its decoding is withheld. What the setting buys is a cleaner
transcript and spare processing: below the threshold the decoder receives
fragments rather than copy, so it fills the transcript with one- and
two-element characters that mean nothing while each such track costs as much
work as a readable one.

The default is measured rather than chosen. Across the capture corpus the
weakest track that ever carried a correctly recovered callsign sat at 19.5 dB,
so 12.0 dB leaves over seven decibels of margin before the threshold could cost
a station that was genuinely readable. Lower it, or turn the toggle on, when
working a quiet band where marginal signals are the point; raise it on a
crowded band where the transcript is filling with noise.

Both values are stored per profile, and changing either keeps every open track,
transcript, and confirmed callsign: the setting selects which tracked signals
are decoded and never alters the audio the detector receives. They apply to
both decode paths, so a WAV recording opened for replay is gated exactly as
live audio is, which matters because a marginal signal is usually studied in a
recording rather than as it passes.

### Debug capture

On a direct SDR source, capture is written as **SigMF IQ** (`.sigmf-data` plus a
`.sigmf-meta` sidecar) rather than audio, preserving both components of the
complex signal so the recording is usable for later analysis and in other
software. Sound-card sources continue to record audio as before.

Recording stops at whichever comes first: the **Stop automatically after**
duration below, or an internal byte budget. At receiver sample rates the byte
budget normally binds first — roughly a quarter of a gigabyte per minute at one
megasample per second — and the finish message names which limit was reached and
the file written. Gain state and level telemetry are stored with every IQ
recording.


The **Debug capture** control also appears here, not only in the decoder panel
header, together with a button that opens the capture folder in the file
manager and a **Stop automatically after** value between 30 and 1800 seconds
(default 300). Increase it for a signal that only misbehaves occasionally;
reduce it for a quick reproduction, so there is less to review before sharing.
The JSON lines include bounded completed turns plus explicit current-sender and
supported sender-cadence fields. See the operator guide's Debug capture section
for the complete recorded-field description.

### Local character model

Native builds that include the optional local-model backend expose **Settings
→ Decoder**. Enable **Local character refinement**, then select both a local
`.onnx` model and its matching JSON metadata file. The application never
downloads or bundles a character model. Files are validated when **Apply** is
selected; an incompatible model, tensor contract, or metadata file leaves the
normal deterministic decoder running and shows an error instead.

The supported feature contract is 3200 Hz audio, FFT length 256, hop length 48,
and 65 frequency bins covering 400–1200 Hz. Inference is CPU-only and limited
to four active verified, Morse-likely, or manually selected lanes. A strong
character normally needs two overlapping eight-second windows; a moderately
confident character needs three time-aligned windows and remains provisional
with only two. This is a delayed refinement rather than an instant character
display. When processing
falls behind, older pending windows for the same lane are replaced instead of
allowing an unbounded queue to interfere with live reception.
Automatically qualified lanes continue through a bounded two-second silence
grace so slow manual word gaps do not reset their feature history; an
operator-selected lane receives up to ten seconds for initial acquisition.
Bursts that never fill one complete eight-second window produce no local-model
text; the deterministic decoder remains available for those shorter signals.

**Local callsign suggestions** can use either an operator-selected
N1MM-compatible `master.scp` or Call History text export, or a managed copy of
`MASTER.SCP` downloaded directly from the Super Check Partial (SCP) Database.
Managed use and automatic updates are independently optional. Automatic checks
run no more than once per day, honor the provider's cache validators, and
download only a changed file. **Check for updates** performs the same safe check
on demand. The source, release, last-check time, and current state remain
visible in Settings.

Downloaded and selected files must be readable, non-empty, and no larger than
32 MiB; import is capped at one million unique calls and ignores comments,
directives, duplicates, overlong lines, and structurally invalid calls. A
managed download is checked against the provider's advertised SHA-256 identity,
parsed into a fresh bounded database, and atomically installed only after every
check passes. A failed, interrupted, malformed, oversized, or implausible
download never replaces the last valid copy, so decoding can continue offline.
Until the first managed copy is installed, an already-enabled operator-selected
file remains the active fallback.
**Reload local file** rereads an operator-selected file after an external
update.

Super Check Partial is maintained by W9KKN and is an activity-derived
contesting aid, not an official callsign register. CW Buddy downloads it at
runtime from `supercheckpartial.com`; the database is not bundled or
redistributed with CW Buddy. Update requests identify CW Buddy and its
version but never send received audio, decoded text, station identity, or a
callsign query. Absence from SCP never makes a decoded call invalid.

A directory match is eligible only for an already verified CW channel, a
completed uncertain call-shaped transcript span, and agreement by at least two
current competitive acoustic alternatives on the strongest complete call.
The acoustic candidate may differ from that span by no more than two
wildcard-aware substitutions, insertions, or deletions. An ambiguous result
causes abstention. The card and marker prefix an eligible result with `≈`; an
exact list hit shows **DB**, while an absent stronger winner shows **AUDIO**
and cannot be displaced by a weaker database candidate. This is an
advisory hypothesis: it does not alter decoded text, confirm the callsign,
verify a stream, trigger an own-call alert, or control transmission.
An **AUDIO** suggestion uses only current decoder alternatives and therefore
does not require a local list. Enabling a list adds **DB** provenance when that
same acoustic winner is present; it does not replace the acoustic choice.

Local-model output appears in a separate **LOCAL MODEL** section in the decoder
card. A structurally valid call confirmed across overlapping windows may
complete verification only after the carrier has independently reached
Morse-likely through spectral, keying, cadence, and coherence checks; the
ordinary sustained-entry interval remains. It is displayed with a **MODEL**
badge. The model never creates a carrier, replaces raw text, keeps a silent
stream active, or controls transmission. Builds without the optional runtime show
the setting as unavailable while retaining all deterministic decoding.

## Station page

**Own station callsign** is stored separately in every station profile. Input is
trimmed and normalized to uppercase using the same exact callsign policy as the
ignore list and transmit guard. Portable suffixes may use a single `/`.

Example values:

```text
IU0LFQ
AD2FC
IU0LFQ/P
```

The value populates station logging data and is the exact-match source for the
open decoder card's highlighted **YOUR CALL HEARD** visual notification. The
match must appear as a complete token in stable decoded text; partial calls and
provisional elements do not alert. Audio and remote notifications remain future
optional additions. The future optional closing
macro remains subject to explicit configuration, arming, QSO-context checks,
and cancellation before transmission.

### Operating role

**Operating role** tells the decoder whose callsign a monitored stream is
expected to carry. Exchange context alone cannot always say: `TU` precedes a
runner identifying itself and equally the station it has just worked.

| role | meaning |
|---|---|
| **Monitoring** (default) | No assumption. Exchange context alone ranks callsign candidates. |
| **Search and pounce** | You are hunting stations that are calling, so the stream you are listening to is a runner and its own call is the label. |
| **Running** | You are calling and others answer, so the stream is somebody answering you. |

In every role your own callsign is removed from callsign candidates for other
stations' streams, so a transmission that mentions you is still labelled with
the station actually being heard. Your call being heard is a separate thing, and
still raises the **YOUR CALL HEARD** notification described above.

The role only changes which candidate is ranked highest. It never creates,
rewrites, or corrects decoded characters.

## Radio page

### Radio participation and detection

- **No radio — receive-only (SWL):** process audio without CAT, PTT, or KEY.
  The first-run wizard skips the CAT and Keying pages.
- **Detected online radio:** lists only a device that a supported integration
  positively identifies as online. The initial Windows implementation reads
  the two OmniRig slots. It never treats a COM-port name as a radio model and
  does not issue speculative CAT commands.
- **Manual radio template:** loads known safe starting metadata when the
  operator deliberately chooses manual setup. The chosen frequency provider
  still determines which connection fields are shown.

Use **Refresh detection** after starting or reconfiguring the frequency service.
An installed but disabled, busy, unresponsive, or unconfigured radio is not
shown in the detected-radio list. Hamlib model discovery remains planned, but
macOS, Linux, and Windows can connect to an already configured local rigctld
service.

### Reference radio

- **Yaesu FT-450D:** starts at 4800 baud, 8 data bits, no parity, 1 stop bit.
- **Yaesu FT-818/FT-818ND:** starts at 4800 baud, 8 data bits, no parity,
  2 stop bits.

Confirm these values against the radio menu and the interface cable. Loading a
reference profile does not guess a physical port.

### Frequency provider

- **OmniRig (Windows):** select radio slot 1 or 2. The Configure button opens
  its native setup. Direct key/PTT remains a separate COM connection.
- **Hamlib:** connect to `rigctld` started with `--vfo`. Configure the loopback
  host (normally `127.0.0.1`), port (default `4532`), and distinct RX/TX VFO
  names (normally `VFOA`/`VFOB`). Start read-only, or explicitly enable writes
  for frequency, mode, and split. Raw rigctld has no authentication or TLS, so
  CW Buddy refuses non-loopback hosts. For a remote radio, terminate an
  authenticated encrypted tunnel locally and point CW Buddy at its loopback
  endpoint. Hamlib PTT and KEY commands are never used.
- **CAT4OM network service:** connect to a group-specific Control WebSocket and
  select a radio ID. See [CAT4OM setup](cat4om-setup.md).

**RX tuning step** is stored per station profile as a whole-kHz value from 1 to
100 kHz (default 1 kHz). It controls the waterfall-edge `<` / `>` RX buttons;
changing it does not tune the radio until one of those buttons is activated.
Exact readout entry is not rounded to this step.

When authoritative radio state is available, the decoder pane shows a compact
faceplate with grouped whole-hertz RX/TX digits, ON AIR, VFO, SIMPLEX/SPLIT,
and provider-reported RX/TX modes. Each RX/TX frequency, mode, and split action
is independently enabled only when the provider advertises that exact write;
unknown state remains visibly unavailable rather than being inferred.

The TX row represents the independent standby/transmit VFO even in simplex;
the effective transmitted frequency remains the RX frequency until split is
enabled. CAT4OM can report both VFO modes independently. OmniRig exposes only
one mode value, so CW Buddy remembers a mode for A or B only after that VFO has
actually been observed as the receiver. An inactive VFO whose mode has never
been observed remains `?`; CW Buddy never copies the other VFO's mode into it.

### Serial CAT values

CW Buddy does not duplicate connection settings owned elsewhere:

- OmniRig's native setup owns its COM port, baud, data bits, parity, stop bits,
  and polling behavior. CW Buddy selects only OmniRig slot 1 or 2.
- `rigctld` owns its physical radio and serial connection. CW Buddy selects only
  the loopback host/port, RX/TX VFO mapping, and whether writes are allowed.
- CAT4OM owns its physical radio connection. CW Buddy selects only its Control
  URL and radio identity.

The retained direct-serial profile schema is reserved for an implemented
in-process direct-CAT provider. Its UI must offer only 1200, 2400, 4800, 9600,
19200, 38400, 57600, and 115200 baud—not an arbitrary numeric step—and explicit
data-bit, parity, stop-bit, and flow-control selections. Until that provider is
implemented, these nonfunctional fields are deliberately absent from Settings
and the first-run wizard.

### Split and offsets

Enable split when reception and transmission use independent frequencies.
Offsets accept positive or negative whole hertz values.

Examples:

```text
Direct HF radio: RX offset 0, TX offset 0
Up-converter:    RX offset +116000000
Down-converter:  RX offset -116000000
Cross-band SAT:  independent RX and TX offsets, split enabled
```

**CW audio-to-RF mapping** chooses whether an audio tone above the configured
CW pitch lies above (**CW-U / USB**) or below (**CW-L / LSB**) the radio's
actual RX reference. Actual-RF marker labels require live capture, the explicit
Audio-page radio association, and valid live state from Windows OmniRig or
CAT4OM. RX transverter offset is applied first. WAV, SWL, unlinked, and unknown
states remain labeled **AF**.

## Keying page

Select a dedicated serial port and assign different lines to PTT and KEY.
Defaults are RTS for PTT and DTR for KEY, both active high. Change polarity only
to match an electrically verified interface. Port enumeration is passive.

**Run measured loopback** is an electrical safety gate, not an acknowledgement
checkbox. Disconnect the radio physically, connect RTS→CTS and DTR→DSR on the
selected interface, confirm that disconnected state, then run the probe. CW
Buddy opens only that exact port, establishes an inactive baseline, observes
each loop separately, releases KEY before PTT, and closes the port on success
or failure. A successful result is bound to a SHA-256 fingerprint of the exact
port, line assignments, polarities, and platform; changing enablement or any
fingerprinted value requires a new measurement. CAT and keying ports must be
different.

The direct serial adapter and worker-thread Morse scheduler are connected to
the guarded application controller. Opening initializes KEY and then PTT to
inactive. Transmission asserts PTT before KEY; cancellation, error, shutdown,
and emergency release deassert KEY before PTT. The scheduler uses the fixed TX
speed when configured, otherwise the selected stream's bounded RX estimate.

The **QSO** drawer requires explicit arming, an exactly decoded and retyped
target callsign, and a second exact confirmation of the normalized outgoing
message. Own-call, editable report/exchange, profile-configurable quick macros,
and free-text actions all use the same boundary. The progress panel reports
elapsed and remaining time from the worker's monotonic clock while a message or
TUNE is active and clears at every terminal state. **Auto-QSO** can propose one
of those messages from decoded context
but cannot confirm or send it. **TUNE** is an operator-only toggle with a hard,
non-extendable 15-second hardware deadline. Ordinary Morse elements have a
separate three-second continuous-KEY guard. A measured loopback permits safe
port opening but is not on-air acceptance: always complete the documented
message, cancel, watchdog, and emergency-release checks into a dummy load at
minimum power first.

## Display page

- **Spectrum view:** profile-persisted **Audio spectrum** (smoothed) or **CW
  symbols** (verified-channel keying raster on a neutral background).
- **Target FPS:** UI redraw target from 10 to 120.
- **Waterfall lines/second:** independent scroll/update rate from 1 to 120.
- **Automatic range:** adapts the visible dBFS range using smoothed robust
  spectrum levels. Disable it to use the editable lower and upper bounds.
- **Spectrum averaging:** applies exponential power averaging in DSP from 1 to
  32 frames; higher values steady the trace but react more slowly.
- **Reference grid:** shows or hides functional frequency/level guide lines.

## Cluster (DX Cluster / RBN)

Its own tab in Settings, not part of Decoder.

A receive-only feed of what other receivers report hearing. It places a marker
on the separator between the spectrum and the waterfall where a station has
been reported, and it can corroborate a callsign this receiver decoded for
itself. It is off by default.

| Setting | Meaning |
| --- | --- |
| Cluster link | Master switch. Off by default; everything below is inert while it is off, and it cannot be turned on before the station callsign is set. |
| Spot retention | How long a report is kept, 1 to 60 minutes. |
| Match tolerance | How far from a reported frequency a station still counts as the same one, 50 to 1000 Hz. |
| Show labels | Draw callsigns beside the markers. Turning this off keeps the markers, which is useful when a crowded band makes the text unreadable. |

### Connecting to a cluster

The cluster network speaks telnet and nothing else. The public HTTPS feeds are
aggregators, and the Reverse Beacon Network -- the one source here that is
itself a receiver rather than a person -- publishes no web interface at all;
its live stream is telnet or nothing. So a server is chosen from the list
rather than an address typed into the endpoint field.

| Setting | Meaning |
| --- | --- |
| Server | One of the servers in `dictionaries/dx-cluster-servers.txt`, or **Custom…** to give a host and port yourself. The note beside each says what you are joining. |

A cluster is joined with your station callsign, which is what the network is
for and how every cluster client has always worked. The callsign configured for
the station is used; there is no second one to set, and the cluster cannot be
enabled before the station callsign is. Cluster logins are unencrypted, as the
protocol has always been.

**Connection SSID** appends a number to that login. At *none* the bare callsign
is sent; at 1 the node is joined as `CALL-1`, leaving `CALL` and `CALL-2` free
for a logging program or a second client. A node tells one station's several
connections apart by this and nothing else. It is a number rather than a second
callsign field on purpose: the login is always composed from the station
callsign, so the two cannot drift apart, and the line under the switch shows
the login exactly as it will be sent.

Beyond the login and the setup commands listed for that server, the client
sends nothing: no spots, no announcements, and no reply to anything the server
or its users send.

These are volunteer machines shared by thousands of operators, so one
connection is held at a time and a failed one is retried after a wait that
lengthens with each failure rather than in a loop.

### The scale on the separator

When the main axis reads RF, a smaller audio-offset scale appears in the gutter
between the spectrum and the waterfall, marked `AF`. The RF axis says where the
band is; this says how wide the visible span is in audio, which is what the
decoder hears and what the receiver's filter sets.

It appears only where the two scales genuinely differ. On a direct SDR source
the frames already arrive in absolute RF, so there is no separate audio scale
to show and none is drawn; the same applies when no radio frequency is known
and the main axis is showing audio already.

### Filtering

Spots are filtered twice, and the two are not redundant.

On the server, where the cluster software supports it: the filter commands
listed for that server are sent at login, and any of them naming a band is sent
again whenever the radio changes band, so what the server sends follows the
receiver. This is the cheaper filter, because the spots never cross the
network.

Every filter command shipped was watched being accepted by the live server it
is listed against, because a guessed one earns an error reply the operator
never sees and leaves a filter that quietly is not there:

| Software | Command | Servers |
| --- | --- | --- |
| DXSpider | `accept/spots 0 on {BAND}/cw` | GB7DJK |
| AR-Cluster | `set/dx filter band={BANDNUM} and mode=cw` | NC7J, W3LPL |

The two tokens are not interchangeable. `{BAND}` is the ADIF band name, `20m`;
`{BANDNUM}` is the bare number, `20`. AR-Cluster refuses the suffix outright --
`band=20m` is rejected as failing validation while `band=20` is accepted -- and
DXSpider wants the suffixed form, so a single token would leave half the
servers unfiltered while appearing configured.

CC Cluster nodes carry no band command. Their login banner lists the whole
command set and there is no band filter in it, and none was seen to be
accepted. They rely on the arrival filter, as every server does anyway.

On arrival, always: reverse-beacon reports come in at roughly six a second
worldwide across every band, and the reverse beacon network accepts no filter
commands on its telnet port at all, so its feed can only be filtered here.
Anything outside the band being received is discarded before it is stored. A
spot you could not possibly hear is not corroboration, and keeping it would
cost the slot of one you could.

When the receive frequency is unknown -- no radio, no receiver running --
nothing is discarded and no band filter is sent. A filter that silently threw
everything away because it did not know where the radio was pointed would be
worse than no filter at all.

### Editing the server list

`dictionaries/dx-cluster-servers.txt`, one server per line:

```
name | host | port | source | login commands | note
```

`source` is `rbn` for a reverse-beacon feed or `cluster` for human-entered
spots. In the login commands, `{BAND}` is replaced by the ADIF band name of the
frequency being received and `{BANDNUM}` by the bare number; a command
containing either is sent again on every band change, and is not sent at all
while the band is unknown. The two are weighed differently and agreement between them counts for
more than either alone, so do not relabel one as the other. Login commands are
separated by `;` and may be empty; they exist because cluster software differs
in what it sends by default, and several withhold skimmer spots or send digital
modes this application cannot use.

A line that does not parse is skipped rather than taking the rest of the file
with it, so one bad edit cannot leave you with no servers at all.

Spots sit in a bar of their own on the separator, drawn only when there is
something to put in it. A two-pixel stripe under each callsign says where the
report came from: blue for a reverse-beacon receiver's own measurement, tan for
a person's cluster spot, and half of each when the two independent sources
agree. Both hues sit in the chrome family, never one of the identities given to
decoded streams and never the decode window or the transmit slice, so a report
is not mistaken for something this receiver copied.

**A spot is corroboration and never authority.** It can reduce how much of its
own evidence a decoded callsign needs before being offered, and it can show that
an outside source disagrees. It can never replace a decoded callsign with a
spotted one, never rewrite a character, and never let a callsign with no
acoustic support win: reverse-beacon reports carry a measured error rate
approaching two per cent per receiver, and a confidently wrong callsign is worse
than none.

## Network

A live diagnostics stream, so a station can be watched while it runs instead of
being described afterwards. Off by default.

| Setting | Meaning |
| --- | --- |
| Enable | Master switch. Off by default. |
| Addresses | The addresses this machine can bind, one checkbox each, with the interface named and loopback marked. Loopback is reachable only from this computer. |
| Port | Default 17300, chosen clear of the ports amateur software already claims -- Hamlib's `rigctld` on 4532 and `rotctld` on 4533, cluster nodes on 7300, 7373, 8000 and 23, reverse-beacon telnet on 7000 and 7001. Privileged ports below 1024 are refused. |
| Allowed peers | Addresses or subnets that may connect -- `192.168.1.50`, `192.168.1.0/24`, `2001:db8::/32` -- or `any`. Empty permits loopback only. A rule that cannot be parsed permits nothing, so a typo cannot widen access. |
| Access token | Required before any address other than loopback may be bound. **Generate** produces one. |

Connect with anything that reads a socket -- `nc`, `telnet`, a script -- and one
JSON object arrives per line, once a second: the same record the debug capture
writes, including the throughput counters.

**The stream only emits.** This application holds transmit, so a diagnostics
channel that accepted input would be a second and weaker way to reach a radio.
Bytes sent to it are discarded and never parsed, and a peer that keeps sending
is disconnected, because it has mistaken the port for something that answers.

Two controls decide who may read it.

**Allowed peers** is the stronger and the cheaper. Give addresses or subnets --
`192.168.1.50`, `192.168.1.0/24`, `2001:db8::/32` -- or the single entry `any`
for no restriction. A peer's address is known from the socket before a byte is
exchanged, so one that is not permitted is closed without a greeting and never
learns what is behind the port. Left empty it permits loopback only, which is
the safe reading of "not yet decided". A rule that cannot be parsed permits
nothing, so a typo can never widen access.

**The access token** is then read from the client as a single bounded line
before anything is sent to it, and compared in constant time. A client that
presents the wrong token, or none, receives no record at all.

What neither gives you: the token is one shared secret, so it cannot tell one
reader from another or shut out a single one -- changing it cuts off everybody
at once. And the stream is **not encrypted**, so the token and everything it
protects cross the network in clear.

So bind it to a network you trust. If you need it from elsewhere, tunnel it --
over SSH, or the remote-desktop session you are already using -- rather than
forwarding a port to the computer that controls your radio. Authentication
worth the name belongs with the planned remote-operation work, where TLS and
per-client certificates and revocation of one reader are designed for rather
than added afterwards.

At most four readers connect at once, and one that cannot keep up is
disconnected rather than buffered without limit: losing an observer is a
smaller failure than growing this application until the station stops.

**Waterfall rendering**, on the Display tab, exists for the same purpose. A
station left running as a diagnostics server does not need to draw a waterfall,
and switching it off detaches the display from the frame source rather than
merely hiding it, so no row is conditioned, appended or retained and the
history already held is released.

## CW vocabulary files

Read at startup from `dictionaries/` in the application data directory, and
seeded there from copies carried inside the application the first time it runs.

| File | Contents |
| --- | --- |
| `cw-abbreviations.txt` | Abbreviations, Q-codes, prosigns and signal reports the decoder recognises. One token per line; letters, digits and `/` only; case is not significant. |
| `cw-word-gap-prefixes.txt` | The subset that may precede a callsign with no gap. Each entry must also appear in the abbreviations file, or it is refused at load. |
| `cw-distinctive-tokens.txt` | The small subset distinctive enough that one match is accepted as evidence of real CW. Deliberately much narrower than the abbreviation list, and each entry must also appear there. |
| `morse-alphabet.txt` | The element pattern the decoder recognises and the symbol it produces. One entry per line: dots and dashes, whitespace, then the symbol. A symbol may be several characters, which is how prosigns such as `<SK>` are represented. |
| `callsign-prefixes.txt` | The callsign prefix blocks the ITU has allocated, one per line as `START END Country`. A decoded token whose opening characters fall in no block cannot name a country, so it is refused as a stream label. The country names are there to be read by whoever edits the file and decide nothing. A gap is costly and a stale name is not: a prefix the file does not cover is refused, so when in doubt include the block. |

### Contest exchange profiles

`dictionaries/contests/` holds one file per contest, and `README.md` beside them
describes the format in full. A file gives the contest's identity, what the
other station sends, what your station sends, and the order each side sends
them. Everything else -- the conversation flow, its states, and every transmit
safety gate -- is built by the application and cannot be named in a file. No
file in that directory can arm a transmitter, change a key-down timeout, or
relax callsign confirmation.

Cut numbers are declared per field with `cut=`. The shipped contest files
carry the set an operator actually sends at speed -- `T` and `O` for zero, `A`,
`U`, `V`, `E` and `N` for one, two, three, five and nine -- so a serial sent as
`ANU` reads as `123`.

A file that does not parse, or that parses into an exchange the application
rejects, is refused whole and reported; it never loads partially. One bad
contest does not remove the others.

The alphabet is receive-only. What may be transmitted is fixed in the
application and is deliberately narrower: `<SOS>` is decoded so that a distress
call can be read, and is transmittable, because sending one is legal and
appropriate in a genuine emergency. Editing `morse-alphabet.txt` cannot change that.

Blank lines and lines beginning with `#` are ignored. A line carrying any other
character is rejected whole rather than trimmed, so a malformed entry never
scores matches its author did not write. Membership is consulted only to choose
between readings with identical characters, so these files cannot change a
decoded letter. An absent or empty file is valid and contributes no spacing
evidence.
- **Show brief spectrum gesture hints:** enables the compact pointer legend for
  at most ten seconds, with a five-minute cooldown before it can appear again.
  Disabling it leaves ordinary button tooltips available.
- **Lower/upper dB:** manual bounds; at least 10 dB of span is enforced.
- **Decoded signal timeout:** from 5 to 300 seconds, default 30. Controls how
  long a verified track's marker and session stay visible after its signal
  disappears before being removed. Applies immediately to a running decoder
  session on both the live-audio and WAV-replay paths, without restarting RX.
  A carrier recognized again within at least five minutes reuses its previous
  color even if this shorter marker/session timeout has already elapsed.

The current receiver canvas is an honest empty state and does not draw simulated
radio data.

## Profile examples

The planned connection-profile wizard is generic; the following are
illustrative saved topologies, not built-in presets or required equipment:

These examples are acceptance scenarios for the planned generic wizard. The
wizard must discover and validate equivalent endpoints instead of matching the
example names or assuming a particular station layout.

### HF SDR receive with separate transmitter

```text
Example profile name: HF — RSPduo RX / FT-450D TX
RX: SDRplay RSPduo, tuner 2, high-impedance input
TX: Yaesu FT-450D through the selected CAT and guarded keying providers
Duplex: full duplex where the hardware routes permit it
```

### QO-100 full duplex

```text
Example profile name: QO-100 — RTL-SDR downlink / FT-818 uplink
RX: RTL-SDR with a profile-defined receive/transverter offset
TX: Yaesu FT-818 with an independent transmit/uplink offset
Duplex: full duplex; RX and TX frequency domains are validated separately
```

### One-radio HF audio

```text
Example profile name: HF — FT-450D audio half duplex
RX/TX: Yaesu FT-450D
Spectrum input: the FT-450D receiver audio interface/sound card
Duplex: half duplex
```

### HF desk

```text
Radio: Yaesu FT-450D
Frequency provider: OmniRig slot 1 (Windows) or Hamlib
CAT: radio-specific COM port, 4800 8-N-1, hardware RTS flow control
Key/PTT: dedicated interface port, RTS PTT, DTR KEY
Offsets: 0 / 0
```

### Portable radio

```text
Radio: Yaesu FT-818
Frequency provider: OmniRig slot 2 (Windows) or Hamlib
CAT: radio-specific port, 4800 8-N-2
Key/PTT: dedicated interface port
Offsets: 0 / 0
```

### Network-controlled station

```text
Frequency provider: CAT4OM network service
Control URL: ws://127.0.0.1:5001/
Radio ID: run
CAT serial fields: managed by the CAT4OM server, not this client
Key/PTT: remains local and independently guarded unless a future remote-server
         profile explicitly owns transmission
```
