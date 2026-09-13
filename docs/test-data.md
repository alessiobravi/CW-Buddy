# Decoder test data

Real recordings are required because synthesized Morse does not reproduce AGC
pumping, oscillator drift, multipath, clicks, adjacent signals, QRM, or operator
timing. Tests should use both generated fixtures with exact ground truth and
off-air recordings with reviewed annotations.

## Located public recording

Freesound sound 243528, “Hams on CW multiple frequencies & pile-up 7005.0kHz
LSB.wav,” is a 110-second, 7,119 Hz, 16-bit mono recording containing several CW
signals and a pileup. Its page identifies the sound as Creative Commons Zero:

https://freesound.org/people/kb7clx/sounds/243528/

Freesound currently requires an account to download it. We may redistribute a
verified download under CC0, but should store its original page URL, creator,
license, SHA-256, sample format, and any preprocessing in the fixture manifest.
No third-party recording has been committed yet.

## Fixture layout (planned)

```text
test-data/
  manifest.json
  audio/
  iq/
  annotations/
```

Large captures should use release assets or external object storage rather than
normal Git history. `manifest.json` records acquisition source, license,
checksum, sample format, center frequency when known, and annotation revision.

## Annotation format

The implemented bounded TSV sidecar binds exactly one WAV by SHA-256 and
records its integer sample rate. Version 1 remains accepted for compatibility
and treats the complete WAV as reviewed. Version 2 requires one or more sorted,
non-overlapping `coverage` sample intervals. Coverage is exhaustive: an
interval without an event explicitly asserts that no CW is present there.
Every event must lie wholly inside reviewed coverage.

Each `event` contains start/end sample indices, audio-tone frequency in hertz,
literal and canonical normalized text, a comma-separated exact callsign set,
and a `0`/`1` uncertainty marker. Lines are canonical UTF-8-compatible text
with LF endings so their review history is portable. They are limited to 4,096
bytes and manifests to 256 records; malformed, duplicate, overlapping,
out-of-order, noncanonical, checksum-mismatched, or out-of-range data fails
closed. An uncertain event is frequency-matched so its real track is not called
a false publication, but it is excluded from CER, WER, and callsign scores.

A registered test exercises the parser: it reads the committed fixture at
`tests/fixtures/receiver_annotations_v1.tsv`, checks the fixture's own SHA-256,
and then feeds the parser deliberately broken sidecars so that overlapping
coverage, an event outside its coverage, coverage declared in a version 1 file,
a malformed checksum field, reversed or out-of-order intervals, and
noncanonical text are each shown to fail closed. The fixture is a deterministic
version 1 sidecar carrying three events and names no receiver recording, so the
parser stays covered without a capture in the repository.

Run an annotated receiver report with:

```sh
cwa_capture_replay --annotations reviewed.tsv audio.wav
```

That target is built with the rest of the test suite but is deliberately not
registered as a ctest, because operator captures are private and are therefore
not CI fixtures; it is run by hand against a local recording. The same applies
to the receive-path profiler, which reports per-stage wall time rather than
gating anything. The spacing benchmark, which needs no external recording
because it generates its own jittered contacts, is registered and runs with a
600-second timeout.

The report matches a track using its frequency during the annotated interval,
then prints character/word error, timestamped published-callsign
precision/recall, separate transcript callsign extractability, first
provisional/stable latency, non-append provisional revisions, and unmatched
publication/callsign episodes inside reviewed coverage only. Co-channel
operators at the same frequency are not yet truthfully attributable. Completed
turn output includes an exact-run timing fingerprint when the retained lattice
is complete, or explicitly reports it unavailable; the fingerprint is a
measurement, not an operator identity. No reviewed receiver recording is
bundled.

## Synthetic matrix

Generate deterministic cases across WPM, weighting, tone frequency, SNR,
frequency drift, fading, impulsive noise, overlapping callers, and timing
jitter. Generated callsigns must include portable and compound forms. The
supported WPM and prosign requirements that bound this matrix are now settled:
speeds run from 8 to 60 WPM, and the transmittable prosigns are the closed set
of seven, so a generated case outside those bounds measures something the
project does not claim.

## Scoring a recorded SigMF capture

A SigMF capture replays through the same decode path as a WAV clip, by naming
either half of the pair:

```sh
cwa_capture_replay --decoder-center-hz 7012396 --decoder-bandwidth-hz 6000 iq.sigmf-data
```

The wide complex block goes through the live receiver's own chain -- overview
transform, subband decimator, decoder transform -- before the channel bank reads
it, and detection sees only the requested window. A capture block fed straight
to the decoder is not audio: every filter and timing constant in the bank is
sized for the decode region rather than for the hardware passband.

The window defaults to the capture's own centre frequency at 24 kHz, which is
frequently not where the operator was listening -- a receiver watching a pileup
16 kHz down from its dial records a capture whose centre holds nothing. The
sidecar's description records the window that was in use. A window whose slice
falls outside the acquired passband is refused before the replay starts, naming
the frequencies that do not fit, rather than decoding nothing while the spectrum
paints normally.

An annotation sidecar bound to a capture carries the digest of the
`.sigmf-data` and `.sigmf-meta` files together, because the sidecar holds the
sample rate and segment centres that decide what a track's absolute RF means.
Its event frequencies are absolute RF.
