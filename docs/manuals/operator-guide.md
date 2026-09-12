# Operator guide

## Current development status

CW Buddy is pre-release software. The current desktop shell can create and
select isolated station profiles, guide first-time setup, run in receive-only
SWL mode, discover serial ports without opening them, identify online radios
through a supported integration, save radio/keying/display settings, open the Windows
frequency-provider configuration, monitor a CAT4OM radio, process a selected
live sound-card input, and replay a WAV recording through the real spectrum and
waterfall, and run a receive-only decoder across the complete processed
passband. Each tracked frequency now obtains keying evidence from a narrowband
filter over the original audio rather than the display spectrum. Bounded
multi-speed acquisition is active, and the technique that decides keying is
selectable between two: an adaptive threshold, which suits hand and bug
sending, and a duration model that suits machine-sent, weighted and Farnsworth
keying. Direct reception from a software-defined receiver and guarded direct
keying are both implemented and documented below; keying requires a measured
electrical loopback before it will arm, and on-air use additionally requires the
operator-performed dummy-load procedure. Multiple-pass weak-signal recovery,
the logging connection, and the remote-station runtime remain under
implementation.
A saved profile does not arm or key a transmitter.

The replay core accepts little-endian RIFF/WAVE PCM at 8, 16, 24, or 32 bits and
IEEE float32. Multichannel input is averaged to mono for this audio-analysis
path. Compressed WAV codecs are rejected with a clear diagnostic.

Every supported-platform build runs an empty-receiver render regression test
and loads the complete QML desktop shell before it can be published. This
specifically covers first launch before a WAV, audio device, or SDR source has
produced spectrum data. The staged application is also launched with the hosted
runner's native graphics path before its installer or archive is uploaded, with
a deterministic test spectrum to exercise waterfall texture creation.

## Where things are on screen

```mermaid
flowchart TB
  subgraph WIN["Application window"]
    HEAD["Title bar — profile and radio names,<br/>TX state badge, Profiles, Settings"]
    RAIL["Left rail — RX, CALLS, QSO, LOG, REMOTE<br/>only RX and QSO open anything today"]
    TOOL["Receiver toolbar — source, Listen OFF/RX and level,<br/>start and stop"]
    subgraph MAIN["Workspace"]
      direction LR
      SPEC["Spectrum and waterfall<br/>channel markers, callsign labels,<br/>CALLING YOU alert, selection cursor"]
      CARDS["Decoder cards — one per open stream<br/>transcript, callsign and LISTED badge,<br/>speed, signal-to-noise ratio, confidence"]
    end
    STATUS["Status line — source and sample rate,<br/>input overruns, frames and lines per second"]
  end
  HEAD --> RAIL --> TOOL --> MAIN --> STATUS
  SPEC -->|"click a marker"| CARDS
```

A title bar runs across the top carrying the application version, the open
station profile and the linked radio's name, a **TX DISARMED** / **TX ARMED** /
**TX ON AIR** badge, and the **Profiles** and **Settings** buttons. A narrow
rail down the left lists **RX**, **CALLS**, **QSO**, **LOG** and **REMOTE**;
**RX** is the receiver workspace you are already looking at and **QSO** opens
the guarded transmit controls, while the other three name workspaces that do
not exist yet and say so when hovered. The rest of the window is a receiver
toolbar above a workspace above a status line. The workspace
holds the spectrum and waterfall on one side and the open decoder cards on the
other; the application opens maximized so both remain usable. Every tracked
signal appears as a marker on the spectrum carrying its
callsign once one is decoded; clicking a marker opens that stream as a decoder
card, which is where the transcript, the callsign and its corroboration badge,
the speed, the signal-to-noise ratio and the confidence are shown. The status
line reports the source and its sample rate, the running input-overrun count,
and the configured frame and waterfall line rates. The linked radio's frequency
is on the Radio Control faceplate rather than the status line, and debug-capture
progress appears under the CW Decoder heading below it.

Two operators in an ordinary simplex QSO normally alternate on the same
carrier. When the stable text contains a complete `CALL1 DE CALL2` handover,
the one decoder card is labelled **QSO CALL1 ↔ CALL2** and retains both
participants. Sustained silence separates completed transmissions in the card
with `|`. **CURRENT SENDER** appears only when an explicit two-call handover or
`CQ ... DE CALL` identifies the sender; otherwise CW Buddy abstains. It does
not create a second frequency marker or infer a sender from frequency alone.

## Receive live radio audio

1. Open **Settings → Audio**, select the sound-card input connected to the
   receiver, and select **Apply**.
2. In the Receiver workspace, choose **Live audio** instead of **WAV replay**.
3. Select **Start live RX** from either the centre of the empty receiver pane or
   the receiver toolbar. The signal-selection cursor activates only after a
   source starts. On macOS, approve microphone/audio-input access the first
   time; the application requests this only when live RX is started.
4. Confirm the status line names the device and sample rate. Spectrum and
   waterfall frames now come from that device.
5. Select **Stop live RX** before changing cables or audio routing.

Hover over the spectrum or waterfall to see its brief pointer legend. Left-click
opens the decoder card for an already detected stream without changing audio
monitoring; Alt+left-click opens a manual decode where you point, for a signal
detection has not found; right-click points the decode region at that
frequency. When the linked provider supports TX-frequency and split writes,
Ctrl+left-click requests the pointed RF on VFO B/TX and enables split if
needed. The TX guide moves only after provider readback confirms the change;
the gesture never arms or starts transmission. Each pointer gesture is
exclusive, so combining modifiers cannot also open or probe a stream. The
legend hides after ten seconds and is offered no more than once every five
minutes. Disable **Show brief spectrum gesture hints** under Settings → Display
to suppress that overlay while retaining ordinary button tooltips. Action buttons
throughout the receiver, settings, setup, and profile views explain their
effect and any disabled state when hovered.

To listen through CW Buddy, choose a **Monitor output** under **Settings →
Audio**, then use the receiver toolbar's **OFF / RX** control. **RX** passes
the complete receiver window without filtering. Per-stream monitoring is
controlled only by the **Monitor** button with its speaker icon in each decoder
card.
Each enabled card passes through its own carrier-following narrow filter and is
moved to the configured CW reference tone. Enable several card speakers to mix
several isolated streams; disable the last speaker to return monitoring to
**OFF**. The speaker and the guarded **TX** action stay in the card's lower
action row while text and activity state update; they do not become conditional
on the carrier currently being keyed. A card without a confirmed callsign does
not invent a placeholder callsign or expose a callsign TX action. Opening or
closing an ordinary decoder card does not start listening.
The adjacent slider
sets local playback level. Monitoring does not change decoder evidence. Output
is intentionally bounded; if the sound device cannot keep up, old audio is
dropped instead of accumulating delay.

Capture requests 48 kHz mono floating-point audio when supported. Otherwise it
uses the input's preferred PCM format, safely averages channels to mono, and
normalizes integer or float samples. Capture places fixed sample blocks in a
bounded queue without allocating or blocking; FFT work runs on a separate DSP
worker. **Input overruns** should remain zero. A rising value indicates the DSP
cannot keep pace and blocks are being deliberately dropped rather than allowing
unbounded latency.

The spectrum and waterfall visualize all audio energy; they do not wait for a
Morse signal. Receiver noise alone should begin filling the waterfall after
live RX starts. If the display remains blank while **Input overruns** rises,
stop live RX and install a newer build because the processing worker is not
draining captured blocks correctly.

## Receive directly from an SDR

An SDR-enabled build can receive complex IQ directly from RTL-SDR, SDRplay,
and other receive-capable SoapySDR modules. It does not require another SDR
application to be running, and another application must not own the same USB
device.

1. For RTL-SDR, install the current official CW Buddy package. Windows, macOS,
   and the portable Linux archive carry their SDR runtime; the Linux `.deb`
   instead makes APT install the distribution's RTL-SDR Soapy module and its
   dependencies. On Windows, CW Buddy provides the SoapySDRPlay3 bridge but
   requires the separately installed [SDRplay Hardware API 3.15](https://www.sdrplay.com/hardware-api/)
   and service; the
   API that the vendor's own receiver application installs satisfies that
   prerequisite. Close any other application that already owns the RSP before
   discovery, because only one application can own one at a time.
   macOS and Linux additionally require a compatible SoapySDRPlay3 module. A
   custom source build must be configured with `-DCWA_ENABLE_SOAPY_SDR=ON`.
   On Windows, **Installed modules** should include `sdrPlaySupport.dll` after
   opening Settings → SDR. If it is listed but fails to load, reinstall API
   3.15; if it loads but the RSP is absent, close whichever other application
   is holding the receiver and refresh.
2. Open **Settings → SDR**. The page performs one discovery scan after it
   renders; choose the physical receiver and, when offered, its operating mode.
   An RSPduo is one receiver even though its driver exposes Single Tuner, Dual
   Tuner, and Master configurations; start with **Single Tuner**. Application startup deliberately
   does not probe SDR hardware. Select **Refresh devices** after reconnecting or
   hot-plugging a receiver.
   If the backend, module, vendor runtime, USB permission, or device is missing,
   the page keeps sound-card reception available and explains what was not
   found.
3. Enter initial SDR and decoder center frequencies in VFO-style kHz: for example,
   `7021.43` means 7.02143 MHz. Select a supported effective IQ sample rate,
   hardware RF bandwidth, and
   antenna/input where the device exposes them. Start conservatively at 250000
   samples/s. **Dual Tuner** and **Master** are advanced coordinated modes and
   do not create extra CW Buddy panes. The antenna/input list reflects the
   ports available in the selected mode. SDRplay's lower effective rates use
   driver-managed decimation, selected through the effective IQ sample rate;
   there is no second independent decimation setting to guess.

   The RSPduo mode abbreviations come from the SoapySDRPlay3 driver:

   | Mode | Meaning in CW Buddy |
   |---|---|
   | **ST** | **Single Tuner**. One tuner is opened as an ordinary independent receiver. This is the recommended mode for the current CW Buddy receive path; the input selector chooses among the ports the driver exposes. |
   | **DT** | **Dual Tuner**. Both tuners run together with a shared clock/rate and the driver exposes two simultaneous RX channels. CW Buddy currently opens only the first channel, so DT does not yet create a second spectrum or decoder bank. |
   | **MA** | **Master, 6 MHz**. The RSPduo owns the master side of a coordinated master/slave setup using the driver's 6 MHz device sample clock. |
   | **MA8** | **Master, 8 MHz**. The same master role using the driver's 8 MHz device sample clock; this is an advanced alternative, not another receiver. |

   A driver may also advertise **SL** (**Slave**) when a matching master
   context owns the shared hardware configuration. CW Buddy presently has one
   IQ-source contract, one spectrum, and one decoder-window pipeline per active
   receiver. It therefore requests SoapySDR RX channel 0; simultaneous tuner A
   and tuner B display/decoding requires a future second synchronized source
   pipeline rather than treating the modes as duplicate hardware.

   Enable hardware AGC only when that receiver provides it, otherwise select a
   manual gain.
4. Set the **Decoder window center (kHz)** and choose one of the offered
   **Decoder bandwidth** values: 6, 12, 24, 48, or 96 kHz.
   CW Buddy will continue to draw the complete acquired passband, but only this
   bounded, down-converted and decimated window reaches CW detection and the
   stream decoder bank. The 24 kHz default is a practical starting point.
   Configure a signed **SDR LO offset** only when the SDR centre must differ
   from the radio frequency; it is bounded to keep the decode window in view.
5. In the Receiver workspace select **Live SDR**, then **Start SDR RX**. The
   spectrum and waterfall use absolute RF coordinates across the captured IQ
   passband. A translucent region shows which part is currently eligible for
   CW detection and decoding.
   Use **SDR Radio Control** above the independent CAT **Radio Control** for
   normal operation. Click its RX digits to enter a frequency, use `<` / `>`
   there or at the waterfall edges for the selected step, and choose a supported
   IQ rate, hardware RF bandwidth, and antenna/input. A frequency-only change
   retunes the running receiver without stopping the spectrum; controls that
   change the device route or stream format may briefly restart reception.
   Enable **Radio Sync** to link the SDR RX and the configured CAT RX VFO in
   both directions. Turning the physical VFO updates the SDR; editing or
   stepping SDR RX issues one provider-neutral CAT RX request. TX frequency,
   split, mode, PTT, and KEY are never changed by this link.
6. Place the pointer over the SDR spectrum or waterfall and use the wheel to
   zoom around it. Middle-button drag pans the visible view. Select **Full
   span** to return to the complete acquired passband. Right-click
   points the decode region at that RF. Ctrl+right-drag draws and applies the
   region directly, and a Ctrl+right-click without dragging moves its centre
   and keeps the width; the width is rounded to 100 Hz and bounded to
   2–96 kHz. Alt+left-click starts a manual decode, which promotes through the
   same verification path as an automatically found stream. Clicking the wheel
   button retunes the receiver so that frequency becomes the centre of the
   acquired spectrum.
7. Global **RX** monitoring is intentionally unavailable for IQ: raw I/Q samples
   are not loudspeaker audio. Open a decoder card and enable its speaker to hear
   that carrier through the narrow, carrier-following CW filter. Several card
   speakers may still be mixed. Selected-stream audio is re-pitched to the
   configured CW reference tone and level-normalized after filtering; the
   toolbar level remains the final listening-volume control.

Radio Control remains available while Live SDR is selected. The SDR can own the
RX path while a separately configured CAT radio supplies authoritative VFO
state and the guarded TX/keying endpoint; selecting an SDR never grants that
receiver transmit authority.

This direct-SDR path is receive-only. It exposes no SDR transmit, PTT, or KEY
command. Debug capture records an SDR source as interoperable IQ rather than as
audio, which is described under Recording receiver IQ for later analysis below.
If a current official package reports the
SoapySDR backend unavailable, the installation is incomplete or damaged. If
the backend is ready but no receiver appears, check the USB connection,
platform device access, and the device-specific module/vendor driver. Linux
users may need to reconnect the receiver after installing device-access rules;
the portable archive cannot supply kernel drivers or grant USB permissions.
If the display becomes sluggish, reduce the effective IQ sample rate and/or the
decoder bandwidth. Zoom changes only the visible viewport; it does not discard
the wide acquisition or silently enlarge the decoder workload.

If a stationary peak fills the far-left edge, open **Settings → Audio** and
leave **Remove input DC offset** enabled. This is normally sound-card DC bias,
not gain. Keep **Automatic gain** off for a calibrated receiver and tune
**Manual gain** from 0 dB; or enable it and choose the automatic dBFS target.
Use automatic 100–3000 Hz bandwidth for a general CW view, or disable it and
enter lower/upper frequencies around the receiver passband. Select **Apply** to
save the values in the active station profile.

The same operational controls now sit immediately below the spectrum. Changes
to DC rejection, software gain, bandwidth, display levels, automatic span, and
waterfall noise suppression take effect while RX is running. **Save profile**
persists the current values; the Settings pages remain available for complete
profile configuration. The panel collapses to a slim header when the pointer
leaves it; hover over the header to reveal it, or select **Pin** while making
several adjustments.

The waterfall always represents the number of seconds set by
**History (seconds)**, from top to bottom, anywhere between five and thirty. At
startup, unavailable older time stays dark rather
than stretching the first received rows over the pane. Resizing changes only
the pixel height, and capture gaps remain visible as dark time. Display
controls do not change what is decoded: **Averaging** is a presentation setting
only, and any **Lines / second** value of 60 or more supplies the decoder with
identical evidence. Below 60 lines/s the detector receives fewer spectrum
observations and may acquire a signal more slowly, so prefer 60 or more while
decoding.
Adjusting these controls no longer restarts decoding either — only changing the
processing bandwidth, DC rejection, or gain resets tracks and transcripts.
**Lines / second**
controls genuine overlapping analysis updates without changing the window
duration; use 60–120 lines/s when inspecting high-speed dit/dah traces, subject
to available CPU. Reduce **Averaging** to 1–2 frames for crisper element edges;
raise it only when a steadier but less time-sharp display is more useful.

For timing inspection, select **CW symbols**. Unlike **Audio spectrum**, this
is intentionally sparse: only active, verified channels' carrier-on states are
drawn as high-contrast marks on a neutral background. Carrier-off rows form
blank gaps, and full-passband/unverified noise remains exclusively in Audio
spectrum. A retained marker cannot draw marks unless its peak is currently
matched, so residual noise during the identity hold stays blank. This makes
dit/dah timing readable rather than presenting a second noise spectrogram.

The Display controls are grouped into multiple responsive rows. Drag the
labeled sliders for FPS, line rate, averaging, history, levels, CW center/width,
and Audio-spectrum noise margin; each label shows the current numeric value.
The full Settings → Display page offers the same slider interaction.

Enable **TX slice guide** to draw two dashed red boundaries around the
authoritative VFO B/TX frequency. The configured width defaults to 200 Hz. In
direct SDR the guide uses absolute RF; in a sound-card view it is mapped around
the configured CW reference tone according to RX frequency and sideband. It
therefore follows VFO, split, and frequency changes made on the physical radio.
Unknown, replay, or off-screen TX state hides the guide instead of retaining a
stale position. The region has no fill, so it cannot be mistaken for an
identified signal. It is visual only: it does not select a decoder, arm TX,
key the radio, or change decoder bandwidth.

Right-click an unmarked spectrum or waterfall trace to create and immediately
open a neutral manual decoder region at that audio frequency. This does not
change the independent TX-VFO guide. As measured carrier evidence shifts, the
region follows it through the same bounded, smoothed tracker used by automatic
streams rather than remaining frozen at the clicked coordinate.
The slice uses real narrowband evidence but does not claim that the signal is
CW: its text and callsign remain hidden and it is excluded from the detected
count until the normal cadence, timing, symbol, and coherence gates pass. A
verified region becomes the ordinary colored stream with the same session; an
unverified region expires after the configured **Decoded signal timeout**.
Click again to refresh it. Manual
centers within 12 Hz reuse the slice, while more distant centers can remain
separate for close pileup inspection. Radio retuning remains a separate,
capability-checked operation rather than a consequence of this click.
Left-click remains reserved for opening an already detected colored stream.

For a quieter waterfall, open the **Display** live-control tab, leave **Suppress
audio noise** enabled, and start with a 6 dB **Audio margin**. Automatic levels
maintain a
minimum 60 dB span and follow falling peaks slowly, preventing receiver-noise
changes from repeatedly driving the palette yellow. A radio's own AGC may still
change the audio level delivered by the sound card; this application does not
yet control radio AGC through CAT.

The trace and the waterfall deliberately use different bottoms, taken from the
same continuously estimated noise floor. The trace's baseline, which is what
the level axis is labelled with, sits twelve decibels below that estimate: put
it on the noise instead and the floor lies flat along the bottom of the plot,
where neither its shape nor a signal's distance above it can be read. The
waterfall palette starts higher, nearer the noise, so that none of its colour
range is spent colouring noise. Both are derived from the same smoothed bound
and move together while **Auto levels** is on; with manual levels the bounds
you set are used directly. The noise-floor estimate itself rises quickly and
falls slowly, so a passing burst does not drag the whole display with it.

The receiver scans every frequency inside the processed audio bandwidth for
both live audio and WAV replay. Spectral peaks begin as private candidates and
do not immediately receive a line. A candidate must show local prominence,
repeat across spectrum observations within the retained track, contain coherent
narrowband energy and keyed edges, and progress through a Morse-likely state. It
then must
produce at least three known Morse symbols with bounded unknown output plus
adequate spacing cadence, timing, and character confidence. Only then does
it receive a stable color and a colored vertical area with a fixed 120 Hz
presentation width, or increment **signals detected**. The internal adaptive
filter remains independent and is reported in the tooltip/session diagnostics,
so its normal 60/120/240 Hz changes cannot resize or flicker the marker. A
thinner line inside that area flashes
with the live keying state. Click anywhere in that colored area to open its
decoded session in the right-hand panel. Its larger decoded-text window wraps
the latest output and follows new text while its viewport is at the bottom.
When a stream contains **Settings -> Station -> Own station callsign**, its
marker on
the spectrum blinks and reads **CALLING YOU** in red. That happens on the
spectrum rather than only inside an opened decoder card, because the point is
to find the stream in the first place; click the marker to open it and follow
the decode. The operator's own callsign is never used as a stream label either:
hearing it means somebody is calling, and the station worth naming is the one
doing the calling, so the stream stays unlabelled until that station
identifies.

A confirmed callsign shows whether the offline list corroborates it. In the
decoder card a green **LISTED** badge with a tick follows a callsign found in
the list, and a plain **DECODED** badge follows one that was not; on the
spectrum, a corroborated callsign is drawn as a solid chip and carries a tick.
Neither badge appears when no list is loaded. **DECODED** is not a warning: a
station that is simply absent from the list is ordinary, and the badge reports
corroboration rather than correctness.

When the offline callsign list is loaded, a decoded callsign that is within
two characters of a single entry in it can be suggested in place of what was
decoded. That substitution is off until enabled in **Settings -> Decoder ->
Correct near misses**, directly below the callsign-list controls. It is
unavailable until either the managed cache or an operator-supplied list has
loaded successfully. Two listed stations can differ by one character, so a
correction can name a station that was never sent. Enabled or not, the
suggestion stays advisory: it never rewrites the transcript, never becomes the
confirmed callsign on its own, and never affects verification.

Pending updates are reported once per launch. If a newer application build or
a newer callsign list is available, a notice lists them with a button for each;
nothing is downloaded until it is pressed, and the notice waits until any
first-run profile or setup step is finished. For an application update, the
notice then shows download and checksum status and replaces the download
button with **Open Installer** and the platform reveal action after successful
verification.

Scroll upward or select text to inspect earlier output without live updates
moving the cursor or viewport; scroll back to the bottom to resume following.

### Reading the SDR faceplate

**SYNC** is the square tile on the SDR Radio Control. It turns green when
frequency synchronisation with a configured radio is engaged, and stays dark
when it is off or unavailable — a dimmer green means engaged but not yet
confirmed by the radio. Sync only ever exchanges receive frequency; it never
touches transmit, split, mode, PTT or keying.

Zoom and the decode window are **independent**. Zooming and panning change only
what you are looking at; the decode window is set in **Settings → Decoder** and
by shift-dragging across the plot. Because of that you can be looking somewhere
the decoder is not, so when the decode window is off screen a chevron appears at
the edge of the plot pointing towards it and naming its centre frequency. Click
it to bring the window back into view without changing your zoom.

Your zoom persists until you change it. Incoming spectrum frames redraw inside
the span you chose and never widen it back out on their own. Retuning keeps it
too: moving the tuned frequency re-centres the display at that same span
rather than returning to full span.

Retuning keeps the waterfall history as well. A row is a history of frequency,
so when the receiver moves that history is still true and simply sits at
different bins; the rows are therefore slid sideways by the same number of bins
the band moved, and what was already drawn stays under the frequency it belongs
to. Bins that come into view carry no history and are filled with the row's own
quietest value, so newly exposed spectrum reads as empty rather than as a copy
of the old edge. Only a pure translation can be slid: if the span itself
changes, the bins no longer mean the same width and the old rows are dropped.
The per-bin conditioning baseline cannot be slid meaningfully across a retune,
so it alone is re-established, and it converges again in well under a second.
The noise-floor estimate that sets the display range is not disturbed by a
slide, so the levels you are reading stay where they were.

A debug capture also records what the decoder was working from: the analyzer
settings in force, how many spectrum frames reached signal detection, how many
times the decoder was restarted, and whether the Morse alphabet came from your
file or the application's own copy. If you report that nothing is decoding,
that capture usually answers why without further questions.

Each snapshot also records whether the application is keeping up, which is a
different question from what it found. `modelPublishesPerSecond` counts how
often the decoded-channel model is handed to the part of the program that
draws; `drainsCappedPerSecond` is non-zero only when samples were still waiting
after a drain gave up, which is the honest sign that they are arriving faster
than they are consumed; and the drain durations say how long the signal work
itself is taking. These exist because an application that had stopped
responding while several signals decoded left no number anywhere to look at,
and the cause was invisible in diagnostics that described only the decoding.

### Watching a session as it runs

The snapshots are appended to `diagnostics.jsonl` about once a second while a
capture records, so the file can be followed live rather than read afterwards.
On Windows:

```powershell
Get-Content "$env:APPDATA\CW Buddy\diagnostics\cwa-debug-capture-*\diagnostics.jsonl" -Wait -Tail 1
```

For anything outside a capture, start the application with `--log-file <path>`,
or set `CWA_LOG_FILE`, and its diagnostic output is appended there for the life
of the session. Every line is flushed as it is written, because a log that
loses its last buffer is silent about the one moment worth reading. It is
off unless asked for: a log nobody requested is a file that grows on your
machine forever.

One caution when reading a stall in the operating system's task manager: a
total that looks modest can still mean one thread is saturated while the rest
idle, which is what an unresponsive window usually is. A per-thread view --
Process Explorer on Windows shows one -- distinguishes the two immediately, and
a task manager cannot.

### Recording receiver IQ for later analysis

The **Debug capture** button records a direct SDR source as interoperable IQ.
The result is
a SigMF pair — `iq.sigmf-data`, holding interleaved complex samples, and an
`iq.sigmf-meta` sidecar describing sample rate, centre frequency, sample format
and start time — so the recording can be replayed in other software rather than
only in this application. Samples are written as `ci16_le`, which is lossless
for the receivers this application records (an RSPduo digitises at fourteen bits
and an RTL-SDR at eight) and half the size of 32-bit float; at megasample rates
that halving is what decides whether a capture is usable at all. A retune
mid-recording starts a new capture segment rather than silently mislabelling
the samples that follow it.

Two limits apply, and at receiver sample rates the size limit usually reaches
first: recording stops at whichever of the four-gibibyte payload budget or the
**Stop automatically after** duration is reached, and the reason is reported
when it finishes. A megasample per second produces roughly a quarter of a
gigabyte per minute, so plan captures in tens of seconds rather than minutes
unless you have reduced the sample rate.

Each recording also stores the receiver's gain state and its own level
measurements — peak magnitude, how often samples approached full scale, and the
residual direct-current offset. If you are investigating whether the receiver is
set up well, keep those: a recording without them shows the symptom but cannot
show whether the front end was being over- or under-driven.

Review a capture before sharing it. It contains whatever the receiver was
hearing across the whole acquired passband.

## Guarded TX preparation

Open **QSO** to use guarded direct serial CW transmission. Before **Arm TX** can
be used, all of the following must be true:

1. **Settings → Keying** names a dedicated serial port, assigns separate PTT
   and KEY lines, and uses an electrically verified active-high interface.
2. Physically disconnect the radio, fit RTS→CTS and DTR→DSR loopbacks, select
   the disconnected confirmation, and press **Run measured loopback**. CW Buddy
   must observe the inactive baseline, each independent transition, and the
   final release. Changing any keying detail invalidates the stored result.
3. The radio-control provider confirms the exact TX frequency, split state,
   and a CW or CW-R TX mode. A target that the provider cannot read back is not
   sufficient to arm.
4. Your own callsign is configured under **Settings → Station**.

Arming opens the named keying port in its inactive state and snapshots the
confirmed radio state. A later TX-frequency, TX-mode, split, port, line, or
polarity change disarms before keying; a change observed while KEY/PTT is active
causes an emergency release and latched fault.

Choose **TX** on a decoder card whose callsign was decoded exactly. Retype that
station and press **Confirm station** before preparing **Send my call**, the
editable report/exchange, a profile-configured quick macro, or
operator-authored free text. CW Buddy
normalizes the message to uppercase
Morse-compatible text and shows its duration at the selected 5–80 WPM; retype
that exact preview and press **Confirm preview** as a separate confirmation.
**TRANSMIT PREPARED MESSAGE**
then schedules the immutable Morse plan on the direct adapter. **CANCEL TX**
and **EMERGENCY RELEASE** synchronously release KEY before PTT;
emergency release also latches a fault and requires an explicit reset, which is
offered as **Reset fault (stays disarmed)**.
While active, elapsed/remaining time and progress come from the worker's
monotonic schedule rather than an optimistic UI timer and clear on completion,
cancellation, or fault.

### Sending prosigns

Write a prosign in angle brackets and it is keyed as one symbol, with no gap
between its letters — which is the whole difference between `<AR>` and `A R`.
Seven can be sent: `<AR>` end of message, `<AS>` wait, `<BK>` break in, `<CT>`
start of message, `<KN>` go ahead addressed station only, `<SK>` end of
contact, and `<SOS>`.

Anything else in brackets is refused when the message is normalized, so it
never reaches the preview. That includes a misspelled prosign and an
unterminated one: the message is rejected rather than keyed as loose letters,
because writing `<XX>` means you intended a prosign and sending something else
in its place would be worse than sending nothing.

A distress call is included deliberately. It is legal and appropriate to send
one in a genuine emergency, and it passes exactly the same gates as any other
message — armed station, exact callsign confirmation, retyped preview, and an
explicit send action. Nothing the decoder hears can ever initiate a
transmission.

### How many signals can be decoded at once

Decoding cost grows with the number of signals actually being decoded, not with
the width of the band you are watching and not simply with the number being
tracked. Building the spectrum costs the same
whether one signal is present or twenty-four; each signal that reaches a
decoder then adds its own filtering on top, and a tracked signal that will not
be decoded is skipped before that filtering rather than paying for it.

Measured on a current desktop, a single decoded signal uses around eight per
cent of one processor core in real time, and twenty-four — the maximum the
decoder tracks — reaches real time, meaning the machine is doing a second of
work for every second of radio. A slower machine will reach that ceiling
sooner. If a crowded band feels sluggish, narrowing the decoder window so fewer
signals are tracked helps far more than reducing the spectrum's resolution.

### Decoding weak signals

**Settings → Decoder → Weak signals** decides which tracked signals are handed
to a decoder at all. It is off by default, and the threshold beside it,
**Decode only above**, starts at 4.0 dB above the noise floor. A signal that
has not reached that level is still detected, still followed, and still drawn
in the spectrum; only its decoding is withheld. It is not suspended, and it is
not hidden.

The level that decides is the highest the track has reached rather than the
level of the moment, because a signal is necessarily weak while it is still
being acquired, and every new track is filtered for a fixed warm-up period
before the threshold can apply to it at all. A track you select yourself is
always decoded regardless of level, and so is one you are monitoring through a
card speaker.

The default was 12 dB, a figure taken from the capture corpus, where the
weakest track carrying a correctly recovered callsign sat at 19.5 dB. That
corpus is twenty-two recordings made through one receiver, and it was never the
whole population: on air the gate proved aggressive enough to suppress workable
signals, so the default is 4 dB. Corpus recovery is unchanged at the lower
setting. Below the threshold the decoder receives fragments rather than copy,
so the transcript fills with nothing while each such track costs a full
decoder's work. Tick
**Decode every tracked signal** when you would rather have that fragmentary
output than none — when chasing a signal you know is there and can barely
hear — and expect both the transcript and the processor cost to reflect it.

For initial hardware acceptance, connect the transceiver to a dummy load, use
minimum power, keep an independent means of removing power available, and
verify a message, cancellation, watchdog release, and emergency release before
any on-air use. The measured serial loopback proves only the selected control
and sense paths; it does not certify the radio interface or replace this
dummy-load acceptance.

**Auto-QSO suggestions** only prepares an operator-visible suggestion when the
selected stream contains a listening cue such as `CQ`, `QRZ`, or `UP`, or when
your exact callsign is decoded. A callsign within two edits of yours must appear
twice in the raw acoustic transcript before it can propose repeating your call.
It never arms, confirms, retunes, or keys.

When the selected card has checked live-radio RF context, **Anchor runner at
700 Hz** (or the profile's configured CW reference) explicitly retunes RX so
that station lands on the guide. TX frequency, split, and mode are not changed.
For normal `UP` operation, callers then appear at higher audio frequencies to
the runner's right. The control is unavailable for WAV/AF-only streams or a
read-only/unlinked provider. It is not yet a quiet-slot TX selector.
**EMERGENCY RELEASE** clears pending transmission state and latches a fault;
resetting that fault leaves TX disarmed.

**TUNE** is an armed, operator-only PTT/KEY toggle: press once to start and
again to release. It cannot be restarted to extend the interval and releases
automatically at the independent 15-second hardware deadline. The Radio
Control and QSO controls invoke the same guarded action. Decoder output and
Auto-QSO suggestions have no route to TUNE or direct keying.
New provisional characters are appended immediately and later refinement may
correct only the unsettled tail. The existing text document is not rebuilt for
ordinary appends, so the view stays where it is instead of shifting as each
character arrives. When the transcript is following the tail it stays pinned
to the bottom in the same frame the text grows; scrolling upward suspends that
automatic following. A sustained inter-transmission pause starts one clean new
line, while spaces inside a transmission are inserted only when supported by
cadence evidence. The transcript remains plain text so incoming characters
cannot cause styled text or scrollbar-driven line reflow. Short content fills the complete
transcript viewport instead of leaving a differently sized inner box; longer
content grows vertically inside the same scroller and its scrollbar appears
only when needed. A confirmed remote callsign
is bold and shown in the card header using the stream color. A completed
two-callsign handover shows both participants in that header. The guarded TX
button includes the exact callsign it will select, so a two-party card never
hides the target behind an ambiguous generic **TX** label. If
stable text contains an exact match for **Settings → Station → Own station
callsign**, the card displays **YOUR CALL HEARD** and its border flashes five
times. This notification is visual and receive-only;
it never arms or starts transmission. The session remains selected if the same frequency/color is
reacquired under a replacement tracker ID. Up to 2,048 stable text characters
remain visible across that replacement, but the confirmed callsign is cleared
until the replacement's own acoustic suffix establishes it. The prior
transcript is carried forward once rather than
being appended repeatedly during live refreshes. Simultaneous nearby decoded
signals keep separate cards and colors; only a later return after the previous
track has ended inherits that track's retained identity. Closing a card with **×** does not
stop its DSP; click the marker to reopen it. Drag the card's handle to set the
preferred order, or focus the handle and press Up/Down for keyboard reordering.
The card prefers
the append-only phase/timing consensus once it has stable content and uses the
literal greedy decoder only while that consensus is unavailable. This makes
compressed manual character and word gaps easier to read. Its newest roughly
one second remains provisional until at least six later mark/gap observations
support it; a brief detector dropout therefore does not freeze a premature
guess, while sustained silence still closes the transmission. Callsign labels
compare the literal and refined paths: shared evidence wins disagreements, and
splitting a prosign from a call-shaped fragment is not sufficient by itself to
create a label. Stable text, amber
provisional text/elements, adaptive WPM, SNR, confidence, drift, and selected
filter width update in place. Competitive acoustic timing paths remain
available for callsign selection and debug capture; they are not appended as a
second competing transcript.
A completed turn may display conservatively reconstructed word boundaries, but
only when an acoustically competitive path contains exactly the same decoded
non-whitespace characters. Context never changes a letter, digit, punctuation
mark, raw transcript, phase-consensus transcript, callsign confirmation, or CW
verification decision. The current-sender label and its WPM appear only after
strong handover evidence; bare or repeated calls and an isolated `DE CALL`
fragment are intentionally insufficient.
A keyed gap does not immediately discard a track; decoded tracks are retained
for a configurable timeout (Settings → Display → **Decoded signal timeout**,
default 30 seconds, configurable up to 300 seconds) so normal word and message
gaps preserve identity. Silence retains rather than invalidates the verified
observation. The filled area and center line clear
while inactive, leaving a short horizontal identity-color mark on the frequency
axis. The stream label is 18 px normally and magnifies to 32 px while the marker
is hovered.
Retention preserves identity and text only: without a current matched peak it
cannot keep the area active or generate CW-symbol rows from residual noise.
Spectrum association and decoder input bridge ordinary word gaps for 750 ms.
After that unmatched interval, the decoder forces key-up, drains its pending
acoustic segment once, and stops accepting narrowband audio until a candidate
at that carrier is matched again. This alone is not a transmission boundary:
on reacquisition, only the longer sustained-silence rule completes a turn, so
a slow operator's ordinary word gap is preserved. This prevents residual energy or a nearby
station inside the analysis-filter skirt from extending the retained
transcript. The filled area clears at the same boundary. This hold is
independent of the longer six-second verification-exit and configured
marker-retention timers. At first verification the marker corrects an initially
biased acquisition from recent consistent carrier measurements. It then
follows only sustained, coherent carrier motion slowly; short peak jitter,
nearby signals, silence, and noise cannot move it, resize it, or change its
retained color. The label grows to 32 px on hover.
If the marker eventually expires, its carrier keeps
the same reserved color for at least five minutes and reuses it when recognized
again; a new track ID therefore does not make the same frequency look like a
different station merely because one pass ended.

Peak shape is measured in hertz rather than a fixed number of FFT bins. A small
near-shape requirement rejects broad shoulders, while farther references allow
the wider peak produced by a real receiver/audio path to enter private
tracking. Therefore a visible narrow trace may take several characters before
its marker appears, but a steady carrier or broadband level change should not
be presented as a decoded station. A genuinely broad spectral feature
(adjacent SSB audio, AGC pumping, a receiver-filter skirt) is expected to
never appear as a candidate at all, because its shape fails the
local-prominence check before any track is created.

Frequency text is written vertically beside the matching colored area inside
the upper spectrum region, never over waterfall history. Until a callsign is
confirmed the label contains only its frequency; the confirmed callsign then
replaces it. Callsign text remains
hidden until the track is verified, the decoder has promoted the text to stable,
a word gap confirms that the structurally valid token is complete, and decoded
exchange context (`DE`, `CQ`, `TU`, `UP`, `PSE K`, `K`, `KN`, `AR`, or `SK`)
or exact repetition supports it.
The candidate may come from the literal path or the append-only acoustic
consensus, but it must pass the same context/repetition policy. This is
signal/timing and text-context evidence, not external directory validation. The marker
tooltip exposes the same confirmed call, frequency, audio tone, filter width,
and measured drift.

The FFT-bin tracker discovers candidate frequencies, but it does not provide
the key-up/key-down evidence. For both live audio and WAV replay, the DSP worker
uses the original samples to mix each tracked tone to baseband, evaluates
three-stage 60, 120, and 240 Hz paths, compares their energy with independently
smoothed references on both sides, and supplies new soft evidence 500 times per
second to that track's adaptive timing decoder. The 120 Hz path remains fixed
during initial acquisition; the decoder can then narrow a clean slow signal or
widen a fast/drifting signal. This is deliberately independent of spectrum averaging,
waterfall levels, display gain, and the 700 Hz visual guide.

Each new track evaluates nine timing starts from 8 through 60 WPM. During at
least the first 2.5 seconds after keyed evidence begins, the best current path
is intentionally shown as amber provisional text because it may change as
slower hypotheses gain enough evidence. After the timing score and
decoded-symbol threshold pass, one path becomes the presentation leader while
all nine fixed speed anchors keep processing. A gap of at least 2.5 seconds
reselects the best complete path, preserves stable text, and starts a fresh
speed acquisition, so an early choice cannot permanently disable alternatives
and another sender can use a substantially different speed. Quality is judged
over bounded recent evidence, allowing a rough acquisition to recover. Short
or ambiguous fragments may remain provisional rather than being presented as
certain.

In parallel, a bounded timing lattice uses shared cadence evidence to revisit ambiguous dit/dah and
character/word-gap boundaries every 500 ms and at completed gaps. It retains at
most four competitive acoustic paths. Only a common prefix with sufficient
evidence crosses the append-only boundary during continuous reception. At an
explicit completed-transmission boundary, the best bounded path finalizes a
remaining ambiguous suffix instead of losing it. This specifically helps
manual keying and compressed spacing; it cannot reconstruct two exactly co-channel stations
whose simultaneous marks have already merged into one envelope.

A colored verified marker is intentionally delayed until the complete evidence
set remains valid for roughly half a second. Brief fades then receive a longer
hold before removal, reducing both transient false markers and visible flapping.
No-signal evidence cannot demote a verified marker before the configured
decoded-signal timeout; contradictory evidence from an active carrier can.
Cadence, pure timing, and blended character confidence remain separate gates;
one strong metric cannot substitute for a failing one.
An extremely short fragment can therefore end while still provisional; this is
an abstention, not evidence that its carrier was absent from the symbols view.
Development builds also preserve the final verification summary when the
hosted live-audio acceptance fixture fails, so a release is not advanced on an
opaque or unexplained decoder result.

Element timing is measured without a length bias. The keying decision is taken
in the linear power domain from separately measured space and mark levels. A
responsive tracker follows fading and manual weighting, while a bounded recent
history anchors it to robust low and high populations only when they are
clearly separated; a broad noise population cannot supply that anchor. This
keeps ambiguous keying-edge samples from pulling both levels together. The
smoothing applied before the decision scales with the element length being
tracked rather than being fixed. In practice this removes the strong speed
dependence the decoder used to have and improves the generated speed/noise
surface, but it does not make every field transcript correct. The element
length is measured from a mark together with the
gap that follows it, whose combined length does not depend on how heavily the
operator weights their sending, so bug and hand-key styles are no longer
penalised the way they were. The separately displayed/per-sender acoustic WPM
uses the same paired-duration principle, but filter width and decoded
characters retain an independent mark/gap timing estimate. This prevents a
better speed readout from silently changing text or publishing a different
callsign. Weak signals below roughly 15 dB remain
unreliable, very light machine weighting is slightly worse than heavy, and
speeds near 50 WPM are currently limited by the narrowband filter width rather
than by timing.

The baseline handles letters, digits, common punctuation, selected prosigns,
sub-bin drift tracking, and automatic filter width selection, but does not yet
provide calibrated confidence, multiple-pass weak-signal recovery, or separation of
callers occupying the same frequency. Signals closer than about 45 Hz may
therefore appear as one track, and noise or non-CW
carriers may produce `?` or incorrect text. Changing the audio source or
processing bandwidth clears decoder state. Decoder output cannot arm TX, key a
radio, or initiate a QSO.

An independently trained acoustic likelihood model was explored during
development and is not part of the project today, so no released application
loads one. The live primary path uses the deterministic narrowband envelope and
timing decoder. Such a model would be enabled only after it improves locked
receiver recordings at character level, preserves no-CW safety, fits the
CPU/memory budget on every packaged architecture, and keeps the deterministic
path available as fallback.

Builds with the optional local character backend can additionally run an
operator-supplied ONNX character model selected under **Settings → Decoder**.
The application supplies no model and performs no download. Once enabled and
validated, up to four verified, Morse-likely, or manually selected tracks are
refined on a CPU worker. The card shows delayed append-only output in a separate
**LOCAL MODEL** block. Strong characters normally require two overlapping
eight-second windows; a moderate-confidence character needs three aligned
windows and remains provisional with only two. Deterministic text remains
separate. A
structurally valid call confirmed across overlapping model windows may complete
verification after the carrier has independently passed the ordinary spectral,
keying, cadence, and coherence checks and the sustained-entry interval. It then
appears in the header with a **MODEL** badge. Model output cannot create a
carrier, keep silence active, replace raw text, or initiate transmission.

Treat `?` as retained acoustic uncertainty, not as a character that a directory
has disproved.

A run of six or more of the one- and two-element characters — E, T, I, A, N and
M, with any spaces inside the run counted as part of the same damage — is
replaced in the published transcript by a single space. When keying evidence
breaks up, those are the characters it breaks up into, and a long run of them
is the signature of that rather than of copy. The run is replaced rather than
deleted so the gap says plainly that something here was not readable, instead
of joining unrelated text together. Six is the shortest run that is safe:
ordinary copy really does reach four and five, and a track that has recovered a
correct callsign several times can still produce a run of ten between the good
passes, which is why only the run is suppressed and never the whole track.

### The CW vocabulary the decoder reads

The abbreviations, Q-codes and prosigns the decoder recognises are plain text
files you can edit, not a fixed list inside the application. On first run they
are written to `dictionaries/` inside the application data directory, and an
edit you make there survives an upgrade.
`cw-abbreviations.txt` holds the vocabulary itself, one token per line, with
blank lines and lines beginning with `#` ignored. `cw-word-gap-prefixes.txt`
holds the smaller set that may run straight into a callsign, which is what turns
a run-together `CQDE` reading into `CQ DE`; every entry there must also appear in
the abbreviations file.

`cw-distinctive-tokens.txt` is a much shorter list, and it is not a place to
add favourites. A track carrying one of these is accepted as real CW on that
evidence alone, which works only while a match stays harder to counterfeit than
the checks it stands in for. `CQ` and `599` belong there; a single letter, or
anything noise assembles often, does not.

Upgrading never requires anything of you. The copy inside the application is
the authoritative one, and your copy is an override that has to earn its place:
it is used only when it is present and actually parses to something usable, and
for the alphabet only when it still carries the letters and digits. A file left
by an older version, truncated or half-written, therefore cannot change how a
new version decodes. A missing or unusable file is rewritten from the copy
inside the application so that what you see on disk is what is in force; a
usable file you have edited is never overwritten.

The four contest exchange profiles in `dictionaries/contests/` are seeded the
same way and read back from your directory, so an updated or added contest
needs no new build. Each profile describes what a contest exchange looks like —
which fields are sent and received, and how they may be abbreviated — and a
profile can never arm a transmitter or relax a safety gate. A profile you add
yourself is read alongside them; only the four carried inside the application
are ever written out.

The alphabet goes one step further than the other files. A copy of it is
compiled into the application at build time, generated from the same shipped
file so the two can never drift apart, and that copy is used if every attempt
to read a file fails. A decoder with no alphabet decodes nothing at all, so a
missing or empty one cannot stop it working.

`morse-alphabet.txt` in the same directory holds the alphabet itself: the
element pattern on the left, the symbol it produces on the right. Extend it if
you work stations sending accented letters or a prosign the decoder does not yet
name. It is read only for receiving. What the application may transmit is a
separate, shorter list held in code rather than in any file, precisely because
what may go on the air is a safety boundary; adding a prosign here therefore
lets you read it and never adds it to what can be sent.

**REGION** appears beside OFF and RX while Live SDR is the source, and plays the
whole decode region at once. Every signal inside the window is heard together,
each at the pitch its own position in the region gives it: a higher tone is a
station higher in the region, and two stations 400 Hz apart are heard 400 Hz
apart. Use it to sweep a window by ear before deciding what to open. It is not a
decoder card's speaker, which isolates one carrier and moves it to the
configured CW reference tone -- if you expect the sidetone pitch and hear a
spread of pitches, you are listening to the region and it is working correctly.

Region audio is 48 kHz mono, and that is what fixes the 24 kHz limit on the
decode region: real audio sampled at 48 kHz carries 24 kHz of bandwidth and no
more. A narrow region is the more useful setting here -- at 3 kHz the whole
window sits inside comfortable listening pitches, while at the full 24 kHz the
upper part runs past what most people can hear. Region audio is produced only
while you are listening to it, a remote observer is being sent it, or a debug
capture is running.

A debug capture taken from an SDR now writes this same audio to `audio.wav`
beside the IQ recording, so a capture can be listened to as well as analysed.
The two are the same reception: the WAV is exactly what REGION listening plays.

`callsign-prefixes.txt` decides which decoded tokens are allowed to name a
station. Every amateur callsign opens with a prefix some administration was
allocated, so a token whose opening characters belong to no country is far more
likely to be a decode that lost an element than a rare station: one missed dot
turns a real prefix into an impossible one. A label that fails this check is
withheld and the stream stays unnamed until the station identifies cleanly. The
transcript is never touched -- you still read exactly what was copied, and can
judge it yourself.

Edit it if you see a real station refused. The file errs towards admitting and
should stay that way: if it is missing, empty or unreadable, every callsign is
admitted, because refusing every station on the band would be a far worse fault
than the misdecodes this catches. Removing blocks is the one edit that can do
harm.

These lists only ever choose between readings that carry exactly the same
characters, so adding a token can move a word boundary and can never change a
decoded letter. Adding something that is not real CW therefore costs you
accuracy in spacing rather than correctness in text. A longer token counts for
more than a short one, because a single letter falls out of almost any spacing
by chance while a three-character Q-code does not. If the files are missing or
empty the decoder simply works from timing alone. Settings → Decoder can use an operator-selected offline
`master.scp`/Call History file or an optional managed `MASTER.SCP` copy
downloaded directly from the Super Check Partial (SCP) Database. Managed use
and automatic checking are independently switchable. Automatic checks run at
most daily and download only a changed release; **Check for updates** starts an
immediate operator-requested check. The last valid copy remains usable offline
and is preserved after any network, validation, or write failure. SCP is an
activity-derived contesting aid maintained by W9KKN, not an official callsign
register, and is not bundled with CW Buddy.

If at least two current competitive acoustic paths agree on the same strongest
complete callsign, and it is within two wildcard-aware substitutions,
insertions, or deletions of a completed uncertain call-shaped span, the
marker/card may show `≈ CALL`. An exact list hit carries a **DB** badge; if the
stronger acoustic winner is absent from the list it remains visible as
**AUDIO** instead of being displaced by a weaker database candidate. Ambiguous
evidence causes abstention. The
approximation sign is intentional: the transcript remains unchanged, and the
suggestion cannot confirm the callsign, verify a stream, trigger the own-call
alert, or control transmission. Acoustic alternatives remain
available in diagnostic capture, while the card
shows the deterministic phase/timing consensus when available and falls back
to the literal acquisition path otherwise. A future online callbook, activity
list, or cluster spot must preserve the same separation and provenance.
**AUDIO** suggestions remain available without downloading a callsign list;
the list only changes the badge to **DB** when it independently contains the
same acoustic winner.

## Choose the keying model for the sender

**Settings → Decoder → Keying model** selects how the decoder decides where the
key goes down and comes back up. Two are available, and neither is better in
general — they suit different senders.

```mermaid
flowchart TB
  Q{"How is the station sending?"}
  Q -->|"By hand, or with a bug"| T["Adaptive threshold<br/>(the default)"]
  Q -->|"Timing sounds uneven"| T
  Q -->|"Keyer or computer"| S["Semi-Markov (HSMM)"]
  Q -->|"Heavy or light weighting"| S
  Q -->|"Wide Farnsworth spacing"| S
  T --> N["Follows the envelope moment by moment.<br/>Shows text soonest."]
  S --> M["Weighs each mark and gap against the<br/>lengths Morse expects. About one<br/>character more delay."]
```

Choose by how the station sounds. Hand and bug sending has timing that wanders,
and the adaptive threshold copes with that far better — roughly three to four
times fewer character errors on a signal with ten per cent timing jitter. A
keyer or computer sends to a machine's timing, and a station using heavy or
light weighting, or wide Farnsworth spacing, is systematically off the textbook
ratios rather than random; the semi-Markov model handles that better, roughly
halving character errors on heavy weighting.

If you are not sure, leave it on the default. On general accuracy across speeds
and signal levels the two measure the same, so the choice is only worth making
when you can hear what kind of sending it is.

You can switch while a station is still sending. The decoder restarts but the
stream, its colour and its history are kept, so you can hear the same station
under both and keep whichever reads better.

## Tell the decoder what you are doing

The current release provides the neutral behavior described below. The four
first-level operating modes — **Standard**, **PileUp Chaser**, **PileUp
Slicer**, and **Runner** — are planned rather than implemented: no part of the
application offers them today, and the left rail lists **RX**, **CALLS**,
**QSO**, **LOG** and **REMOTE** instead. As specified, Chaser would first learn
a runner's listening pattern without transmitting, Slicer would rank
comparatively clear slots, and Runner would organize callers in simplex or
split operation. See the public
[operating-mode specification](../operating-modes.md). None of them would
bypass TX arming or confirmation.

**Settings → Station → Operating role** tells the decoder whose callsign a
stream is expected to carry. It matters because the text alone is sometimes
genuinely ambiguous: `TU` comes before a runner identifying itself, and equally
before the station it has just worked.

- **Monitoring** — the default. No assumption is made; the exchange text alone
  decides.
- **Search and pounce** — you are hunting stations that are calling. The station
  you are listening to is a runner, so its own call is the one shown.
- **Running** — you are calling and others answer, so the stream is somebody
  answering you.

In every role your own callsign is never used to label another station's
stream, so a station calling you is still labelled with *its* call rather than
losing its label. Your call being heard is separate and still raises the
**YOUR CALL HEARD** notification.

The role only changes which candidate is chosen as the label. It never changes,
corrects, or invents the decoded text itself.

## Debug capture

When a visible signal will not decode and the on-demand **Diagnostics**
readout is not enough to explain why, use **Debug capture** in the decoder
panel header, or **Start capture** under **Settings → Decoder**. Both run the
same recording, and it is only available while live
RX is running. Selecting it
starts a bounded recording:

- The exact samples feeding the decoder. A sound-card source is written to
  `audio.wav`; a direct SDR source is written instead as the SigMF IQ pair
  described above, because discarding the quadrature component would destroy
  the sideband distinction that a later RF analysis needs.
- A private per-track diagnostic log, `diagnostics.jsonl`, with one line per
  second listing every currently tracked frequency — including tracks that
  never become visible — with its SNR, narrowband coherence, filter width,
  verification state and reason, spectral observations, key transitions,
  decoded/unknown symbol counts, timing/cadence quality, WPM, both provisional
  and stable decoded text, and the presented callsign with
  phase-consensus/literal/retained provenance. Capture context states whether
  decoder tracks existed before recording began. Presentation diagnostics show
  local-model/offline-database state and each callsign suggestion or its
  rejection reason. Completed transmission turns, explicit current-sender
  evidence, and its supported cadence estimate are included as separate fields.
  Each line also records the linked radio's RX/TX frequency
  and split state at that instant, so reviewing the
  file shows whether (and exactly when) the VFO moved during the capture —
  a common explanation for a signal that stops decoding partway through.

The files are written to a timestamped folder under the application's
standard per-user data location; the panel shows the exact path while
recording and after it stops, and **Settings → Decoder** has an **Open capture
folder** button that opens it in your file manager. Capture stops itself after
the **Stop automatically after** value in Settings (30 to 1800 seconds, 300 by
default) and always requires an explicit click to start; it is never silent or
automatic. Select **Stop capture** to end it early. Raise the limit for a signal
that only misbehaves occasionally; lower it for a quick reproduction, so there
is less to review before sharing. Because the recording is exactly what the
selected input picked up, review its contents before sharing the
capture folder with anyone.

## Replay a receiver recording

1. Choose **WAV replay**, select **Open WAV**, and choose a local recording.
2. Review the detected filename, sample rate, and duration.
3. Select **Play**. The upper trace is the current Hann-windowed FFT; the lower
   panel is the scrolling waterfall. Select **Audio spectrum** for smoothed
   spectral history or **CW symbols** for crisp verified-channel acoustic
   dit/dah/gap rows on a neutral background. The symbols view is keyed envelope
   evidence, not reconstructed decoder text.
4. Use **Pause** to retain the current display or **Stop** to return to the
   beginning. Opening another file clears the previous display.

The progress bar and elapsed time follow sample-derived recording time rather
than wall-clock guesses. The first integration does not yet provide seeking or
looping. Frequency labels cover 0 Hz through half the WAV sample rate because
ordinary WAV replay is treated as real-valued audio, not complex I/Q.

When a transmission ends, the decoder may compare the completed mark/gap
sequence against the other timing speeds it was already tracking. It adopts an
alternative only when the acoustic fit improves clearly, extends already
committed text cleanly, and no near-tied physical interpretation disagrees.
This can correct a bad early speed lock, but it does not rewrite text while the
station is still sending and it does not use a callsign database or conversation
guess to manufacture characters.

Debug captures record a `timingFingerprint` for each completed turn when its
retained mark/gap sequence is complete and contiguous. It summarizes physical
timing only; a missing value is reported as unavailable, and a present value
does not identify an operator or confirm a callsign.

Open **Settings → Display** to select the redraw target, waterfall row rate,
automatic or manual dBFS range, DSP averaging from 1 to 32 frames, and the
profile-persisted spectrum view. Switching views while live or replaying does
not reset the decoder. Automatic range uses a smoothed robust estimate so an isolated
strong bin does not repeatedly rescale the entire view. These values are saved
independently in each station profile.

## About and author

Open **Settings → About** to see the application version, license, author, and
author website. The displayed author is **Alessio Bravi (IU0LFQ / AD2FC)** and
the **Author Website** button opens [https://iu0lfq.it/](https://iu0lfq.it/) in
the system browser. The application, taskbar/dock entry, and installed shortcuts
use the same Morse-key dot/dash mark.

Continuous downloads are published only after every supported-platform build,
test, staged-layout check, and native startup smoke test succeeds. A failed
matrix does not replace the last fully verified download.

The displayed version is the same `major.minor.revision` value embedded in the
native installer/package and application metadata. Continuous builds use the
GitHub workflow run number as the revision, so a hosted build may show, for
example, `0.1.245`; a default local development build shows `0.1.0`.

## Checking for updates

The same **Settings → About** page checks for updates. A background check
runs a few seconds after every launch (uncheck **Automatically check for
updates** to disable it), and **Check for updates** runs one on demand,
showing when it last ran and whether a newer version is published.
During the short interval in which continuous-release files are being replaced,
the previous manifest remains available and the application retries transient
404, timeout, and server errors. A persistent failure is still reported after
three attempts; it is never treated as an available or verified update.

When an update is available, **Download update** fetches this platform's
installer/package to your Downloads folder and verifies its checksum against
the published `SHA256SUMS` before keeping it — a failed or mismatched
download is discarded automatically, never silently kept. Once verified,
**Open Installer** and the platform-specific reveal action replace **Download
update** in the same action row. The startup update notice follows the same
sequence: it shows download and checksum-verification status, then replaces
its download button with **Open Installer** and the platform reveal action
without requiring you to find the About page. **Open Installer** hands it to
the OS's own
installer or package manager
(the Windows MSI installer, the Linux package tool, or an archive tool on
the portable builds) so you complete the install the normal way; **Show in
Finder**, **Show in File Explorer**, or **Show in Folder** reveals it instead.
The application never downloads or installs an application update without you
clicking these buttons, and never silently replaces itself while running. This
is separate from an explicitly enabled callsign-database refresh, which only
replaces its validated local data cache.

## First launch

1. Start `cw-buddy-desktop`.
2. Enter a descriptive station profile name, such as `HF desk` or
   `Satellite station`.
3. For audio-only decoding, select **No radio — receive-only audio decoding
   (SWL)**. The wizard skips CAT and Keying after the Audio step.
4. For radio operation, select a positively identified online radio. On
   Windows, the initial detector reads the online state and model name from the
   installed OmniRig service; it does not send probe commands to arbitrary COM
   ports.
5. If the radio cannot be identified, select **Set up a radio manually**, choose
   the nearest reference template, then choose the frequency provider. Configure
   the physical COM/framing values in OmniRig, `rigctld`, or CAT4OM itself; the
   wizard shows only the slot, endpoint, VFO, identity, and permission fields
   that CW Buddy actually consumes.
6. On **Audio**, select the sound-card input carrying receiver audio. **System
   default input** follows the operating-system default when devices change.
   This step is always present for radio and SWL profiles. For a controlled
   radio, also confirm **This input carries RX audio from this radio** if the
   selected device is physically connected to that receiver.
7. Select a physically separate direct-COM key/PTT interface. Port enumeration
   never toggles RTS or DTR.
8. Review the display defaults and finish the wizard.

The Back and Next controls live in a fixed wizard footer and remain visible when
a setup page must scroll on a small or scaled display.

Finishing the wizard saves settings only; it never opens a keying port and
never transmits. Hardware ownership, the measured keying loopback, and the
transmit guard all remain required before anything can be keyed, and they are
performed from Settings → Keying and the QSO panel rather than from the wizard.

Selecting SWL mode persists that choice per profile, disables radio/keying
validation, and labels the workspace as receive-only. Previously entered radio
values are retained so switching the profile back to radio operation does not
discard configuration.

The current build discovers, displays, and saves audio-input selection, including
an unavailable marker when a previously selected device is disconnected. Live
sound-card capture runs from that selection, as described under Receive live
radio audio. The advanced input controls — level metering and explicit
channel, sample-rate and buffer selection — are still planned; live RX asks the
device for 48 kHz mono float and otherwise takes its preferred format.

## Multiple radios and application instances

Create one named profile for each independent station chain. If more than one
profile exists, the startup helper asks which profile to open. A shortcut or
automation can bypass the helper:

```text
cw-buddy-desktop --profile "HF desk"
cw-buddy-desktop --profile "Satellite station"
```

Two application processes may use different profiles. Future device locks will
prevent both processes from opening the same serial, audio, or SDR device.

## Frequency, split, and transverter terminology

- **RX dial frequency** is the frequency reported to or requested from the
  radio for reception.
- **TX dial frequency** is independent when split is enabled.
- **RX/TX transverter offset** is a signed integer in hertz. It is used to
  calculate actual RF for display and logging; the offset is never sent to a
  direct CAT radio by accident.
- **CW audio-to-RF mapping** selects CW-U/USB or CW-L/LSB direction. For a live
  linked input, each signal is calculated as `actual RX RF + direction ×
  (decoded audio tone − configured CW pitch)`.

Actual-RF marker labels are enabled only while live capture is running, the
profile explicitly links that audio input to the radio, and the selected
frequency provider has a valid state. Windows OmniRig is polled for its online
state, Hamlib publishes only complete rigctld polls after verifying `--vfo`,
and CAT4OM uses its pushed radio state. The RX transverter offset is
applied before tone mapping. Recordings, SWL profiles, unlinked inputs, and
unavailable frequency providers deliberately show **AF** rather than guessing.

The **Radio Control** panel shows the resolved radio state as a compact faceplate
above the signal list: grouped whole-hertz RX and TX digits, a dim/red ON AIR
area, explicit VFO and SIMPLEX/SPLIT state, provider-reported RX mode, a
separate CW/CW-R operator TX target, and an orange TX frequency. Unknown
frequency, split, VFO, or observed mode state is shown as unavailable; CW Buddy
never manufactures a simplex value or derives observed radio state from its
audio decoder. The faceplate disappears
entirely for receive-only SWL setups, WAV replay, and whenever no radio is
currently linked, rather than showing a stale or meaningless value. Note
that showing this readout at all requires **both** Settings → Radio
**Radio enabled** and Settings → Audio **This input carries RX audio from
the configured radio** — enabling
the radio alone is not enough.

When the linked provider is writable, click the green LCD-style **RX**
frequency to edit it in the displayed unit. Press Enter to request the exact
value; press Escape or click elsewhere to cancel and restore the readout. The
`<` and `>` controls at the left and right edges of the waterfall
tune RX down or up by the profile's **RX tuning step**; the default is 1 kHz.
Settings → Radio allows whole-kHz steps from 1 to 100 kHz. The controls are
hidden for WAV/SWL operation and read-only, disconnected, non-master, or
otherwise incapable providers.

When the linked provider reports the matching write capability, click the TX
frequency to enter an independent actual-RF value, click the split badge to
toggle split, and click the RX mode badge to cycle supported receive modes.
The TX mode badge always toggles the profile's CW/CW-R operator target. It says
**CONFIRMED** only when the provider reports that exact mode; otherwise it says
**TARGET**, and its tooltip explains whether the provider can request or read
the mode. A read-only backend keeps the chosen target but does not claim to
have changed the radio. Entering a separate TX frequency explicitly enables
split if the provider supports both operations; a connection or state refresh
never changes the rig merely to make the faceplate complete.

Use **A=B** to copy the checked VFO A/RX actual-RF frequency into VFO B/TX.
The same provider-neutral route performs independent transverter-offset
conversion and enables split when the backend advertises both operations. A=B
is disabled when RX state is unknown or either required capability is absent.
It changes frequency only: it never copies the RX mode into the CW/CW-R TX
target.

The faceplate gives ON AIR, RX mode, SIMPLEX/SPLIT, TX mode, and A=B the same
compact tile dimensions. ON AIR uses a smaller status symbol so the frequencies
remain visually dominant. At the minimum decoder-pane width, the TX caption
moves above its digits and both RX and TX digits scale to fit rather than
becoming `…`; grouped frequencies through 99 GHz are accommodated. The
RX and TX mode tiles occupy the same rightmost position in their VFO rows. The
right-hand **Radio Control** heading owns this faceplate. The separate
**CW Decoder** heading below it owns Diagnostics and Debug capture, so receiver
control state and decoding tools are not presented as one panel.

The orange **TUNE** tile sits directly beneath the borderless ON AIR indicator;
while active it displays the remaining watchdog seconds rounded up, and a
second press releases KEY and PTT immediately.
SPLIT and A=B are centered in the remaining lower-row space. The tile invokes
the same guarded action as TUNE in the QSO panel, remains disabled until TX is
explicitly armed, never bypasses the hardware-readiness gate, and retains the
hard 15-second continuous-KEY watchdog.

The entered value is actual RF, not necessarily the radio dial. CW Buddy
removes the configured RX transverter offset with checked integer-Hz arithmetic
before sending the provider request. RX edits and edge steps target only the
receive VFO, while TX edits target only the transmit VFO. The provider's subsequent
poll/pushed state remains authoritative, so the display changes only when the
radio reports the new frequency. Windows OmniRig tuning is enabled only while
the radio reports online receive state and a writable active RX-frequency
property. Hamlib is read-only unless writes are explicitly enabled in Settings;
it accepts only a local rigctld endpoint because the raw protocol has no
authentication or encryption. Start rigctld with `--vfo`; use a locally
terminated authenticated encrypted tunnel for a remote radio.

Retuning the linked radio's RX VFO while live audio is running follows any
already-identified signal rather than losing it: every tracked signal is
re-centered by the exact amount the RX dial moved (translated to audio Hz
using the configured CW-U/CW-L sideband direction), so its decoded text and
verification carry over across the retune instead of restarting.

Next to the VFO readout is an **ON AIR** indicator. It lights only from the
guarded local keying engine's authoritative KEY state, never because a CAT
request was accepted, a message was queued, or decoder text suggested a reply.

Example satellite station:

```text
RX dial:        29,900,000 Hz
RX offset:     116,000,000 Hz
Actual RX RF:  145,900,000 Hz

TX dial:        28,300,000 Hz
TX offset:     407,000,000 Hz
Actual TX RF:  435,300,000 Hz
Split: enabled
```

The log record derives `FREQ_RX`, `FREQ`, `BAND_RX`, and `BAND` from those
actual RF values. Satellite operation also supplies `PROP_MODE`, `SAT_NAME`,
and `SAT_MODE`. Station equipment rules select `MY_RIG` and `MY_ANTENNA` from
the actual TX/RX bands.

## Safety principles

- Decoder output never starts transmission.
- The first transmission of a QSO requires operator confirmation of the exact
  selected callsign.
- An ignored callsign is refused by the transmit guard and excluded from
  display, queueing, QSO selection, and TX authorization. The desktop shell
  does not yet offer a control for adding one to that list.
- Direct key/PTT is separate from frequency control.
- Network receivers are receive-only and cannot own TX.
- Remote operation, which is still a specification rather than a shipped
  feature, keeps final interlocks and CW timing at the station server.
