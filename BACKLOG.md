# Product backlog

Updated: 2026-09-12

This is the canonical prioritized backlog. Status values are `todo`, `active`,
`blocked`, and `done`. Every source, test, build, or automation change must
review this file and update affected items or the “Last reviewed” note.

Last reviewed: 2026-09-13 (sixty-sixth entry) -- the pileup capture became a
score, and the first thing it scored was a wrong answer nobody could see before.

The replay harness now routes a wide complex block through the receiver's own
two-stage chain, so the owner's 133-second pileup runs in 26 seconds. Both
individually resolvable stations are found within 16 Hz of where independent
analysis put them. What it then showed: exactly one callsign published in the
whole recording, and it was wrong, on the one station whose transcript carries
the right one three times.

Instrumenting all 19462 stream-label readings put the cause upstream of every
rule that had been suspected. The correct callsign is offered zero times.
`CallsignPolicy::best_complete_in_text` has an acceptance floor of 3 and awards
exactly 3 for standing third after a `CQ`, so a one-element misdecode in that
position clears the floor while the true call, standing further in with no
adjacent context word, scores 0 and is refused. The ITU prefix gate cannot
help -- both impostors carry real prefixes, Spain and Cook Islands. Two rules
written for this population were inert on the old corpus for the same reason
nobody had noticed: every clip in it holds one station whose single
identification dominates its text.

Worth recording as method rather than as fact: four candidate fixes were
measured against the benchmark before one was chosen, and two of them cost two
correct callsigns each. The one that shipped costs none and removes the wrong
name without inventing a right one. A station this decoder recovers correctly
only once still cannot be named, and that is now an explicit backlog item with
the offending lines named rather than a mystery.

Also settled by measurement rather than preference: the keying-speed ratio and
the acoustic cadence fit are complementary, not competing. The ratio produced no
estimate at all for 41 of 48 carriers on the capture; the cadence fit answered
all of them but does not refuse a synthetic two-carrier fixture the ratio
catches. Both are kept, either refusing is enough, and the newer measure did not
displace the older one merely for being newer.

Previous review: 2026-09-13 (sixty-fifth entry) -- an operator capture of a real
pileup, and the corpus rather than the decoder turning out to be the constraint.

The owner sent 133 seconds of off-air IQ from a DX pileup: about fifty real
stations in a 6 kHz window, median spacing 70 Hz, minimum 52. One station, the
DX calling CQ, is isolated and decodes cleanly. The rest are calling him from
closer together than any filter can separate, and the decoder was publishing a
transcript for every one of them -- forty pages of noise and one real contact,
which is what "the decoding is not working" meant.

Three things were measured rather than argued, and two of them corrected a
position taken here earlier. Narrowing the filter does not help: at 30 Hz the
timing statistics look plausible and the text stays noise. But the claim that
overlapping signals cannot be separated at all was wrong, and the owner said so
-- a human copies a caller out of a pileup, and the ear does it by grouping
pitch and rhythm over time, not by filtering. Solving neighbouring carriers
jointly, using frequencies that are constant and measurable to about a hertz,
recovers text a filter cannot: on the capture it turned "C? CQBE" into
"CQ CQ DE" on a carrier with neighbours 52 and 58 Hz away. It rescued four of
fifty-one, and the dense centre stays unresolvable, so the gate that withholds
text is a statement about effort spent so far rather than a verdict, and it
records what a later separation stage would need to pick its candidates.

The third measurement is the one worth remembering as a pattern. The track cap
was 24 because the colour-lease table was 24 because a hand-written display
palette had 24 entries -- a presentation constant setting a signal-processing
capacity, through two hops, with no comment anywhere on the chain. It was the
only field in `CwChannelBankConfig` without one, which is what gave it away. It
had also been quietly clipping measurements: every live diagnostics record read
`tracks: 24` pinned flat, which reads as a busy band and was a saturated bank.

Also recorded: the decoder corpus, not the decoder, was the binding constraint
on the two measurements before this one. A rule that is inert across twenty-two
single-station clips has not been shown to be inert on the band it was written
for. The SigMF reader added here is what makes that distinction testable; the
replay harness still needs the wide complex block routed through the subband
decimator before a capture can actually be scored.

Previous review: 2026-09-12 (sixty-fourth entry) -- the jerkiness was measured on
the owner's running station rather than guessed at, and it had two causes, one
on each side of the thread boundary.

Profiling the decoder at the load the station was actually carrying found the
larger one: 47% of all decoder time was spent reconstructing the word gaps of
the active transmission, from the beginning, on every sample block, for every
track, over a transcript that only grows. That is why the application got worse
the longer it ran rather than settling at a cost -- and why no processor meter
showed it, since the growth is on one thread while the machine stays idle.
Remembering the reconstruction against its input halved the decoder's total
cost at one track and removed the growth entirely.

The measurement mattered more than the guess. Three theories were plausible
before profiling -- the model copy crossing the thread boundary, the spectrum
frames, the diagnostics record -- and all three were wrong. The harness that
settled it drives CwChannelBank at a chosen track count and reports the
distribution of per-block cost over the run; the tail and the trend are what
identify this class of fault, never the mean.

Not fixed, and named so it is not rediscovered: the worst blocks remain far
more expensive than the median and that tail still grows with transmission
length. It is `refineCwEventLattice` at a transmission boundary, which decodes
the event lattice once per candidate speed -- up to nine passes. Real work at a
real boundary rather than a repeat, so it wants an algorithmic answer, not a
cache. Measured at two dozen tracks: median 2.0 ms, p99 35 ms, worst 276 ms.

Previous review: 2026-09-12 (sixty-third entry) -- the jerkiness was measured on
the owner's running station rather than guessed at, and it was change
notification, not load.

The remote diagnostics stream earned itself here. Connected to the station live
while it carried two dozen tracks, the counters ruled out most of the search
space in one window: no drain was ever capped, blocks arrived at a steady 31 a
second, and the event loop never stalled once in 226 seconds. What it did show
was the drawing thread blocked 22 ms at the median second against a 16.7 ms
frame -- one or two frames lost a second, every second, which is exactly what
"jerky while the processor is idle" feels like and exactly what a CPU meter
cannot show.

The cause was a fourth instance of the pattern below, one turn further on. The
session list reports only rows that differ, but every measurement on a row is
continuous, so no row was ever equal to itself: on the live station, narrowband
coherence and the keying-level figures differed on 100% of consecutive samples
and signal-to-noise on 98%. Every decoded card was therefore re-evaluated,
transcript text layout included, two dozen times a second. Rounding each
measurement to what the card can actually display fixes it, and the general
lesson is worth more than the fix: a value that crosses to the drawing thread
needs a rate, a bound, *and* a resolution. The first two were already rules
here; the third is new. Publishing at a bounded rate does not help when every
publication claims everything changed.

Two earlier claims in this file were narrower than the evidence: the
decoded-channel model was said to have been fixed by publishing "no faster than
forty milliseconds", and the cost was attributed to deep-copying across the
thread boundary. The rate cap was real and holds -- publishes measured 23 a
second on the station -- but the copy was never the expensive part. Measured,
building the whole model for 24 channels costs 0.071 ms, about 0.2% of one
thread. The expense was always on the far side, in what the notification made
the view redo.

Previous review: 2026-09-12 (sixty-second entry) -- the application grew until it
had to be killed, and the cause was a queue rather than a leak.

The operator's evidence decided it: memory climbing from 244 MB to 668 MB while
the interface died. Spectrum frames were handed to the thread that draws with no
backpressure, and a wide IQ transform makes one frame about 64 kB, so thirty a
second is two megabytes a second whenever that thread is behind -- and 424 MB is
a little over three minutes of it. The growth was self-reinforcing, which is why
it ended in a forced kill rather than merely feeling slow. At most two frames
are in flight now and the rest are dropped, through a single bounded publisher
so that a new emission site cannot reintroduce it. The test fails with exit 20
against the unbounded behaviour.

Worth recording as a pattern, because this is the third fault of the same shape
in two days: publishing to the GUI thread at the rate work is produced rather
than the rate a person can see. The decoded-channel model did it once per
drained block on a five-millisecond timer, the diagnostics record did it on the
same timer, and the spectrum did it per analyser frame with payloads two orders
of magnitude larger. Anything crossing to that thread needs a rate or a
bound and a resolution, all decided when it is written.

IPv6 is not backlog work: it was already carried throughout the diagnostics
service and is now proven at runtime rather than by reading the source -- a case
binds `::1`, streams a record over it, and a mutation that refuses IPv6 binding
makes it fail. Recorded in the changelog instead, on the owner's instruction.

Previous review: 2026-09-12 (sixty-first entry) -- a station can be watched while
it runs, and the application stopped burying the thread that draws.

The freeze came from publishing the decoded-channel model once per drained
block on a five-millisecond timer: each publication deep-copies the model across
a thread boundary and the receiver rebuilds it, so a handful of signals
saturated one thread while the processor looked idle. Published after the drain
and no faster than forty milliseconds now. The diagnostics said nothing about
it, which is why they now carry throughput counters: what the decoder found and
whether the application is keeping up are different questions.

The live stream was built in four lanes against a contract header fixed first.
The lane implementing it declined an instruction and was right to: the brief
asked for a `challenge` field in the greeting while also forbidding any
implication of access control that does not exist, and a field named for an
exchange that cannot happen is exactly that implication. It reports
`authenticated: false` and says why, to the peer as well as the operator.

Previous review: 2026-09-12 (sixtieth entry) -- moving the transmit VFO moved
the receive frequency, and two spectrum gestures were one.

OmniRig's `Freq` is the selected VFO, not the receive VFO. A rig publishing
FreqA and FreqB is read per VFO and this never arises; a rig publishing neither
-- the FT-450D among them -- had its receive frequency read from `Freq`
unconditionally, so with split on, selecting the transmit VFO to set it dragged
the receive frequency with it, and with that the RF axis, the spot band filter
and the decoder's frequency mapping. The reading is now taken only where one
VFO is in play, and the last known receive frequency is held otherwise: no
reading beats one that is wrong exactly when the operator is working the other
VFO. The decision is a pure core function so it is tested on every platform,
because the COM code around it builds only on Windows.

Right-click both pointed the received spectrum and opened a manual decode, so
an operator asking for either always got both; CTRL+RIGHT now opens the manual
decode. The waterfall banding was self-inflicted: the retune slide reset the
conditioner's per-bin baseline, which takes about a second to re-converge, so
every retune painted a band and tuning across a band produced a row of them.
The baseline slides with the rows now.

Radio control is provider-neutral in its semantics and in two of its three
backends, and not in the third. Recorded as CAT-008: OmniRig has no adapter at
all, and the call sites branch on a backend index instead of calling one
interface. The receive-VFO fix above was correct but had to be checked against
Hamlib and CAT4OM by reading them rather than by construction, which is the
argument for the interface.

Direct SDR reception still identifies nothing, and it is NOT understood. A
theory that the sample-timing guard was tripped by arrival jitter was checked
and abandoned: SDR block timestamps are synthesised from a sample counter and
are exact. Waiting on a debug capture rather than guessing a third time.

Previous review: 2026-09-12 (fifty-ninth entry) -- two faults introduced by the
previous two waves, both found by the owner.

Making the decoder window follow the receiver fixed direct SDR reception on
every band and broke retuning: the window is republished on each retune, and
the worker called reset() on the channel bank whenever it changed, so every
track, transcript and identity died each time the radio moved. The samples jump
at a retune; the stations do not. The bank now has noteInputDiscontinuity(),
which restarts the signal path and leaves the tracks standing, and the
out-of-band parking rule written for a VFO move is shared with it rather than
duplicated, so the two cannot disagree about what out of band means.

The RF axis re-evaluated on the wrong signal. Everything it reads is set in
setRadioFrequencyContext, which emits radioFrequencyChanged, while the property
notified on stateChanged. On an audio card the spectrum's own bounds do not
move with the dial, so a 40 m to 20 m change left the ruler printing 7 MHz.

Both now have tests that fail on the old behaviour. The retune test drives only
spectrum bounds -- no shift call at all -- because that is what a receiver
retune actually presents to the bank.

Previous review: 2026-09-12 (fifty-eighth entry) -- UI-008 closes, and a cluster
login may carry an SSID.

Both were built in parallel lanes, and both lanes corrected the brief they were
given. The ruler lane was told to draw only when `axisShowsRf`; on a direct SDR
source that is true while the transform is the identity, so the brief would
have reprinted the RF numbers a second time under an AF heading. It gates on
whether the two scales actually differ instead. The SSID lane found that the
login was written to the socket through `CallsignPolicy::normalize`, which
refuses a hyphen -- an SSID login would have connected and then never sent its
callsign at all, waiting at the node's prompt forever.

The callsign policy itself was deliberately not touched. It is the one
definition of callsign syntax in the program and the decoder judges signals on
the air by it; admitting a hyphen for the sake of a cluster login would have
changed what the decoder accepts. The SSID is validated in the login path only.

Previous review: 2026-09-12 (fifty-seventh entry) -- the two spot subsystems are
one, a retune no longer costs a tracked stream, and the spectrum axis reads RF.

Three faults reported by the owner in one sitting, all of them regressions of
intent rather than of code. A track carried out of the passband by a retune was
expired on the ordinary timeout "exactly as an ordinary lost signal would" --
the comment in `shiftTrackedFrequencies` said so outright, and it was wrong: the
operator turned the dial, so the station's position is known and turning back
puts it at a computable place. It is parked now. The spectrum axis was labelled
in audio while the decoder cards beside it already showed RF, so one screen
named the same signal two ways; that is UI-008, which had been logged and left
twice. And the spot overlay was gated on the deleted web feed's switch, so
joining a cluster drew nothing.

The test for the retune fix had to be rewritten twice before it discriminated:
the first version held the parked track for four seconds, under every retention
bound, so it passed with the fix reverted and proved nothing. It now fails on
the old behaviour, which is the only evidence that matters.

Previous review: 2026-09-11 (fifty-sixth entry) -- direct SDR reception decoded
nothing outside 20 m, and the spot provider has no feed it can parse.

The SDR fault was not in detection. The decoder is fed a narrow slice cut from
the wide capture, and `IqSubbandDecimator::process` refuses a block outright
unless that slice lies wholly inside the passband being acquired. The slice's
centre defaulted to 14.050 MHz and moved only when the operator dragged a
selection in the zoomed view, so any receiver started on another band had every
block refused. Because `drain()` publishes the overview spectrum before it ever
consults the decimator, the failure presented as a perfectly live spectrum and
waterfall with nothing identified -- which is why it survived several rounds of
looking at the detector. The slice now follows the capture. The test drives the
controller rather than the decimator, and then feeds the published window to a
real decimator, so it fails if either side's idea of an admissible window moves:
it returns 77 against the old code.

The HTTPS spot provider has since been deleted outright rather than fixed. It
was the second of two parallel spot subsystems, and the owner's question --
"what is the purpose of having TWO different cluster settings?" -- had no good
answer: it accepted a document shape no public service emits, so it never had
an address that would work, and its two source checkboxes were meaningless once
a node is joined, because what kind of report a spot is arrives with the spot.
Closing INT-003 by deletion. A per-source HTTPS adapter for DXHeat or POTA, the
latter carrying RBN-relayed CW spots with SNR and speed, remains worth having
and would be new work rather than a repair.

That split also caused a fault the owner reported separately: the spectrum
overlay was gated on the web feed's master switch rather than on whether any
source was live, so joining a cluster drew nothing at all.

Probing the public spot feeds established that the HTTPS provider cannot parse
any of them, because it defines a document shape rather than reading what the
services send; and that the cluster network is telnet, so live RBN is out of
reach of an HTTPS client entirely. Recorded as INT-003 and INT-004.

INT-004 was then built in four parallel lanes against contract headers fixed
first, and every server and every command was verified against the live network
rather than against documentation. DXSpider accepts `accept/spots 0 on 20m/cw`
and echoes it back through `sh/filter`; CC Cluster's own login banner lists its
whole command set and has no band filter in it, so those nodes carry none and
are filtered on arrival like every other server. A naive client that writes its
callsign on connect has it swallowed by VE7CC's banner, which is why the
callsign is sent only in answer to a recognised prompt.

The lanes caught each other's defects, which is the point of running them apart.
The test lane found two the implementation lane had not: `DX de :` parsed to a
spot with an empty spotter -- corroboration attributed to nobody -- and a bare
carriage return inside a login command in the operator-editable server list
survived `trimmed()` and would have reached a stranger's socket as a second
command line. Reconciliation found a third: the HTTPS provider and the telnet
client had each defined `kMinimumSpotFrequencyHz` and `kMaximumSpotFrequencyHz`
with the same names and different values, so the same report was accepted by one
path and refused by the other. One definition now lives in the shared header.

Previous review: 2026-09-11 (fifty-fifth entry) -- external spot evidence is
delivered, and the documentation was reviewed against the source rather than
against itself.

`CALL-007` was resumed on the owner's instruction and built in five parallel
lanes against a contract header fixed first, so none blocked another: a bounded
expiring registry in the dependency-free core, a receive-only HTTPS provider,
capped corroboration inside the existing provider budget, an operator settings
section, and the separator overlay with its square-before and circle-after
source marks. Two lanes independently caught a Clang compile error in that
contract header -- a default argument needing a nested type's own initializers
while the enclosing class is incomplete, which GCC and MSVC accept -- before it
could reach the build.

The property that mattered held. Corroboration matches on exact string equality
rather than the bounded edit distance, because matching on edit distance would
let an external report rename a decode. A spotted callsign no hypothesis
produced never appears, a spot cannot lift a candidate below the acoustic floor,
and both that and the registry were mutation-tested rather than merely asserted.

The documentation review found the tree and the documents had drifted
substantially apart in both directions: capabilities that ship described as
planned, and capabilities that do not exist described as present, including a
trained likelihood model with no artifact or training code anywhere. The worst
of it was mine and recent -- the transmit alphabet was still documented as
unable to send a distress prosign after that decision was reversed and
implemented, in the manual and in the shipped dictionary comment alike.

Outstanding from the settings lane, and not yet done: receiver enumeration runs
synchronously on the interface thread, so the application looks frozen while it
scans and no waiting indicator can animate, because the event loop is stopped
for the duration. It needs a real running-state property and the scan moved off
that thread; `refreshSelectedSdrCapabilities` has the same fault.

Previous review: 2026-09-11 (fifty-fourth entry) -- a VFO move could lose an
identified stream, and the way it was found is the point.

The channel bank carries every tracked signal by the amount the dial moved, and
that is tested directly: the track keeps its identity, verification state and
decoded text, and survives continued processing at the new frequency. All of it
passed while the application lost streams in the field, because nothing tested
the other half -- that the controller actually asks for the shift.

Written as a controller-level test, the defect appeared immediately. The request
was guarded on the radio having been readable in the previous report as well as
this one, so a momentary gap in availability, which a polled radio produces
while it is busy retuning, swallowed the move that followed it. The tracks stayed
where they were while the signal moved away, and the stream was lost and
re-acquired as a new one. The guard now asks only that a previous frequency is
known and that the dial has moved.

That is the second time in this session the core behaviour was tested and
correct while the path that reaches it was not. A feature is only covered when
the test enters where the operator does.

Previous review: 2026-09-11 (fifty-third entry) -- the waterfall slides with the
receiver instead of being erased, and the standing lesson of this session is
recorded with it.

Clearing the waterfall on any axis change was introduced with the zoom fix and
is wrong for the case it fires on most: retuning a receiver whose frames carry
absolute radio frequency moves the bounds at every step, so the display was
wiped at each click of the dial, while the same action on audio -- whose axis
does not move with tuning -- left it alone. A row is a history of frequency and
stays true when the receiver moves; it is slid by the number of bins the band
moved. Only a span change, where the bins stop meaning the same width, still
drops it. The regression test was verified by making it fail against the old
behaviour before the fix was trusted.

The pattern this session is worth stating plainly, because it has now cost the
owner repeatedly: spectrum zoom reset on every frame, the waterfall erased on
every retune, a decode gate that suppressed acquisition, and a reported loss of
decoded streams on a VFO move. Each was a working feature with no behavioural
test, so the suite stayed green while the application regressed, and each was
found in the field rather than here. Areas still without that cover are
spectrum and waterfall presentation, source switching, VFO retune with track
identity, and monitor selection. A feature without a test is a feature that
will be broken by the next fix.

Previous review: 2026-09-11 (fifty-second entry) -- the exact envelope densities
are settled in the negative, and the reason the earlier entries gave for trying
them again was wrong.

The evidence chain is not miscalibrated. Working it through: evidence is
midpoint plus scale times the bounded ratio, and probability is a sigmoid of
evidence minus midpoint over the same scale, so the two cancel and the chain
reduces to the sigmoid of the ratio clamped to the evidence bound. That is
exactly the Bayesian posterior for a log-likelihood ratio in nats. The fortieth
entry's claim that a statistic in true nats cannot be dropped in is therefore
false; the only free parameter is the bound.

What actually defeated the Rician and Rayleigh densities is that a per-sample
ratio assumes independent samples, while the detector runs at the sample rate
behind a filter a few hundred hertz wide. Scaling the ratio for that and
sweeping the factor gives: 0.3843 at 0.6, 0.3735 at 0.3, 0.4282 at 1.0, 0.4352
at 3.0, 0.4946 at 0.1, and 0.9198 at 0.01, against 0.3356 for the fitted
Gaussian. Worse at every scale, with no interior optimum. Reverted.

The conclusion is worth stating so a fourth attempt is not made. The levels the
decoder tracks are not the true Rician and Rayleigh parameters; they are
smoothed estimates of an envelope that has already been filtered, and the
two-level Gaussian with per-level scatter fits that observation better than the
idealised single-sample densities fit it. Improving the keying evidence means
changing what is measured, not which density is assumed of it.

Previous review: 2026-09-11 (fifty-first entry) -- fragment runs are suppressed
and `PERF-002`'s ceiling is cleared, both measured rather than argued.

Fragmentation is regional, not per-track. The obvious reading of the previous
entry -- that a track emitting runs of short characters is not CW -- does not
survive measurement: across the corpus, tracks that recovered a correct
callsign reach runs of ten themselves, because the same track carries good copy
and fragment sections in turn. Suppression therefore removes the run and keeps
what surrounds it. Six characters is the shortest safe threshold, real copy
reaching four and five, and the run becomes a space rather than being deleted.
On the reported capture the longest surviving run fell from nine to five with
corpus recovery unchanged.

The performance work reached the same lesson by a worse route. Skipping the
per-track filter chain for tracks that will not be decoded is correct, but the
first attempt judged a track by the spectrum's level estimate against a
threshold calibrated for the keying envelope's -- two different scales -- and
skipped tracks that decode perfectly well, costing two and a half points of
character error and failing a keying-edge test. Giving every track a fixed
warm-up during which it is always filtered, and judging it afterwards on its
own measured level, removes the circularity without the miscalibration.

Measured at twenty-four tracked signals, which is `maximum_tracks`: the sample
stage falls from 21.0 s to 13.6 s per twenty seconds of audio, so the real-time
factor goes from 1.09 to 0.71 and the pipeline is no longer behind the radio.
Mean character error on the synthetic surface moved from 0.3534 to 0.3356 and
callsign recovery held at 8 of 9. The larger prize the survey described --
taking the localisation ratio and the noise reference from the transform the
spectrum stage already computes, leaving one cascade per track instead of five
-- is untouched and still worth roughly an order of magnitude.

Previous review: 2026-09-11 (fiftieth entry) -- a field report of poor decoding
produced the first measurement of where decode quality actually goes, and it is
not where the report suggested.

The spectrum floor was a defect of my own making: the palette bottom introduced
at 0.1.151 is also the trace's baseline and the labelled axis, so the noise lay
flat on the bottom of the plot. The two wants are opposite -- a palette should
start just under the noise, a trace needs room beneath it -- and they are
separate numbers now.

Weak-signal decoding is an operator setting, off by default, gating at twelve
decibels. Two things were learned building it. Gating on a track's present
level suppresses it during acquisition and cost two of eight recovered
callsigns, including one settling at thirty-five decibels, so the gate reads
the strongest level a track has reached. And suspending a weak track is wrong:
suspension belongs to the association-loss path and is only resumed from there,
so a track suspended for being quiet would never decode again once it grew
loud. It is simply not fed instead.

The gate is honest but weak as a filter for what the operator actually
complained about. On the reported capture it removed one of eight text-emitting
tracks: level does not separate copy from fragments, because the corpus shows
fragmenting tracks at twenty-six decibels and a cleanly recovered callsign at
fifteen.

The real finding is what the fragments are. Every poor track emits runs of E, I,
S, T and H -- the shortest characters in the code -- and so does the verified
track, between good copies of its callsign. A track carrying `CQ POTA DE
SN5WLF` reads it correctly several times over and fills the gaps with
single-element runs. That is the two-level keying envelope admitting noise
during quiet periods, which
[[cw-decoder-bottleneck-is-upstream]] already names as the limit. Suppressing
fragment runs, and separating them from real copy, is the next slice and it
needs the same treatment as everything else here: measured against the corpus
before it is believed.

Previous review: 2026-09-11 (forty-ninth entry) -- the Windows build was broken
by the previous change and neither of the two releases since was green. The
regression test added with the built-in alphabet clears the dictionary
directory from the environment, and did so with `unsetenv` and `setenv`, which
are POSIX and absent under MSVC. Linux and both macOS jobs compiled it happily,
so nothing local caught it: the core tests link no Qt, so `qputenv` is not
available either, and the fix is a small guarded helper around `_putenv_s`.

Two things worth recording. Nothing here can compile for MSVC, so a
platform-only construct is invisible until the Windows job runs, and the habit
of pushing without then reading that job is what let two releases go out red.
And the generated built-in alphabet embedded the absolute path of the machine
that produced it in a comment; it names only the file now.

The earlier theory that a per-file resource alias failed on Windows does not
survive this either. The operator's data directory contains correctly seeded
dictionary files with the right contents, which means the bundled resources
were found and read correctly on that platform all along. The change to a
single base path remains worth keeping as the more robust form, but it was not
the fault.

Previous review: 2026-09-11 (forty-eighth entry) -- the field report is not
explained yet, and the previous entry's diagnosis was wrong. The operator's
Windows dictionaries were produced and inspected: all four files are present and
parse exactly as the repository copies do, 56 symbols and 82 tokens, nothing
rejected. So the alphabet was not missing and the fix in the previous entry,
though worth having, did not address the reported fault.

What is established. The receiver, the audio and the core decoder are all
sound: the reported capture replays against a build of the exact released
commit, using the operator's own dictionary files, and recovers the callsign,
the sender and the speed at eight seconds. The application meanwhile produced
no tracks at all for sixty seconds, and the slice the operator opened by hand
sat unmatched for a further sixty-seven with forty decibels of signal and no
spectral observations. Detection received nothing.

What has been excluded: the dictionaries and the alphabet, since the capture
decodes with the vocabulary files emptied; the spectrum analyzer change, which
is additive and leaves the window sum and the bins untouched; the speed-anchor
refactor, which resolves to the same index; the analyzer band, which automatic
bandwidth pins to 100-3000 Hz around a signal at 892; transform size; and the
settings and reset paths, which are unchanged since the last working release
and guard against repetition. The only channel-bank change in the range affects
verification rather than detection.

The instruments cannot reach it. The core harness decodes the capture and the
end-to-end worker test passes, so neither reproduces what the operator sees.
Two things follow. The application must not depend on the data directory being
correct -- the bundled copies are authoritative now and an operator copy is
used only if it parses -- and the capture must record what the detector was
given, which it now does: analyzer configuration, frames actually delivered to
the detector, decoder resets, and whether the alphabet came from a file or the
built-in copy. The next capture should identify this rather than narrow it.

Previous review: 2026-09-11 (forty-seventh entry) -- a released build decoded
nothing, reported from the field against 0.1.155. The receiver was fine: the
capture replays here and recovers its callsign, sender and speed. The decoder
had no alphabet.

Making the alphabet data put the decoder's core function behind a file load
that no test covered. Every test sets `CWA_DICTIONARY_DIR`; the application's
own path -- bundled resource, seeded into the operator's directory, read back
-- was exercised by nothing. The failure is also silent by construction: with
an empty alphabet every element pattern decodes to nothing, so tracks are
acquired and never verified, and the interface shows a strong signal with no
text and no speed, which is what was reported.

Three changes. The shipped alphabet is compiled in as a last resort, generated
from the same file at build time so it stays one alphabet rather than a second
table. An empty operator copy is repaired rather than preferred, which is what
made a single failed read permanent. And the bundled files are addressed with
one base path instead of a per-file resource alias on absolute paths outside
the target's directory, which is the most likely reason the read failed on the
packaged build while working here.

Verified by replaying the reported capture with no dictionary present at all:
before, no output whatsoever; after, the same callsign and confidence as with
the files in place.

The lesson is narrower than "do not move data out of code". It is that moving
something into data makes the loading path part of the function, and the
loading path the application actually uses must then be tested -- not a
substitute for it arranged by the test harness.

Previous review: 2026-09-11 (forty-sixth entry) -- `PERF-002` has an instrument,
and the first measurement is decisive. `cwa_receive_profile` reports wall time
for the spectrum and sample stages against tracked signal count, on twenty
seconds of audio per case:

| tracks | spectrum | samples | real-time factor |
| --- | --- | --- | --- |
| 1 | 0.71 s | 0.80 s | 0.076 |
| 8 | 0.73 s | 6.68 s | 0.370 |
| 16 | 0.74 s | 12.95 s | 0.684 |
| 24 | 0.74 s | 21.00 s | 1.087 |

The spectrum stage is flat: building it costs the same for one signal as for
twenty-four. The sample stage is linear at roughly 0.88 s per track and crosses
real time at twenty-four, which is exactly `maximum_tracks`. So the pipeline's
ceiling is the number of signals tracked, not the bandwidth watched, and the
configured maximum sits on the wrong side of it.

Reading the per-track chain against that: each track mixes with three
oscillators and runs five three-stage complex cascades per sample -- the three
centre widths plus a lower and an upper noise reference. Only the selected
width feeds keying evidence. The other two centre widths exist solely for the
centre-localisation ratio, and the lower and upper pair solely for the noise
reference; width selection itself is made from measured WPM and not from
comparative filter power. Four of the five cascades therefore produce summary
figures that the transform the spectrum stage already computes could supply,
and for every track except the monitored one the complex result is consumed
only by `std::norm`. That is the optimisation, and it is now justified by
measurement rather than by inspection. It changes measured quantities the
decoder acts on, so it must be validated against the surface benchmark, the
capture corpus and the spacing benchmark before it is accepted.

Previous review: 2026-09-11 (forty-fifth entry) -- `REQ-001` is answered and
closed, which clears the only blocked item in the backlog. The owner fixed the
speed range at 8 to 60 WPM, which is what the anchors and every quoted accuracy
figure already assume; put international characters out of scope so the ASCII
set becomes a stated claim rather than an omission; and kept break-in at semi
for the initial scope.

Prosigns needed real work rather than a statement. The transmit encoder is
keyed by character and could not represent a multi-character symbol at all, so
seven are now encoded as single symbols with no internal character gap, which
is the entire difference between `<AR>` and `A R`. The owner overruled the
proposal to exclude a distress call, and was right to: it is legal and
appropriate to send one, and unintentional transmission is already prevented by
arming, exact callsign confirmation, preview, explicit send, and a decoder that
can never initiate a transmission.

One thing the tests caught. Admitting the brackets in the guard and leaving the
prosign decision to the encoder made the guard accept `CQ <SCRIPT>`, which an
existing test rejects by design. The guard now judges the whole bracketed token
against the encoder's closed table, so an unknown one is refused during
normalization and never reaches a staged message.

Previous review: 2026-09-11 (forty-fourth entry) -- the retained-observation
route for the unfinished final over was tried and does not work, which settles
what the fix has to be.

`retained_observations_` does hold a snapshot that outlives its track, so it
looked like the decoupled record the previous entry asked for. It is not: those
observations are refreshed from live tracks during the snapshot rebuild on the
sample path, and both consumers -- the desktop worker and the replay tool --
read that path and discard what the spectrum path returns. A track flushed and
removed during the spectrum update is therefore already gone when the rebuild
that consumers read happens, and its completed over goes with it. Measured: the
corpus stayed at 268 of 268 with the flush writing into the retained
observation, and reached 269 only when the track was additionally held alive
into the next cycle.

Holding it alive is what cannot be had. Replacement identity matching requires
a predecessor to be absent before a successor can inherit its colour and text,
so the two requirements are in direct opposition: publishing a final over needs
the track present at the sample-path rebuild, and replacement inheritance needs
it gone. Every attempt that recovered the over broke
`genuine replacement inherits its predecessor text exactly once` and
`refreshing a replacement cannot append its inherited prefix again`.

So the conclusion of the previous entry stands and is now evidenced rather than
argued: completed overs need an emission path of their own, independent of
whether the producing track is still in the bank. That is an API addition and
should be scoped as one. Splitting removal so only a track that finished an
over lingers, and gating the flush on `ever_verified` so noise tracks cannot
manufacture one, are both necessary and both already proven; neither is
sufficient without the separate path.

Previous review: 2026-09-11 (forty-third entry) -- the unfinished-final-over
defect is confirmed, measured, and the obvious fix rejected on the measurement.

Confirmed: `CwMultiSpeedDecoder::flush` is called from tests only. Production
completes a turn exclusively through `resumeInput`, which requires the same
track to receive signal again after a sustained silence, and `CwChannelBank`
erases expiring tracks without flushing them. A station that finishes an over
and does not return to that frequency therefore leaves its last one open: never
completed, never context-rescored, never among the track's transmissions.

Flushing an expiring track recovers exactly one over across the twenty-two
captures, 269 against 268. Two things were learned getting there. Flushing every
expiring track manufactures overs out of noise -- the corpus produced turns
reading "M", "IEE" and "E E" from tracks that never carried a signal -- so any
such flush must be restricted to tracks that were verified. And the completed
over does not reach a consumer without also holding the track for an extra
cycle, because `updateSpectrum` erases it before `processSamples` rebuilds the
snapshots that consumers actually read.

That deferral is where the fix became disproportionate. Holding an expiring
verified track one cycle longer broke three existing guarantees: replacement
text inheritance, replacement refresh, and manual-probe expiry all depend on
the current lifetime. Reverted: one recovered over in two hundred and sixty
eight does not justify perturbing track lifetime, and the corpus cannot show
whether live operation would benefit more.

The correct shape, if this is taken up again, is to stop coupling a completed
over to the track still existing. A completed over is a record about a
transmission, not about a tracker; emitting it on its own path would let an
expiring track be flushed and removed in the same cycle, with no lifetime
change and none of the three breakages. That is an API addition rather than a
repair, and should be scoped as one.

Previous review: 2026-09-11 (forty-second entry) -- the outstanding cleanups are
done. Cut numbers carried only `T` and `N`, so a serial sent as `ANU` read as
letters instead of 123; the shipped contest files now declare the set an
operator sends at speed, which was a data edit rather than a rebuild now that
contests are files. The nine speed anchors existed twice, in the multi-speed and
probabilistic decoders, and the starting anchor was a bare index into one of
them; both read one definition and the starting speed is named. The contest
contraction of a signal report is written once.

The crash recorded two entries ago is fixed and the fix is verified rather than
assumed: with no dictionaries the suite exited 139 before and exits 1 now, with
twenty-four named failures instead of a signal. The pattern was reading front()
of a container immediately after the expectation that catches it being empty,
so the crash replaced the diagnosis.

Remaining, and none of it is tidying. A turn becomes semantic only once the
decoder observes the next transmission begin, so the last thing a station sends
never reaches the context rescorer -- usually its callsign. The rescorer runs
only in `completeTransmission` and so never shapes the running text an operator
watches. `SpectrumSnapshot::noise_bandwidth_hz` is published and nothing
consumes it. The per-track front end still builds a complex baseband at the full
rate and discards it for all but the monitored track, and all nine hypotheses
continue after lock (`PERF-002`). Recalibrating the evidence chain, which the
fortieth entry establishes as the prerequisite for any likelihood change,
remains deferred.

Previous review: 2026-09-11 (forty-first entry) -- contest exchanges are data.
CQ WW, CQ WPX, ARRL Field Day and November Sweepstakes moved out of
`conversation_profile.cpp` into `dictionaries/contests/`, one file each, with
the format documented beside them. The parser feeds the existing
`contestProfile` builder, which is what keeps the safety boundary intact: a file
supplies only the exchange, while flows, states, macros and every transmit gate
are still constructed in the application and cannot be named in a file.

The accessors became library lookups, and a contest that failed to load returns
an empty profile rather than a plausible one, so validation fails loudly instead
of presenting an operator an exchange that is quietly wrong. A malformed file is
refused whole; one bad contest does not remove the others.

Two things worth noting. Moving the profiles out left `cqZones` unused in
`conversation_profile.cpp` while the parser had grown its own copy, so the dead
one was removed rather than left to become the next silent divergence. And the
format documentation living as `README.txt` inside the directory the loader
globs made it parse as a contest; it is `README.md` now, which keeps it beside
the files it describes without a filename special case in the loader.

Still compiled in, deliberately: `neutral_monitoring_profile` and
`ordinary_cw_profile`. They are the structural fallback rather than published
rules that change, and making them data would mean an installation with no
dictionary has no conversation model at all.

Previous review: 2026-09-11 (fortieth entry) -- two attempts at the per-frame
keying likelihood, both measured on the full surface against 0.2579 with 150
correct and 33 wrong callsigns, both reverted.

The first was the missing normalizing term. The Gaussian ratio in
`cw_channel_bank.cpp` computes only the half-difference of squared standardized
offsets; with unequal scatter the true ratio also carries
log(sigma_space/sigma_mark), and because the pair is ordered so a mark never
scatters less than a space that omitted term is always negative. Restoring it
made every seed set worse -- 0.2604/0.2163/0.2971 became 0.2857/0.2326/0.3254,
overall 0.2579 to 0.2812, callsigns 150/33 to 146/34. Consistent in sign across
all three sets, so it is a result and not spread. The incomplete formula is
compensating for the two-level Gaussian being a fitted approximation rather than
the real densities; correcting one term of a wrong model is not an improvement.

The second replaced the model with the exact envelope densities: Rayleigh under
noise alone, Rician under tone plus noise, whose ratio reduces to
log I0(r*nu/sigma^2) - nu^2/(2*sigma^2) with both parameters available from the
levels already tracked. It measured 0.3175 with 136/46. That number should not
be believed, because the experiment was invalid: at 20 dB the statistic reaches
about 96 nats for a mark and -82 for a space, and `cwEvidenceBoundNats` clamps
at 3. Every sample saturated, so the run measured a hard slicer -- the exact
failure the surrounding comments describe as fragmenting weak elements -- rather
than the model.

That invalidity is the finding worth keeping. The evidence chain is calibrated
to the heuristic's scale and not to nats: bound 3, midpoint 4.5 dB, scale 1.5,
against key-on 6 dB and key-off 3 dB. A statistic in true nats cannot be dropped
into it, so replacing the likelihood means recalibrating the chain -- bound,
scale, and both thresholds -- as one piece of work, and the research direction
that recommends the exact densities is not the small slice it appeared to be.
Prerequisite for any further attempt.

Previous review: 2026-09-11 (thirty-ninth entry) -- the remaining duplicate
vocabularies are resolved, and one of the three was not a duplicate. Callsign
attribution's glued-prosign list and the context rescorer's word-gap prefixes
were the same idea maintained twice, at four tokens against six, and both now
read `dictionaries/cw-word-gap-prefixes.txt`. Track verification's distinctive
tokens moved to their own file rather than the shared vocabulary, because that
gate accepts a single match as proof of real CW and only works while a match is
harder to counterfeit than the checks it replaces; the test guards that
property -- `CQ` and `599` admitted, `K`, `R`, `ES` and `QSO` refused -- rather
than the contents.

The third site the audit flagged, the prosigns inlined through
`callsign_policy.cpp`, is not a vocabulary at all. Those are positional rules:
`CQ` before `DE` means self-identification, `K`/`KN`/`AR`/`SK` after a call
means closing and carries its own weight. Membership lookup cannot express that
and replacing them would have destroyed the grammar. Left alone deliberately.

Both shared dictionaries now recover from `CWA_DICTIONARY_DIR` when a caller
never loaded them, the vocabulary's fallback wrapped so a noexcept verification
path cannot terminate on an unreadable file. Corpus recovery unchanged at 8 of
9, suite 46 of 46.

Previous review: 2026-09-10 (thirty-eighth entry) -- the Morse alphabet is one
copy and it is data. It was two byte-identical tables in `cw_decoder.cpp` and
`cw_event_lattice.cpp`; both now read `dictionaries/morse-alphabet.txt`, which
is bundled and extensible for accented characters and unnamed prosigns. Because
an empty alphabet decodes nothing at all, unlike an empty vocabulary, the shared
instance falls back to `CWA_DICTIONARY_DIR` and every test directory sets it in
one loop rather than each target remembering.

The audit's claim that the transmit table had drifted does not survive contact
with it. `<SOS>` is absent from transmit deliberately -- a distress call must be
readable without being sendable -- and `<SK>` cannot exist in a `char` keyed
table at all, so the two are different shapes rather than the same table drifted
apart. The transmit punctuation set and `transmit_guard.cpp` already agree
exactly. The transmit alphabet therefore stays in the application: a transmit
constraint must not live in a file an operator can edit.

One pre-existing fragility surfaced. With no dictionary the suite reports ten
clear failures and then crashes, because `core_tests.cpp` dereferences
`front()` on an empty container immediately after the expectation that catches
the failure; `core_tests.cpp:1005` is the first instance. The library
itself returns an empty symbol and the decoder an unknown character, both
correctly. Worth hardening the test's post-failure dereferences.

Previous review: 2026-09-10 (thirty-seventh entry) -- spacing is measurable. The
previous entry recorded that no instrument could see the context vocabulary at
all; `cwa_spacing_benchmark` now scores word-boundary placement directly, on
scenes whose character and word gaps are pushed together until a threshold
cannot separate them. Paired on identical samples, the vocabulary lifts boundary
recall from 0.429 to 0.582 at the hard setting and from 0.958 to 1.000 at the
milder one, while asserting exactly as many spurious boundaries as the empty
vocabulary does at both. That last figure is the one that matters, and it is now
a gate: the benchmark fails if the vocabulary stops earning boundaries, starts
inventing them, or changes a character.

Two findings came out of building it. The context rescorer runs only in
`completeTransmission`, so it shapes a completed turn and never the running
text an operator watches live -- worth deciding whether that is intended. And a
turn becomes semantic only once the decoder observes the next transmission
begin, so a scene that ends in silence never closes its last turn and never
reaches the rescorer at all; the benchmark carries a short second transmission
for that reason. The corpus and surface figures are unchanged, as expected,
since neither reaches this path.

Previous review: 2026-09-10 (thirty-sixth entry) -- the CW exchange vocabulary is
data. It was thirteen tokens in a `constexpr` array; it is now eighty-two in
`dictionaries/`, loaded at startup, seeded into the operator's data directory so
it can be edited and survive an upgrade. The `PSEK` special case is gone, having
been replaced by decomposition against the vocabulary itself. Matching is
weighted by token length, without which a vocabulary this size lets a chance
one-letter match outvote a Q-code -- the first attempt at that weighting scaled
by internal gaps pinned, which demoted `CQ` and `DE`, and the existing tests
caught it on a case that was winning by 0.02.

The measurement gap is the finding worth acting on. The synthetic surface is
byte-identical across all three seed sets before and after, because it never
exercises the context rescorer at all; the capture corpus is unchanged at 8 of 9
because it scores callsign recovery, not spacing. Neither instrument can see
this class of change, and spacing is where the literature and the receiver
captures both say the remaining error lives. A spacing metric is the next slice
and gates the rest of the vocabulary work.

An audit of compiled-in domain data found the Morse alphabet in three places --
`cw_event_lattice.cpp`, `cw_decoder.cpp`, and `cw_transmit_encoder.cpp` -- with
the transmit copy already carrying a different character set from the two
receive copies, plus a fourth copy of the punctuation set in `transmit_guard.cpp`
that must agree with the encoder or a legal character is refused without saying
so. Whether the transmit set is deliberately narrower is the question to settle
first. Three further vocabularies duplicate what `CwVocabulary` now serves
(`callsign_policy.cpp` glued prefixes and its inlined prosigns,
`cw_channel_bank.cpp` verification tokens), and the contest profiles remain
compiled in although their schema already carries the revision and validity
fields external data would need.

Previous review: 2026-09-10 (thirty-fifth entry) -- zoom on the wide SDR display
was released broken and is fixed. Preserving a zoom across a retune was gated on
the receiver's bounds having moved, and the same condition was then used to
decide whether to rebuild the view at all, so every ordinary frame took the
rebuild path and reset the span. The display could be zoomed but not kept there.
Only a source change may move the view now.

The gap that let this ship is the more useful finding: `SpectrumWaterfallItem`
had no behavioural coverage at all. The one test naming zoom asserts that a
string appears in `Main.qml`, which a broken implementation satisfies. The item
is now exercised directly in the offscreen desktop render test -- accept a
frame, zoom, accept further frames on unchanged bounds, require the span to
survive, then retune and require it to be carried. Verified to fail on the
released code with a distinct exit status and pass on the fix.

Previous review: 2026-09-10 (thirty-fourth entry) -- the wide SDR display was
showing the measurement rather than the signal. All 16384 overview bins were
being drawn into around 1500 pixels with no reduction, so the trace rendered
each column as the full spread of its bins and the waterfall texture was built
one pixel per bin and then minified by roughly eleven; the palette floor sat
about fourteen decibels below the real noise floor because it used the twentieth
percentile of an exponential distribution and then subtracted eight more; and
the local noise reference collapsed to six bins at wide spacings, adding speckle
instead of removing it. Reducing bins to display columns also cuts the
per-frame waterfall texture from roughly forty megabytes to three, which is
`PERF-002` territory reached from the quality side.

Zoom is now preserved when the SDR is retuned. SDR frames are described in
absolute radio frequency, so a retune moves the axis and any axis change was
treated as a new source; the audio path never showed this because its axis does
not move with tuning. Stale waterfall history is cleared on a frequency change,
which the previous bin-count check could not catch.

Interoperable IQ recording exists (`REC-001`, `SDR-001`). Recording refused to
run on an SDR source, and the writer behind it kept only the real component of
each complex sample, which discards the sideband distinction; its cap was
expressed in samples at an audio rate and would have been reached in about
twenty seconds at eight megasamples per second. Captures are now SigMF with a
metadata sidecar, bounded by bytes as well as duration, and carry the receiver
gain state plus per-block level and direct-current telemetry -- without which a
recording cannot answer whether the front end was over- or under-driven, since
the application still has no overload indicator (`SDR-001` remaining).

The selectable keying model is measurable for the first time. Every benchmark
left the model at its default, so the alternative offered in Settings had no
coverage at all. Measured on the full surface at this commit: mean character
error 0.2579 for the per-frame threshold against 0.2591 for the duration model,
level against a seed-set spread near 0.08, but callsign recovery 150 correct
against 136, for 33 and 31 wrong. The threshold remains the default on that
evidence, and the duration model's advantage stays confined to weighted and
Farnsworth fists on the keying-style bench.

Two counts corrected: the private capture corpus holds **22** recordings, not
the 24 quoted in recent entries, and all 22 replay cleanly.

Previous review: 2026-09-10 (thirty-third entry) -- direct-SDR operation now has
one physical-device selector plus a separate driver operating-mode selector,
so the RSPduo's ST/DT/MA/MA8 configurations no longer look like four receivers.
An operational SDR faceplate provides center-frequency stepping/editing,
capability-derived IQ rate, RF bandwidth and antenna/input controls, and
bidirectional provider-neutral Radio Sync. Center-only changes retune the open
RX stream without rebuilding the device/DSP pipeline; configuration changes
that alter the stream or hardware route retain the controlled restart. SDRplay
decimation remains selected through its supported effective IQ sample rates
rather than a fabricated independent control. Live hardware acceptance and
complete telemetry remain open under SDR-001 through SDR-003.

Previous review: 2026-09-10 (thirty-second entry) -- radio Settings and the
station wizard now project only fields consumed by the selected provider.
OmniRig owns its COM/framing setup, rigctld owns its physical-radio transport,
and CAT4OM owns its server-side radio connection. The retained direct-serial
schema constrains baud to conventional 1200--115200 choices and validates data
bits, parity, stop bits and flow control, ready for CAT-001's future in-process
direct-CAT implementation without exposing nonfunctional controls today.
Direct-SDR Shift+left-drag now selects decoder center/bandwidth in the plot,
closing the quick window-selection part of SDR-001 while hardware acceptance
and measured overload work remain. Manual selection is deferred across the SDR
channelizer reset until the first new-slice spectrum is ready, so moving the
window also starts the requested probe instead of producing only an overlay.

Previous review: 2026-09-09 (thirty-first entry) -- OPS-001 specifies four
first-level operating modes: Standard, PileUp Chaser, PileUp Slicer, and Runner.
Chaser requires an observation-only learning phase, protects another caller's
exchange from local TX, gives the runner full decoding priority, limits pileup
lanes to callsign/report evidence, and supports configurable half-duplex
relearning or continuous full-duplex learning. Any automatic VFO B placement
requires explicit per-session enablement and authoritative provider readback;
no mode gains PTT, KEY, arming, or message-send authority.

Previous review: 2026-09-09 (thirtieth entry) -- UI-007 is implemented with an
authoritative, sideband-aware TX-VFO slice and capability-gated Ctrl+click
frequency selection through the provider-neutral TX route. Direct SDR no
longer hides Radio Control, and its center editors use exact VFO-style kHz.
Decoder-card transcript padding is independent of the Qt style and TX/Monitor
now form a balanced lower action row. UI-008, CFG-003 and PERF-002 remain
proposals for later source-bearing waves.

Previous review: 2026-09-09 (twenty-ninth entry) -- PERF-002 records a strictly
measurement-gated live-pipeline optimization proposal: per-stage telemetry,
GPU waterfall row uploads, fewer DSP/UI allocations, decoder-first overload
control, and SIMD only where profiling justifies it. UI-007 records replacing
the fixed pitch guide with a provider-readback-driven TX slice that follows
physical-radio VFO/split/frequency changes and disappears on unknown or
off-screen state. UI-008 adds a capability-aware absolute-RF/audio-offset ruler
to the spectrum/waterfall separator. CFG-003 specifies a generic, validated
wizard for recalling complete independent RX/TX device topologies without
hard-coded equipment presets. No performance benefit, RF mapping, or TX
position may be guessed.

Previous review: 2026-09-09 (twenty-eighth entry) -- Wide SDR input now has two
explicit processing branches: the complete acquired passband feeds a bounded-
rate overview, while one shared tuned, anti-aliased and decimated window feeds
CW detection and every stream decoder. The selected receiver advertises its
standard sample-rate, RF-bandwidth, antenna/input and gain capabilities; the UI
also explains RSPduo operating modes. Spectrum wheel zoom, middle-drag pan and
an explicit full-span reset preserve the wide view. Optional provider-neutral RX-VFO follow
with a signed LO offset keeps the decoder window inside the acquired span.
Decoder-card commands now live in a stable lower action row and commit monitor
intent before live model refresh can replace a delegate. SDR-001 through
SDR-003, AUDIO-002, UI-003, UI-005, OBS-001 and PERF-001 remain active for the
hardware, telemetry, format-recovery and measured overload work stated below.
Live provisional characters now append without card rebuilds, while sustained
inter-transmission pauses create clean transcript lines and cadence-supported
word gaps remain conservative.

Previous review: 2026-09-09 (twenty-seventh entry) -- Opening the SDR settings
page now requests one deferred discovery scan so attached receivers are listed
without requiring the first manual refresh. Application and profile startup
remain probe-free, reception remains explicitly operator-started, and the
manual refresh action remains available for hot-plug/reconnect testing under
SDR-001 through SDR-003. Windows packaging now supplies the pinned
SoapySDRPlay3 bridge while keeping SDRplay's proprietary API external, and the
global audio toolbar no longer competes with decoder-card speaker selection.

Previous review: 2026-09-09 (twenty-sixth entry) -- Official packages now enable
the RX-only SoapySDR adapter and include or depend on a verified RTL-SDR runtime
closure. Application-relative discovery preserves external vendor-module paths,
and staged-package smoke tests load the RTL factory before publication. The
SDRplay API remains an operator-installed proprietary prerequisite. The first
hosted runs exposed and corrected a Windows shell parse error plus Ubuntu
24.04's `librtlsdr.so.2`/libcap closure requirements without relaxing package
validation. Windows also declares the supported legacy CMake policy floor for
the pinned upstream source; bounded package-step diagnostics are retained in CI
status tags.
Configure-stage CMake failures are retained through the same bounded channel.
The Windows build resolves and verifies the pinned package only from upstream's
platform-specific `<runtime>/cmake` directory, keeping hosted builds independent
of CMake's transient user registry.
SDR hardware enumeration is operator-triggered rather than blocking application
or profile startup; physical hot-plug and reconnect acceptance remains open.
The Windows build-tree UI smoke uses the same pinned runtime root later copied
and independently loaded from the staged installer layout.
Linux package acquisition uses bounded APT retries and retains failures in the
same CI status channel. It is scoped to the runner's signed Ubuntu source file,
so unrelated preinstalled repositories cannot affect SDR package validation.
The isolation uses an explicit empty source-parts directory rather than a
special path value or mirror-name assumption.
Physical RTL-SDR and SDRplay acceptance, IQ recording, and complete telemetry
remain active work.

Previous review: 2026-09-09 (twenty-fifth entry) -- Direct SDR reception work
adds an RX-only SoapySDR boundary, wide-IQ validation, and explicit RTL-SDR/
SDRplay validation paths. CALL-007 remains deferred, but its future optional
DX-cluster/RBN overlay is now specified at the spectrum/waterfall separator;
CALL-008 separately owns collision avoidance and decluttering. Stream-label
hover growth is reduced so magnification does not obscure adjacent signals.

Previous review: 2026-09-09 (twenty-fourth entry) -- Wave 2 keeps the latest
one-second/six-run lattice suffix provisional across brief association loss,
then commits append-only after later spacing evidence or finalizes at a real
turn boundary. Literal/refined callsign evidence is reconciled so a boundary
reconstruction alone cannot create or replace a label. A checksum-bound
receiver annotation workflow and disjoint generated receiver-path holdout now
measure the remaining gap honestly: CER 0.458, WER 0.786, exact-call precision
1.0, recall 0.333, and zero no-CW publications. A separated-state envelope
experiment was rejected after one disjoint profile regressed despite a small
aggregate gain. Reviewed legally reusable receiver annotations remain the next
required evidence. Per-platform diagnostic status publication now retries
bounded transient Git failures after all substantive checks pass; this does
not retry, skip, or weaken any compiler, test, package, smoke, or artifact gate.

Previous review: 2026-09-09 (twenty-third entry) -- Wave 1 adds an executable
CER/WER, exact-callsign, latency, and false-publication gate; removes nine
steady-state per-track hypothesis deep copies; and makes reported/per-sender
WPM resilient to normal hand-key weighting. The paired cadence estimate is
intentionally isolated from decoding and publication after that coupling was
measured to create extra wrong callsigns. Remaining quality claims still
require annotated receiver audio, and the outer presentation snapshot remains
the next copy target.

Previous review: 2026-09-09 (twenty-second entry) -- added the first
platform-neutral Hamlib rigctld provider with complete-poll authoritative
RX/TX frequency, mode, VFO, and split state; loopback-only transport; and no
PTT/KEY surface. A=B now sends exact whole hertz through that same provider
boundary. The manual keying acknowledgement is replaced by an actual bounded
RTS→CTS/DTR→DSR measurement tied to the exact configuration. TX/TUNE progress,
editable exchange/report fields, and four profile quick macros now retain the
same exact-confirmation gate. TUNE is orange beneath ON AIR; SPLIT and A=B are
centered. Remaining hardware work is actual per-platform fixture execution and
minimum-power dummy-load acceptance, not additional software trust flags.

Previous review: 2026-09-09 (twenty-first entry) -- connected the guarded
operator-confirmed transmit workflow to the direct serial adapter. Arming now
requires an explicit physical-loopback acknowledgement and authoritative TX
frequency, CW/CW-R target-mode, and split readback; captured state changes
disarm or emergency-release. Message cancellation and TUNE release
synchronously in KEY-before-PTT order, an independent worker deadline bounds
TUNE, and monotonic snapshot revisions prevent stale cross-thread hardware
state. The faceplate now aligns its frequency edges and fixed-size right mode
column, keeps ON AIR as a borderless status lamp, and puts TUNE beside SIMPLEX
and A=B. Remaining acceptance is physical loopback and dummy-load testing on
each platform.

Previous review: 2026-09-08 (twentieth entry) -- implemented bounded
transmission-turn segmentation at sustained-silence/end-of-input boundaries,
strong-evidence current-sender attribution, and separate retained cadence
summaries for explicitly identified senders. A prior cadence can only nudge an
already compatible live timing estimate. Completed-turn context may repair
word boundaries only among acoustically competitive paths with identical
decoded characters; it cannot rewrite raw or phase-consensus evidence. The
alternating-simplex regression now requires two correctly attributed turns at
distinct 18/30 WPM cadences, and an association-suspension regression prevents
a slow word gap from becoming a false turn. The VFO presentation and hover
guidance were also consolidated without adding provider TX/mode write support.

Previous review: 2026-09-08 (nineteenth entry) -- a clean ordinary-QSO capture
proved that one frequency observation can contain two alternating operators.
The completed `CALL1 DE CALL2` handover now retains and presents both
participants on one card. Per-transmission silence boundaries, current-sender
inference, and conversation-aware timing reset remain explicitly separate so
the application does not invent two frequency tracks or guess a speaker.

Previous review: 2026-09-08 (eighteenth entry) -- implemented bounded full-window
and selected-track audio monitoring plus the first hardware-inert guarded TX
workflow: free text/own call/report Morse plans, exact confirmation, advisory
Auto-QSO proposals, emergency release, and a tested 15-second TUNE watchdog.
Defined the next pileup operating slice: keep the runner at the 700 Hz guide,
show its receive-side pileup to the right, and rank operator-confirmed split-TX
slots only after authoritative CAT split state and selected-track monitoring.

Previous review: 2026-09-08 (seventeenth entry) -- the owner-supplied CW Buddy
mark now has a genuine transparent exterior around its rounded-square edge.
The alpha-aware master is propagated to Qt/Linux PNG, Windows ICO, macOS ICNS,
and README artwork. This local packaging polish is held for the next
source-bearing publication rather than spending a release on artwork alone.

Previous review: 2026-09-08 (sixteenth entry) -- the public product identity is
now CW Buddy across the UI, executables, installers, release assets, update
manifest, documentation, and repository links, with the owner-supplied mark as
the cross-platform application icon and README artwork. The established macOS
bundle identifier and Windows upgrade GUID remain stable so the rename is an
upgrade, not a second product; the Debian package replaces the previous package,
and first launch imports existing profiles and the managed SCP cache. Hosted
packaging remains the required cross-platform acceptance gate.

Previous review: 2026-09-08 (fifteenth entry) -- DSP-006's first bounded slice is
implemented. The responsive hard-assignment tracker remains for fast attack,
fading, and manual weighting, but a 512 ms allocation-free amplitude history
now anchors it to two robust modes only when both populations have support, at
least 6 dB power separation, and at least 70% explained variation. This is the
separation prior the rejected soft/EM experiment lacked. Paired on identical
cross-platform reproducible generated audio, mean full-surface character error
improves from 0.3070 to 0.2579, with every seed set improving; wrong callsign
assertions fall from 40 to 36. The receiver corpus holds at 8/9
corroborated calls and 0/4 false calls on no-CW recordings. Captures now record
the split evidence and whether the anchor was accepted. Full corpus replay rises
from 20.13 to 25.75 seconds, so eliminating per-frame deep decoder snapshots is
the next performance task before wider multi-track scaling. Remaining DSP-006
work is a fully probabilistic separated-state estimator with held-out annotated
receiver CER, not further threshold-only tuning.

Previous review: 2026-09-08 (fourteenth entry) -- the keying technique is now a
choice rather than a fixed part of the decoder, and a duration-explicit model is
offered beside the shipped threshold. Measured paired against the previous
build, the two are level on the synthetic surface (0.2946 against 0.2884, inside
the +/-0.03 spread that surface has) and equal on captures (8/9, 0/4 false), and
they differ sharply by keying style rather than in general, so neither replaces
the other. The default is unchanged and reproduces its previous figures exactly.

Two things this measurement settled and that should not be re-derived. First,
the duration model is four times better than the threshold in isolation on
matched evidence (0.061 against 0.236) yet only level end to end, which places
the remaining limit upstream, in the two-level envelope model that produces the
keying likelihood: it assigns each frame to whichever level it is nearer, and at
low SNR that is close to a coin toss that drags the levels together. Replacing
that with plain soft assignment is worse, not better -- 0.3884 against 0.2946,
collapsing outright at 12 WPM and 12 dB -- because near the middle both
responsibilities sit near a half and an ambiguous frame pulls the levels
together. Whatever replaces it needs a separation prior. Second, the calibrated
likelihood was clamped to three nats per frame, a bound a per-frame threshold
does not notice and an integrating model pays for; widening it for the latter
moved capture recovery from seven of nine to eight.

Previous review: 2026-09-07 (thirteenth entry) — tracks are verified as CW on
recordings that contain none: eight across the four such captures, one to three
each, published with text like "BL E EEHWE IH K I E". Nothing measured this. The
quality checks count false callsigns, so a track wrongly accepted as a signal was
invisible, which is the same blind spot callsign precision had.

Tightening the verification timing-quality threshold does not fix it and is
recorded so it is not retried: at 0.55 there are eight false tracks and eight of
nine callsigns recovered; at 0.70, still eight and seven; at 0.80, four and six;
at 0.88, one and four. Reaching one false track costs half the real stations,
because noise reaches the same timing quality as CW -- measured values overlap,
noise 0.62 to 0.80 against real 0.75 to 0.94. The threshold stays where it is.
What discriminates on structure rather than timing is CW-006's recognisable
pattern evidence, and that is the honest next attempt.

The same sweep found that the threshold had been left parameterised by a
build-time define since the keying-decision work earlier in the day, surviving
eight commits and every verification run: the sweep silently changed nothing,
and three identical rows were what exposed it. The define is gone and the
verification battery now fails on any stray build-time define or leftover
standard-error output anywhere in the shipped sources, because the previous
check listed macro names by hand and could only catch the ones already known.

Last reviewed: 2026-09-07 (twelfth entry) — the local model reported an error
for a model that was never configured. Enabling it without selecting files still
attempted a load, and an empty path fails the metadata check by the same branch
as a file of the wrong kind or past the size limit, so the card showed a size
complaint for a model the operator had not chosen. An enabled but unfinished
setup is now its own state, loads nothing and says what to select, and the panel
is hidden while the feature is unused.

Also measured and not adopted: choosing between the consensus and the literal
transcript when they disagree. Neither available signal discriminates. The
lattice's evidence confidence moves only from 0.830 to 0.821 across a seventeen
fold change in the consensus's own character error, and its acoustic cost per
symbol is anti-correlated at the extremes -- the best consensus measured, at
0.016 error, carried the highest cost per symbol at 0.41, while one at 0.391
error carried 0.18. A four point sample suggested cost tracked quality and an
eighteen point sample destroyed it. Any preference rule written on these signals
would be arbitrary, so none was written; calibrating the lattice's confidence so
that it means something is the real work, and it is its own piece.

Last reviewed: 2026-09-07 (eleventh entry) — CW-001's Farnsworth item is closed.
The event lattice scored gaps against fixed centres of one, three and seven
element lengths. Farnsworth sending holds element timing at the operator's speed
and stretches the character and word gaps by a common factor, so a stretched
character gap sat nearer the word-gap centre and was read as a word gap: the
consensus transcript split every character apart while the literal transcript
beside it was perfect, and the card prefers the consensus. The factor is now
recovered from the observed gaps, since ordinary text holds far more character
gaps than word gaps and the median gap clearly longer than an element gap is
therefore a character gap. Character and word centres scale together, element
timing is untouched, and nothing adapts without at least six confident gaps or
outside the range real sending occupies.

Measured on a controlled fixture with the word gap stretched proportionally, as
Farnsworth does: consensus character error falls from 0.562 to 0.031, 0.547 to
0.062 and 0.516 to 0.094 as spacing stretches, while the standard-spacing case
is unchanged. The paired surface benchmark and the keying-style figures are
byte-identical to baseline and captures hold at eight of nine with none asserted
on the four containing no CW. On receiver capture 20260907-175150 a callsign
that decoded as "R 7 K B ?" now reads "R7KBB"; the same capture also begins
asserting R7KBTI, joining its one corrupted repetition where previously nothing
was asserted, which is a real cost of joining what had been fragments.

Two things measured along the way and not adopted. The lattice's evidence
confidence does not track its own correctness -- 0.845 when its transcript was
0.016 character error and 0.847 when it was 0.594 -- so it cannot be used to
decide whether the consensus or the literal transcript should be shown. And a
fixed stretched character-gap centre improved only the operating point it was
placed at, leaving the gap between centres untouched, which is fitting a fixture
rather than modelling the sender.

Last reviewed: 2026-09-07 (tenth entry) — PKG-004's startup update notice now
shares the verified-download presentation contract used by Settings → About:
progress and checksum status remain visible, then **Open Installer** and the
platform reveal action replace **Download update** in place. CALL-001/CALL-005
also keep a missing word gap from turning `DE`, `CQ`, `TU`, or `QRZ` into part
of the selected callsign when the remainder is independently plausible, and
place the correction control beside the loaded-list state.

Last reviewed: 2026-09-07 (tenth entry) — receiver captures 20260907-125614 and
20260907-141450 were added to the local corpus and drove three fixes. A station
sending CQ CQ CQ DE SV7BIO was labelled DESV7BIO: a missing word gap merged the
prosign onto the callsign, and the merged token then collected the CQ context
credit while the real callsign, which stood alone twice in the same text, got
only repetition. Splitting the pair is now done at tokenisation and only where
the remainder is itself a plausible callsign, which leaves a genuine
DE-prefixed German call intact.

The same capture explained two display complaints. Signal level was reported
from an instantaneous reading, which on a keyed carrier is about +28 dB inside
a mark and below zero inside a gap; the recorded diagnostics for that station
swing between -17 and +30 dB frame to frame, and the -6.3 dB the operator saw
was simply a gap sample on a strong signal. The presented figure is now the
estimated mark level, +31 dB for that station. Confidence was reported from the
instantaneous value, which falls to zero between characters and read zero per
cent while text arrived; it now reports the character-averaged figure.

The wandering speed from those captures is addressed at the presentation. The
estimator is unchanged and still adapts freely, but a speed is withheld until
at least three symbols support one: before that the value is the seeded default
rather than a measurement, which is why a 27 WPM station read 20 WPM with
nothing decoded and 40 WPM on its fourth key transition. Six of fourteen
sampled readings across that recording were unsupported and are now withheld,
narrowing the displayed range from 20-28 WPM to 25-28. The independent cadence
estimate still spikes to 40.9 on thin evidence and is not yet gated. The transcript away from the callsign remains
poor at this signal strength, which is the retention limit already recorded
above rather than anything specific to this recording.

An adaptive gap-timing scheme clustering intra-element, character and word gaps
per segment was evaluated against the paired benchmark and not adopted: mean
character error was identical on all three seed sets, one additional wrong
callsign was asserted, and the refined transcript on 20260907-141450 was worse
than without it. The idea of learning spacing from the observed gap population
is sound and better founded than adapting to signal quality, which has no
usable signal; it needs to show a paired gain before it ships.

Last reviewed: 2026-09-07 (ninth entry) — callsign precision was never measured
and is poor. The quality checks counted callsigns recovered and false callsigns
on recordings containing no CW; a wrong station named on a live signal was
invisible. Measured against the corroborated receiver captures, six of nine
assert some other callsign at the true station's own frequency, usually a near
miss of it: EG1PDA also asserted as EG1PEIE, DB26YLBB as UR5BV, EA1EYL as A1E,
4X1MM as K1NE, EM90ZMV as T90ZTTV. The decoder surface benchmark now reports
the same thing on synthetic signals, where six assertions in thirty-three name a
station that was never sent.

It cannot be fixed by scoring, and two attempts to do so are recorded here so
they are not repeated. Raising the acceptance threshold from 3 to 5 removes
every wrong assertion but drops correct ones from six to four and, worse, kills
the exact-repetition path that identifies a pileup caller sending only its own
call -- which is a case the application must keep. Weakening the bare closing
prosign weight changes nothing at any value from 4 down to 1, because these
tokens are followed by PSE K, a separate and much stronger rule. The reason is
structural: the false tokens sit in positions where a callsign genuinely
belongs, so context scoring cannot separate a correctly placed wrong token from
a correctly placed right one. Only better decoding or external corroboration
can, which is what the offline-list badge and the near-miss correction provide.

Last reviewed: 2026-09-07 (eighth entry) — CW-001. Refining where element
boundaries are placed was proposed as the remaining weak-signal work and is
rejected on measurement, before any decoder code was written for it. The idea
was to keep the threshold for deciding that a transition happened and place the
boundary itself by likelihood, on the reasoning that one threshold crossing
cannot serve both mark retention and timing precision.

Placement is already accurate. Measured against known signals at 20 WPM, the
median boundary lands within 3 per cent of a dot of the truth at 20, 15 and 12
dB. Refining that would gain a percent or two of a dot, and the error surface
says a quarter of a dot of jitter costs only 0.045 character error, so there is
nothing there to win.

The tail is a different failure. The ninetieth percentile sits near half a dot
at every signal-to-noise ratio, and an edge that far out is a mark detected in
the wrong place or not at all rather than one mistimed: it is the retention
problem again, and placement refinement does not touch it. Retention remains
what the error surface says is expensive -- losing a tenth of the marks costs
0.455 against 0.125 for inventing a tenth -- and it cannot be bought with the
keying thresholds without lengthening every mark, which the cadence estimator
and the lattice's evidence confidence both detect. What is left is genuinely
structural: the lattice would have to hypothesise marks and gaps from the
evidence stream instead of being handed runs a threshold already extracted.
That is a large piece of work and should be scoped deliberately rather than
approached as a tuning change.

Last reviewed: 2026-09-07 (seventh entry) — the remaining unrecovered receiver
capture, 20260903-165900, was investigated and two proposed explanations were
disproved by measurement before anything was built on them.

It is not an acquisition transient. The theory was that opening characters are
always lost because element boundaries must be committed before a speed
estimate exists. A clean synthetic signal decodes the same callsign correctly
from its first repetition at 16, 20 and 30 WPM, so no such general defect
exists; the leading-token corruption seen in keying-style output is real but
does not generalise to a mechanism.

It is not track splitting either. Eleven of nineteen captures hold track pairs
within 5 Hz, some 0.1 Hz apart, which looked like one carrier held under two
identities. They are sequential rather than simultaneous: at 1015 Hz in capture
20260902-132323 the acquisitions are 110, 128, 156 and 232 seconds apart. That
is a station transmitting intermittently with tracks expiring between overs,
which is the documented identity lifecycle.

What is actually wrong is narrower. The callsign decodes as "EM ?0ZMV": a
spurious gap splits it in two, and the character distinguishing the station is
unknown. Joining the fragments would not settle it -- a wildcard lookup for
EM?0ZMV matches EM80ZMV and EM90ZMV at distance zero, and the new directory
correction refuses an ambiguous neighbourhood by design rather than guessing
between two real stations. Recovery therefore depends on the operator's own
list: unique there, a span that bridges one spurious gap would recover it;
holding both, the decode genuinely does not determine which station sent.
Bridging a single gap when forming the callsign span is worth trying for that
reason, and must stay behind the existing correction setting.

Last reviewed: 2026-09-07 (sixth entry) — the offline callsign list's fuzzy
lookup was implemented and covered by tests but never called by the application;
its only use was an exact-membership check that labelled a suggestion's source.
It now backs an opt-in correction of near-miss callsigns, which is the directory
half of the acquisition-transient problem recorded in the fifth entry: capture
20260903-165900 decodes EM90ZMV as T90ZMV, losing only the opening characters,
and a bounded edit-distance lookup recovers exactly that. Correction stays off
by default and cannot promote a candidate where more than one entry is equally
close, because two listed stations can differ by one character. The acoustic
half is still open: boundaries are committed before any speed estimate exists,
and the same signature appears across the corpus.

Startup now reports pending application and callsign-list updates once per
launch. UI-005 covers it.

Last reviewed: 2026-09-07 (fifth entry) — CW-001. Two findings, one of which
changes how this decoder must be measured at all.

First, measurement. The character-error figure used to judge decoder changes is
averaged over five noise seeds, and its value moves by about 0.03 with the draw
alone -- larger than most differences that were being read as results. It is
deterministic per seed, so the variance is entirely across draws rather than
between runs, and a comparison of two builds on the same seeds cancels it
almost completely. Every decoder comparison must therefore be paired against
identical draws and reported as the paired difference. An absolute figure
compared against a remembered baseline from a different build means nothing at
this scale, and several conclusions reached that way were wrong.

Second, where the remaining error lives. Feeding the event lattice exact run
boundaries decodes a message perfectly at every speed, so the sequence decoder
is not a limit; segmentation is the whole of it. Degrading those boundaries the
way noise does shows the costs are strongly asymmetric: displacing every edge
by a quarter of a dot costs 0.045 character error, losing a tenth of the marks
costs 0.455, and inventing a tenth costs 0.125. Losing a mark is roughly three
and a half times worse than inventing one.

That asymmetry cannot be exploited by moving the keying thresholds, and the
attempt is recorded so it is not repeated: shifting the hysteresis band down
retains marks but lengthens every one of them, which the independent cadence
estimator and the lattice's evidence confidence both detect, and narrowing the
band restores timing while losing more copy than the original. One threshold
crossing has to serve both retention and timing and they pull opposite ways.
Making the decision adapt to signal quality instead is currently impossible:
the keying evidence is a calibrated posterior that saturates by design, and
neither the decoder's own confidence nor the detector's level separation nor
its mark level tracks the input signal-to-noise ratio -- all three were
measured and all three are flat or saturated across a 30 dB to 12 dB range. A
quality-adaptive rule needs a signal that does not exist yet.

Also rejected on measurement this session: weighting anchor selection by a
searched element length (recorded in the third entry), and an evidence-scaled
speed prior, which bought no copy on its own and broke the timing benchmark.

What ships from it is one constant. Gap classification now sits nearer the
nominal character gap, worth a paired 0.018, 0.039 and 0.013 across three
independent seed sets. The staging fixture that had to move for it was
asserting a two-and-a-half dot gap, neither an element gap nor a character gap
but between them, and now uses unambiguous spacing -- the same correction this
tree already applied once to the replacement fixture, and it passes under the
old threshold as well as the new one.

Still open under CW-001: copy below about 15 dB, Farnsworth spacing and 50 WPM,
and the acquisition transient -- capture 20260903-165900 decodes EM90ZMV as
T90ZMV, losing only the first two characters, because element boundaries must
be committed before any speed estimate exists. DSP-002 and UI-005 unchanged.

Last reviewed: 2026-09-07 (fourth entry) — UI-005: fixed the decoded transcript
shuddering as text arrived. The cause was that every update reassigned the whole
string, rebuilding the text document and resetting the viewport, so one frame in
every decoded character was drawn at a stale offset. Appending only the suffix
and pinning the tail in the same frame the content grows removes it; the local
model transcript in the same card shared the defect and is fixed with it. The
decoder work in this session is unchanged by it.

Last reviewed: 2026-09-07 (third entry) — tested and rejected the element-length
search that CW-001 had proposed as its next step. The proposal was to replace
the nine fixed speed anchors with a searched element length. Implementing it
turned out to need little new machinery, because the event lattice already is a
duration-explicit decoder: it scores a whole segment's run durations against
the dot, dash and three gap classes with a beam search, and merely took the
element length as an input supplied by whichever anchor was leading. Searching
that one parameter, coarse then local and always including the anchor bank's
own answer as a candidate, was therefore the whole change.

It does not pay. The search alone leaves mean character error at 0.307
unchanged, because the lattice feeds the refined transcript rather than the
primary text, and it roughly doubles decode time. Feeding the searched length
back into anchor selection makes copy monotonically worse as its weight rises:
0.307 at zero weight, then 0.317, 0.332 and 0.331. The reason is that the
anchors are not choosing wrongly in the first place. Measured against known
synthetic signals, the leading anchor's speed is within 1.3 per cent of truth
at 20, 30, 40 and 50 WPM at both 20 dB and 12 dB. The premise came from
diagnosing an earlier acceptance-test failure, where a wrong anchor genuinely
did capture a whole segment, and it did not survive the element-timing
corrections that followed; it should not be re-attempted on that reasoning.

What remains under CW-001 is therefore not a speed problem. Copy at 12 dB sits
at 0.719 with the speed already correct, so the residue is noise corrupting
individual elements. The lattice decides against run durations produced by a
threshold, which discards the per-frame margin the detector now measures;
letting it place its own boundaries from that evidence is the remaining
soft-decision step, and it should be justified by measurement before being
built. Farnsworth spacing and 50 WPM stay open. DSP-002 and UI-005 are
unchanged.

Last reviewed: 2026-09-07 (second entry) — replaced the heuristic keying slicer
with a soft decision, the first stage of the weak-signal work. The detector now
emits a calibrated log-likelihood ratio from a two-level model that tracks each
level's scatter separately, so the decision slope is measured rather than set
by hand and an unkeyed channel reports no information instead of being pushed
toward key-up. Mean character error falls from 0.507 to 0.307, concentrated in
the weak columns (20 dB 0.389 to 0.136, 15 dB 0.619 to 0.338, 12 dB 0.915 to
0.719), and keying-style error falls from 0.296 to 0.108. Receiver captures and
level invariance are unchanged. Two intermediate forms were measured and
rejected on the way: a single pooled variance reached only 0.498 because it
cannot express that a mark is noisier than a space, and forcing the decision
back onto half amplitude reached 0.520, which established that the boundary
shift is the gain rather than a defect to be corrected. CW-001 therefore stays
open but narrows: what remains is the duration-explicit sequence decoder that
would search dot length directly rather than choosing among nine fixed speed
anchors, which is the piece that should carry Farnsworth spacing and 50 WPM.
The debounce, the E/T veto and the anchor bank all remain compensations for a
hard slicer that no longer exists, and should retire with it.

Last reviewed: 2026-09-07 — restored the display/detection separation that was
withdrawn on 2026-09-05. The element-timing corrections made since removed the
acquisition sensitivity that had destabilised the hosted live-audio acceptance
test, and with it the change improves every measure: mean character error falls
from 0.545 to 0.507 and receiver-capture recovery rises from six of eight to
eight of nine corroborated callsigns with no callsign asserted on any of the
four captures containing no CW. Two captures previously treated as empty were
found to carry traffic that the earlier detector lost; one is corroborated by
the application's own capture-time diagnostics. A boxcar integrator matched to
the element length was also prototyped for weak signals and rejected: it
improved mean error but smeared element edges by its own window, degrading the
timing corpus and turning consensus SOS into SYS. Weak-signal copy below about
15 dB therefore still needs soft-decision decoding rather than a better filter,
and remains CW-001 with Farnsworth spacing and 50 WPM. DSP-002 and UI-005 stay
active for the wider detection and visualization scope.

Last reviewed: 2026-09-06 — removed the keying-weight bias from the element
estimate after measuring that operator sending style, not just speed and
signal-to-noise ratio, was an untested decoder dimension. Bug-style sending was
previously undecodable. Added keying style (weighting, Farnsworth spacing,
timing jitter) and absolute-level invariance to the local quality gate, and
began scoring receiver captures directly: six of eight externally corroborated
callsigns are recovered, and none of the five captures established to contain
no CW asserts a callsign. Confirmed the keying decision is level invariant -
scaling a whole scene across 52 dB at fixed signal-to-noise ratio produces
identical output - so strength dependence is confined to the deliberately
contrast-adaptive decision band. Damping the paired estimate to 70% of the former
adaptation rate removes the light-weighting regression the first attempt
introduced, taking that case below its original value. An adaptive word-gap
classifier was prototyped and rejected: better on synthetic Farnsworth timing,
but worse on the receiver captures and it asserted a callsign on a capture
containing no CW. CW-001's weak-signal band, Farnsworth spacing and 50 WPM all
remain open.

Last reviewed: 2026-09-06 — narrowband width is now selected from required
keying bandwidth rather than comparative filter power, with the 60 Hz path
restricted to slow signals because its 15.9 ms group delay is a quarter of a
20 WPM element; the keying decision band also adapts to measured contrast.
Mean character error over the audio-driven surface falls from 0.653 to 0.621
and receiver-capture copy improves. A diagnostic run confirmed that spectral
acquisition is not the weak-signal limit — narrowband coherence stays flat at
about 0.386 at every signal-to-noise ratio — and that copy collapses between
15 and 12 dB because the per-interval level slicer fragments elements: 17% of
marks fragment at 12 dB, rising to 90% at 6 dB. Recovering that band needs
soft-decision decoding rather than threshold tuning, and remains CW-001 along
with 50 WPM. PERF-001 and DSP-002 remain active.

Last reviewed: 2026-09-05 — corrected the acoustic front end and element
timing. The keying decision was being taken on a decibel-domain span at a fixed
fraction, placing it far below half amplitude, so marks measured long and gaps
short with the error growing as signal strength grew. Keying now resolves in
linear power at half amplitude, evidence smoothing scales with element length,
impulses are rejected by duration instead of by hysteresis width, and open-gap
timing accounts for the detection delay. Mean character error over an
audio-driven speed/noise surface falls from 0.713 to 0.653 and from 0.363 to
0.053 at 30 dB; acquired-speed error falls from up to 23% to at most 4.2%. The
verification timing floor moves from 0.45 to 0.55 now that real CW and
irregularly keyed noise separate cleanly. CW-001 remains active: weak-signal
copy below about 12 dB is unchanged, 50 WPM is limited by narrowband filter
selection rather than timing, and the full bounded semi-Markov path with
held-out capture calibration is still outstanding. PERF-001 and DSP-002 remain
active.

Last reviewed: 2026-09-03 — added capability-gated RX-frequency entry and
waterfall-edge stepping for linked writable OmniRig/CAT4OM providers. Checked
actual-RF-to-dial conversion preserves transverter offsets, and the active RX
VFO is changed without touching split TX, mode, PTT, or KEY. UI-003, CAT-002,
CAT-003, and CAT-004 remain active for the wider scope recorded below.

Last reviewed: 2026-09-03 — a receiver capture exposed stale callsign
inheritance and showed that exact wildcard matching rejected acoustically
supported substitutions and boundary errors. Replacement trackers now retain
only transcript/color continuity and must establish their own callsign.
Advisory matching accepts at most two wildcard-aware edits only when two
current N-best paths agree on the acoustic winner; ambiguity abstains and an
SCP miss remains visibly acoustic-only. Captures record pre-existing decoder
state and callsign/model presentation diagnostics only while recording.
CALL-005, CALL-006, CW-001, and OBS-003 remain active for indexed candidate
lookup, acoustic accuracy, and full observation-aligned lattice scoring. The
hosted failure marker now retains compiler errors independently of interleaved
parallel-build tail output after the Windows leg hid its failing target. That
diagnostic exposed and the follow-up fixes the missing direct standard-library
include in the callsign-evidence MSVC test.

Last reviewed: 2026-09-03 — a new field capture confirmed correct carrier lock
and roughly 20 WPM cadence but exposed two downstream losses: competitive
timing suffixes were discarded at closed transmission boundaries, and repeated
moderate-confidence character-model overlaps split/duplicated a callsign. The
timing path now finalizes a bounded MAP suffix only at explicit flush, the card
prefers its spaced append-only consensus, and three aligned model windows can
confirm moderate characters. Ordinary-QSO PSE K/K/KN/AR/SK context may rank an
already complete call. Diagnostics record presented-call provenance. CW-001,
CW-002, CALL-006, and OBS-003 remain active for held-out calibration,
contextual N-best rescoring, per-sender cadence, and model qualification.

Last reviewed: 2026-09-03 — added explicit IC-7300 and IC-7610 support scope
under CAT-005. The implementation must stay behind the provider-neutral radio
boundary, retain configurable CI-V addressing, and pass mocked plus documented
hardware acceptance before either rig is advertised as supported.

Last reviewed: 2026-09-03 — added CAT-006 for a runtime, data-driven radio
catalog. Hamlib's own model/status/capability enumeration is the canonical
direct-CAT list; network control programs such as rigctld and Flrig remain
provider entries rather than duplicated radio models.

Last reviewed: 2026-09-03 — disabled the plot-level signal picker until a
receive/replay source is active, restoring the central empty-state start action
and normal cursor. UI-003 remains active for its documented wider scope.

Last reviewed: 2026-09-03 — added a bounded optional local character-refinement
path for operator-supplied ONNX models. It isolates up to four verified,
Morse-likely, or manually selected lanes at 30–50 Hz, performs asynchronous
overlapping-window inference with latest-window load shedding, and exposes
append-only consensus separately from the deterministic transcript. A
structurally valid callsign confirmed across overlapping model windows may now
complete verification only for an already Morse-likely carrier; spectral,
keying, cadence, coherence, and sustained-entry gates remain mandatory. The
model cannot create a carrier, replace raw text, or control transmission.
Character lanes use the robust presentation center so adaptive DSP-center
excursions cannot move the narrow model input off the carrier. Native runtime packaging and
strict model/metadata validation are covered on every desktop architecture.
Windows consumes a checksum-pinned official runtime distribution and disables
runtime telemetry in the application before creating a session; POSIX builds
retain the telemetry-disabled source build and package its upstream privacy
notice plus the loader-required major-version runtime alias. No character model
is bundled or downloaded. CW-002 remains active for corpus qualification,
measured error/resource gates, noise rejection, and a fully independent trained
artifact.

Last reviewed: 2026-09-03 — added the first independently generated synthetic
CW corpus/tooling slice and a dependency-free streaming probability-to-event
decoder boundary. The initial causal GRU experiment predicts only key-down and
target-channel-CW probabilities; temporal metrics exposed excessive transition
fragmentation that aggregate frame scores concealed. Fixed physical feature
scaling, a causal contrast integrator, and stable-region loss materially reduce
that failure, but no model/runtime is shipped until a locked receiver corpus,
character-level gain, runtime budget, provenance, and per-platform packaging
all pass. Established stream ridges are also reserved before global peak
ranking, with stable-center reassociation preventing a returning carrier from
being published as a duplicate beside an internally drifted track.
DATA-002, CW-002, DSP-002, CW-001, PERF-001, and UI-003 remain active.

Last reviewed: 2026-09-02 — connected the bounded timing lattice to live
envelope runs, exposing append-only acoustic consensus separately from literal
text, and added fixed-center manual probes that promote only through ordinary
verification. Right-click creates a manual probe while left-click remains
dedicated to detected streams; a stable plot-level pointer router prevents live
model refreshes from destroying a marker between press and release. Operator
labels now use the stable presentation center. CW-001,
UI-003, DSP-002, and CW-004 retain the calibrated confidence, cancellation,
weak-signal filtering, close-carrier, and true co-channel work documented below.

Last reviewed: 2026-09-02 — implemented the capture-driven CW recovery slice:
bounded recent decoder evidence, continuous processing of every fixed WPM
hypothesis with safe-boundary winner selection, saturated-bank replacement,
track identity jump rejection, adaptive key-envelope normalization, bounded
coherence, and verification enter/exit hysteresis. Added a native private-WAV
replay audit and selectable profile-persisted Audio spectrum/CW symbols views;
the latter displays verified-channel acoustic keying envelopes on a neutral
background rather than inventing decoded glyphs. CW-001 and DSP-002 remain active for the full semi-Markov path,
held-out calibration, and legally reusable real corpus.
Last reviewed: 2026-09-02 — bounded decoder input by spectral association:
after the existing 750 ms normal-gap hold, an unmatched verified or private
track receives one forced key-up/flush and its decoder remains frozen until a
real candidate matches again. A deterministic alternating-carrier fixture uses
a stronger station 85 Hz away and also preserves a 600 ms same-frequency gap.
DSP-002 remains active for calibrated multi-signal association, and CW-004
still owns true overlapping/co-channel separation.
Last reviewed: 2026-09-02 — decoder cards now follow live text, emphasize the
confirmed station call, and provide a bounded flashing visual alert for an
exact profile own-callsign match. Card close no longer competes with whole-card
interaction, explicit up/down controls provide refresh-safe ordering, and
unidentified vertical markers show only frequency. QSO-003 is
active for configurable visual behavior plus its unimplemented audio/remote
notification scope; CW-001 remains active because a new capture confirms that
compressed character/word gaps and digits require ambiguity-preserving timing,
not a global threshold reduction.
Last reviewed: 2026-09-02 — stabilized the decoder transcript viewport under
live updates: appends scroll only the viewport while it is following the
bottom, never move the text cursor, and do not override text selection or an
operator's upward scroll. Plain text plus a permanently reserved scrollbar
gutter prevents rich-text and scrollbar-driven line reflow; confirmed calls
remain prominent in the card header. Its contract test normalizes platform
line endings. UI-003 remains active for its wider interaction scope.
Last reviewed: 2026-09-02 — corrected the live-view selector auto-hide
regression and replaced indirect marker tapping with a direct pointer target.
Decoded text is now scrollable/selectable, and automatic stream naming requires
callsign structure plus exchange-role or repetition evidence. A same-frequency
field capture confirms that this role evidence cannot substitute for the
remaining CW-001 acoustic timing work or CW-004 operator separation.
Last reviewed: 2026-09-02 — corrected the macOS bundle's required identity
metadata and final resource-sealing order after a clean downloaded ARM64 build
was rejected as damaged. PKG-002 remains active for Developer ID signing,
notarization, and clean-machine acceptance; CI now rejects empty plist identity
fields or any invalid staged bundle resource envelope.
Last reviewed: 2026-09-02 — a new live capture confirmed stream acquisition but
showed repeated passes on one carrier receiving new IDs/colors. Silence now
keeps a verified observation for the actual configured timeout instead of
demoting it after the short failure hold; expired tracks reuse a bounded
frequency-color lease for at least five minutes. CALL-006 remains active for
callsign-level identity and stronger same-carrier session continuity.
Last reviewed: 2026-09-02 — anchored the five-minute color lease to the
frequency that established it so a stale verified tracker cannot walk the
remembered identity through nearby noise. Stabilized verified-marker geometry
at a fixed presentation width; the adaptive decoder filter remains diagnostic.
Inactive observations now reduce to an axis mark, stream labels enlarge on
hover, and the CW receive guide uses two unfilled dashed width boundaries.
Display values now use labeled sliders in a responsive multi-row layout rather
than an overflowing row of number boxes.
Last reviewed: 2026-09-02 — made trace activation open a larger decoded-text
window reliably and reconcile an operator-opened session across same-frequency
tracker reacquisition. Retention now preserves identity without allowing
unmatched residual noise to present the carrier as active or draw CW symbols.
The bottom live controls auto-collapse to their header and can be pinned open.
Gap prediction is capped, stale drift decays, verified-exit hysteresis is six
seconds, and a fixed presentation anchor plus short word-gap activity hold
reduce contest-stream churn and area flicker. Bounded session text and a
structurally plausible callsign persist across same-identity reacquisition.
UI-003, UI-005, and CALL-006 remain active for their documented larger scope.
Last reviewed: 2026-09-02 — added independent bounded 1:3 mark / 1:3:7 gap
cadence fitting and guarded decoder reacquisition for a cadence-confirmed
carrier stuck in implausible unverified text. Replaced global waterfall gating
with per-bin/local-side conditioning so narrow CW marks remain visible without
broad passband texture, and made continuous manifest publication last with
bounded client retries for transient release-asset errors. CW-001, DSP-002,
UI-005, OBS-003, and PKG-004 remain active for their documented larger scope.
Last reviewed: 2026-09-02 — the live integration fixture now exercises the
sustained-verification interval with five keyed repetitions, validates both
averaged and instantaneous spectrum output, and emits bounded failure
diagnostics. The independent character-confidence floor is 0.40 while cadence,
pure timing, and the complete hard-negative corpus remain separate gates.
Last reviewed: 2026-09-01 — added AUDIO-002 (selected-track audio monitor
output: play back only the selected decoded CW track's isolated,
700 Hz-repitched narrowband audio to a PC output, like an operator-enabled
bandpass filter tied to the identified trace); no implementation yet.
Last reviewed: 2026-09-01 — fixed `timing_quality` and
`mean_character_confidence` being mathematically forced identical (traced
directly to real contest debug-capture data: a track with a legible `TEST`
in its text never verified because the combined metric never crossed
threshold); see the `CW-001` note for the fix and its remaining known gap
(lifetime-cumulative rather than windowed averaging). Also added CW-006
(recognize well-known CW/contest patterns like CQ, TEST, 599, 5NN, TU, UP
as independent verification evidence, motivated by the same finding) as a
not-yet-implemented follow-up.
Last reviewed: 2026-09-01 — retuning the linked radio's VFO previously lost a
signal's tracking identity, so the receive path added
`CwChannelBank::shiftTrackedFrequencies()`: a retune while live audio is
running now re-centers every currently tracked signal by the exact
audio-domain shift implied (accounting for CW-U/CW-L sideband direction)
and resynchronizes each track's narrowband filter, without discarding
decoded text or verification state. Also gave the VFO readout rig-display
decimal precision (e.g. 7016.45 kHz), and added RX/TX frequency and split
state to every debug-capture diagnostics line so a VFO move during a
capture is visible after the fact.
Last reviewed: 2026-09-01 — implemented the check/download/verify/guided-
install slice of PKG-004: background + manual update
checks against the published manifest's version field, SHA-256-verified
download to the Downloads folder, and handoff to the OS installer/package
handler rather than a silent self-install (deferred until PKG-001/PKG-002
signing lands). Also enlarged the VFO readout (green RX / yellow TX / SPLIT
badge) to match the decoder panel's visual weight, added a styled but
intentionally unwired "ON AIR" placeholder (no backend reports real PTT/
transmit state yet — see CAT-002, CAT-004), and moved the CW guide's axis
line to sit on the spectrum/waterfall boundary rather than the bottom of
the waterfall.
Last reviewed: 2026-09-01 — added DOC-002 (render documentation diagrams,
e.g. Mermaid, instead of the ASCII art currently in docs/architecture.md,
docs/decoder-strategy.md, and docs/decisions/0001-qt-quick-spectrum-renderer.md)
with no implementation yet.
Last reviewed: 2026-09-01 — added PKG-004 (application update checking and
guided install: periodic/manual update checks against the published release
manifest, operator-confirmed download/checksum-verify/install/cleanup) per
the documented safety policy; no implementation yet.
Last reviewed: 2026-09-01 — swapped which spectrum overlay reads as an
"area" to remove visual ambiguity: the CW
pitch guide is now an unfilled pair of dashed vertical width boundaries, and
an active verified CW track is identified primarily by a
stable-width colored vertical area, with the keying-state line drawn thinner
on top. The adaptive filter width remains available as decoder diagnostics.
Last reviewed: 2026-09-01 — renamed the decoder panel from "Full-spectrum CW
decoder" to "CW Decoder" and added a VFO frequency readout showing the
connected radio's actual RX dial frequency (and TX dial frequency when split
is active), hidden entirely unless a live radio/CAT source is actually
linked and driving the current audio input in live-audio mode — never shown
for receive-only SWL setups or WAV replay, which have no radio state to
show. Reuses the existing `resolve_frequencies` core logic; `AppSettings`
gained `controlledTxRfHz()`/`controlledSplitActive()` alongside the existing
RX-only accessor.
Last reviewed: 2026-09-01 — fixed the root cause of the field-reported
"visible CW never decodes" case, isolated using a real operator debug
capture: falsely verified tracks decoded to text overwhelmingly made of the
two single-element characters E and T (the statistical signature of timing
noise, not genuine text), which the existing unknown-symbol-fraction gate did
not catch since it stayed under threshold throughout. Added a
character-distribution plausibility gate
(`CwVerificationReason::ImplausibleCharacterDistribution`) that re-checks
even an already-verified track once enough decoded text has accumulated to
judge it, calibrated directly against the real capture's numbers (0.35
threshold; the three real false positives measured 0.59/0.45/0.76, the
benchmark's legitimate text measures 0.25). Exposed as a standalone testable
function; full `ctest` suite and `cwa_verification_benchmark` hard negatives
confirmed clean, plus a real-GCC local compile (designated-initializer
strict) as an extra portability check.
Last reviewed: 2026-09-01 — implemented an initial OBS-003 slice: an
operator-started, bounded "Debug capture" that records raw live audio (WAV)
and a per-track private diagnostic log (JSON lines, once per second) to a
timestamped folder, capped at 5 minutes, never silent. Added a
dependency-free WavWriter (round-trip tested against WavReplaySource) and
CwChannelBank::allTrackDiagnostics() exposing full per-track state for every
track regardless of verification. Verified end to end with an extended
cwa_live_audio_pipeline_test driving a real decode and checking both output
files. This is the requested path to real field data now that synthetic
reproduction of the reported "visible CW not decoding" case has not
succeeded (see the entry below).
Last reviewed: 2026-09-01 — restored the pre-verification diagnostics as an
opt-in, once-per-second "Diagnostics" toggle in the decoder panel (off by
default) instead of a continuously live-updating label, after a direct
instrumented investigation into a still-unresolved field report of visible
CW streams not decoding. That investigation confirmed the core algorithm
itself reliably verifies a clean or realistically-noisy single tone within a
few seconds and creates no spurious tracks against a synthetic bumpy noise
floor, so the reported field case (many simultaneous candidate/Morse-likely
tracks saturating the 24-track cap) was not reproduced synthetically; real
diagnostic data from the operator's environment is needed to isolate it
further, which the restored toggle is intended to provide.
Last reviewed: 2026-09-01 — fixed a verification-state/reason inconsistency
where a track could stay reported as Morse-likely after later failing an
earlier gate again, confirmed against two operator screenshots showing this
exact contradiction; added `CwChannelBank::configure()` and a Settings →
Display "Decoded signal timeout" control (default 30 s, replacing a fixed
8 s) so an already-running decoder session can have its retention changed
without restarting; and removed the initial always-visible decoder-panel
diagnostics readout after confirming it was constantly flickering and not
useful in practice (`narrowband_coherence` is naturally noisy per-instant
evidence even for a clean tone), while keeping the underlying diagnostics
data on the model for a future dedicated view. Also reverted a first attempt
at bank-capacity eviction (letting a stronger candidate replace the weakest
unverified track when full) after a deterministic test showed it could evict
a genuine intermittent CW signal's own track during its normal key-up gaps;
that starvation theory (the bank saturating at its 24-track cap with
marginal candidates) remains plausible and worth a more careful design, but
is not fixed yet.
Last reviewed: 2026-09-01 — added a decoder-panel diagnostics readout exposing
pre-verification candidate/Morse-likely counts and a rejection-reason tally
without overlaying unverified candidates, and a deterministic
broad-spectral-hump hard-negative benchmark case confirming the local-
prominence guard rejects genuinely wide non-CW spectral features (adjacent
SSB audio, AGC pumping, a receiver-filter skirt) before any track is created,
in response to an operator screenshot showing two broad spectral features
that were not decoding.
Last reviewed: 2026-09-01 — reproduced and fixed hosted QML compilation,
validated the Qt desktop build/install locally, added functional WAV spectrum
replay, corrected macOS bundle deployment and Linux Qt architecture selection,
set and verify the macOS Sonoma 14+ deployment baseline, documented the
temporary Windows SDK-downloader source pin, added remotely queryable CI
outcomes/log diagnostics plus extractor retry, corrected WiX license input and
added MSI failure diagnostics, and replaced the Windows archive plan with an
upgrade-capable MSI delivery flow. The first fully green packaged matrix then
exposed a native first-launch render crash; the null-texture path is now removed
and guarded by empty-render, full-QML, and staged native-graphics startup tests
with deterministic texture creation on every platform. Setup now distinguishes
receive-only SWL operation, positively identified online radios, and explicit
manual templates; the wizard footer is also guarded against clipping. Bounded
compiler diagnostics are now available through the Git-only CI status markers,
which identified and closed the initial setup-dialog QML syntax failure and
the subsequent cross-platform Qt macro/declaration errors. Native audio-input
enumeration, per-profile selection, and bounded live RX now run for both radio
and SWL setup. Station settings include the normalized own callsign and wider
content margins. A CW operating mark is integrated across application packages,
and the Windows shortcut/program-group contract has been rechecked. The
standalone render regression remains linked against the complete receiver
source after live-audio integration and handles every current Qt sample-format
enumerator without compiler warnings. Live DSP timer affinity is now covered by
a cross-thread FFT regression that prevents blank output with queue overruns.
Audio conditioning now separates default DC rejection, optional automatic or
manual gain, visualization scaling, and automatic/manual bandwidth with
deterministic coordinate and gain tests. Decoder planning now includes an
operator-toggleable, provenance-visible callsign prediction/validation service
that never overwrites raw decoded text. Live signal/display controls now sit
below the spectrum; stable automatic levels and waterfall-only noise
suppression reduce color pumping without altering raw decoder input.
The M2 plan now defines a measured hybrid decoder, bounded multi-pass weak-signal
refinement, and same-frequency pileup separation using operator fingerprints,
joint timing inference, conservative cancellation, and optional receive
diversity.
The waterfall now has a constant profile-selected time span independent of pane
size, startup fill, and line density, preserves timestamp gaps, derives genuine
high-rate timing frames with overlapping FFT hops, and provides a configurable
CW guide plus X-axis frequency scale.
The receive-only decoder now scans the complete processed passband, maintains a
bounded independently colored state per detected frequency, publishes soft key
evidence plus provisional/stable text, and removes silent tracks consistently
from both overlay and decode-list models. The configurable 700 Hz guide is
visual-only. Confidence calibration, same-frequency separation, and
multiple-pass stages remain active backlog work.
Sub-bin interpolation and bounded drift prediction now feed automatic
60/120/240 Hz filters with asymmetric local-noise tracking. Decoder cards are
operator-opened from vertical colored markers, independently closable and
reorderable with explicit up/down controls while every track continues decoding. Conservative callsign
tokens appear vertically on their trace. Explicit profile audio/radio pairing,
CW-U/CW-L mapping, RX transverter resolution, live Windows OmniRig polling, and
CAT4OM state now produce RF labels only when the complete evidence chain is
valid; every other source remains explicitly AF.
The first hosted build correction normalized Qt's platform-sized session-list
index before clamping so GCC, Apple Clang, and MSVC share the same bound.
Versioning now derives application, About, native package/bundle metadata,
network identity, installed version record, and release manifest from one
effective CMake value; the hosted workflow revision is the patch component.
Raw spectral candidates are now private DSP state until local prominence,
repeated observation, known-symbol ratio, and timing-quality gates verify a CW
trace. Shaped broadband noise is a deterministic hard negative. Callsigns are
withheld until a stable completed word passes the same evidence gate, and their
vertical annotations now remain in the upper spectrum. The CW guide is a
translucent band rather than two signal-like lines.

## P0 — project decisions and safety

| ID | Status | Item | Acceptance |
|---|---|---|---|
| DEC-001 | done | Select an OSI-approved project license | GPL-3.0-or-later text and dependency/license policy are committed. |
| REQ-001 | done | Fix supported WPM, prosigns, character sets, and break-in scope | Settled by the repository owner and written into `docs/requirements.md` as testable requirements: 8-60 WPM, ASCII character set with international characters out of scope, seven transmittable prosigns keyed as single symbols, and semi break-in for the initial scope with full QSK deliberately not foreclosed. A distress prosign is transmittable; it is not specially excluded because unintentional transmission is already prevented by arming, exact callsign confirmation, preview, explicit send, and a decoder that can never initiate one. Transmit prosigns are a closed table in the application rather than a data file, and an unrecognised bracketed token is refused during normalization. |
| HW-001 | active | Validate initial Yaesu radios and direct serial keying | FT-450D and FT-818 editable defaults and preliminary safety notes are recorded; physical adapter polarity and disconnected/dummy-load procedures still require validation. |
| SAFE-001 | active | Implement independent maximum-key-down watchdog | The dependency-free guard rejects out-of-state KEY, limits message elements to three continuous seconds, gives operator-only TUNE a distinct hard 15-second limit, and latches emergency release/fault until an explicit reset. The controller opens the configured adapter safely inactive only after a configuration-bound measured loopback and authoritative station-state confirmation, observes KEY independently, and disarms or emergency-releases on changed state. The worker enforces its own monotonic TUNE deadline, exposes authoritative elapsed/remaining progress, and revision-orders cross-thread snapshots; cancellation/error/shutdown release KEY before PTT. Remaining: execute physical device-error/process-shutdown/line-loopback and dummy-load tests on Windows/macOS/Linux. |
| ARCH-001 | done | Select graphical rendering architecture | ADR 0001 records the scene-graph approach, modular boundaries, 2D scope, and fallback policy. |
| ARCH-002 | done | Define secure remote-operation boundaries | ADR 0002 records roles, transports, authentication, leases, reconnect, and station-local TX rules. |

## P1 — M1 receive and visualize

| ID | Status | Item | Acceptance |
|---|---|---|---|
| BUILD-001 | done | Establish dependency-free C++20 core build | Core builds and tests passed on Windows x64, Linux x64, macOS ARM64, and macOS x64 in the first complete hosted matrix. |
| PROC-001 | done | Keep manuals, changelog, and backlog current | Repository guidance and PR automation require user manuals plus both project records for implementation and delivery changes. |
| CI-001 | done | Add full desktop dependency/build matrix | Qt 6.11.2 desktop and core tests pass on Windows x64, Linux x64, macOS ARM64, and macOS x64; successful jobs publish artifacts, stable continuous-release assets/checksums, and a verified-commit tag. Existing release assets are replaced with bounded retries to tolerate GitHub's deletion-propagation window, while the manifest and verified tag remain final commit points. A source-contract guard preserves the Windows SDK include order required by the MSVC COM implementation. |
| CI-002 | todo | Add Windows 11 x64 runtime acceptance | Self-hosted or release-candidate testing launches the packaged app and verifies graphics/audio/serial discovery on Windows 11. |
| VER-001 | done | Keep application and package versions consistent | One effective `major.minor.revision` value drives About, Qt identity, Windows executable/MSI metadata, macOS bundle metadata and in-bundle VERSION record, Debian package, CAT4OM identity, and the continuous manifest; hosted checks compare native metadata and installed records before publication. |
| AUDIO-001 | active | Add native audio device discovery and capture | Qt Multimedia discovery, hot-plug/default/unavailable state, per-profile selection, permission-gated live capture, PCM conversion/downmix, allocation-free bounded capture queue, DSP worker, overrun count, DC rejection, bounded manual/automatic gain, selectable/automatic bandwidth, and deterministic pipeline tests are implemented; add operator channel/rate/block controls, level meter, signal-driven bandwidth recommendation, disconnect/reconnect soak tests, and clean-machine hardware validation. |
| AUDIO-002 | active | Add selected-track audio monitor output | Off/full-receiver modes, PC output-device selection, live level control, and bounded low-latency Qt audio output are implemented. The global toolbar exposes only whole-window RX monitoring. Opening a stream changes only its decoder card; per-card speaker controls are the sole way to add or remove independently filtered streams, and several streams may be mixed with bounded gain. The speaker action is in a stable lower card row and records intent on press, so transcript/model refresh cannot swallow the click or make it depend on stream activity. Removing the final speaker returns monitoring to Off. Each selected lane follows its decoder tracking mixer/adaptive filter, rejects adjacent audio, is re-pitched to the configured reference tone, and uses bounded post-filter level normalization with fast attack/slow release so disparate SDR levels remain audible without fast gap pumping; RX preserves the complete receiver audio. Remaining: output-device hot-unplug recovery, format conversion for devices rejecting source-rate mono float, underrun/drop diagnostics, listening tests, and a continuously adjustable monitor filter. |
| REPLAY-001 | active | Add WAV replay source and deterministic clock | Dependency-free PCM/float parsing, deterministic timestamps/restart, downmix, paced UI selection/play/pause/stop, and core tests pass; hash manifests, seek, looping, and repeat-run integration remain. |
| DSP-001 | active | Implement windowing, FFT, and spectral averaging | Hann-windowed radix-2 audio/IQ analysis, dBFS normalization, averaging, frequency mapping, and deterministic tone tests pass; golden fixtures, overlap, calibration, and performance benchmarks remain. |
| UI-001 | active | Create Qt Quick desktop shell | Modern expandable receiver workspace, Settings/About panes, author metadata, profile chooser, guided setup, maximized startup with a larger decoder pane, contextual action tooltips backed by a QML source-contract test, cross-platform offscreen QML tests, and staged native-graphics startup tests are implemented. Radio Control and CW Decoder are distinct right-column sections, with decoder diagnostics/capture controls kept in the decoder header. The radio console uses vertically aligned RX/TX mode controls, protected fitting RX/TX digits through 99 GHz, an orange TUNE tile beneath the compact borderless ON AIR mark, and centered SPLIT/A=B controls; active TX/TUNE progress is visible in the QSO drawer. Clean-machine hardware validation remains. |
| UI-002 | active | Implement modular 2D scene-graph spectrum/waterfall | Public Qt scene-graph line/grid geometry and a backend-native, valid-texture-only waterfall image node render real replay FFT frames with bounded history; empty startup/reset regression tests pass; add palette shader/ring uploads, peak hold, overlays, metrics, and performance validation. |
| UI-003 | active | Add clickable channel/callsign overlays | A stable-center/width colored area is the primary identification cue for each verified CW track, independent of adaptive carrier/filter changes, with a thinner keying-state line on top and an 18 px label enlarged to 32 px on hover. A stable plot-level left-button router opens/reopens a larger scrollable/selectable decoded-text card without changing monitor audio; each card has a separate speaker action for explicit multi-stream listening. Close leaves DSP active, drag plus focused Up/Down keys reorder cards, and an open card follows same-frequency/color reacquisition under a replacement internal ID. Hover explains left-open, right-manual-probe, and capability-gated Ctrl+left TX-VFO selection. A context-confirmed callsign becomes the prominent marker/card label; otherwise the marker shows only stabilized frequency. Right-button trace-area hit testing leaves the independent TX-slice guide unchanged and opens a temporary neutral manual region: measured weak evidence receives priority, text/count/color remain withheld until ordinary verification, qualified carrier movement follows the normal bounded tracker, centers outside a 12 Hz click-reuse boundary stay distinct, and an unverified region expires after the configured decoded-stream timeout. Successful probes promote in place. A linked writable radio supports exact RX entry from a grouped radio-style readout with reliable Enter/Escape/focus-loss dismissal and fixed waterfall-edge stepping by a persisted 1–100 kHz setting; the provider-neutral faceplate exposes authoritative independent RX/TX frequency, mode, VFO and split state without fabricating unavailable values. Add explicit manual-probe cancellation, direct pointed-signal radio retune/confirmation, keyboard tuning, and signal/band navigation. |
| UI-004 | todo | Add render-backend diagnostics and fallback tests | Active API, frame/upload metrics, and fallback reason are visible; replay smoke tests cover shader and CPU fallback paths. |
| UI-005 | active | Model configurable visualization | FPS, waterfall line rate, constant 5–30 second history, range bounds/mode, stable automatic span, waterfall noise suppression/margin, averaging, grid, authoritative TX-slice guide, and seven-point frequency scale are configurable and persisted. A live profile-persisted selector switches between averaged Audio spectrum and a crisp CW symbols raster without resetting decoding; its popup keeps the auto-hiding controls open while in use. Audio suppression uses a slow per-bin baseline plus local side references; CW symbols instead draws only currently matched, active verified channels' keying envelopes on a neutral background, leaving retained identity and full-passband/unverified noise out of the raster. Startup, resize, and timestamp gaps cannot collapse or stretch time, and overlapping FFT hops provide real timing samples at the selected line rate. Wheel zoom, middle-button pan, explicit full-span reset, and the SDR decoder-window overlay now preserve operational context across a wide acquired passband. The spectrum gesture legend has a ten-second lifetime, a five-minute redisplay cooldown, and a persisted opt-out independent of ordinary button tooltips. Immediate Signal/Display controls auto-collapse below the spectrum, can be pinned, and retain explicit profile saving; peak hold, time ticks, palette selection, and persisted zoom remain. |
| UI-006 | done | Distinguish operational guides from detected traces | The guide is an unfilled pair of dashed vertical boundaries, so it cannot be mistaken for an identified signal; UI-007 now drives it from authoritative TX-VFO readback. Active verified traces use a colored vertical area, while inactive retained traces reduce to a short identity-color axis mark. Stream labels are larger and magnify again on hover; every trace remains independently colored and clickable. |
| UI-007 | done | Replace the fixed CW-pitch guide with an authoritative TX-slice guide | The configured CW-width boundaries now follow authoritative independent VFO B/TX readback, mapped directly into SDR RF or sideband-aware sound-card audio. Physical-radio VFO/frequency/split changes therefore move the guide without optimistic UI state; unknown, replay, or off-screen TX state hides it. Capability-gated Ctrl+left-click converts the pointed coordinate to checked exact RF, requests it through the provider-neutral TX route, and enables split when required and supported. Its capability gate depends on the pointed absolute RF plus TX/split write support, not unrelated RX readback; OmniRig targets the last authoritatively identified TX VFO across the split transition. Plain left-click remains decoder open, right-click points the receive window, CTRL+Right-drag owns SDR window selection, and full-span reset is an explicit button so no gesture fires two actions. Hover explains the available gesture. The guide is informational only and grants no decoder, PTT, KEY, or transmission authority. |
| UI-008 | done | Add a capability-aware RF ruler at the spectrum/waterfall separator | Render readable frequency ticks and labels in the horizontal separator using the current visible span, zoom/pan viewport, authoritative RX center/VFO readback, sideband mapping, and receiver tuning limits. The ruler follows physical-radio frequency/VFO changes and SDR retunes immediately and chooses useful tick spacing without label collisions. Show absolute RF only when the source mapping is trustworthy; otherwise label the scale explicitly as audio offset in Hz rather than fabricating RF. Keep stream, TX-slice, and future spot markers visually distinct from ruler ticks. Requested by the repository owner for audio sources specifically: when an audio input is linked to a radio, the horizontal axis should read absolute RF as it already does for a direct SDR source, with the audio scale shown on the separator so the width of the visible spectrum stays legible. The audio-to-RF mapping and its inverse already exist and are tested. The main axis now reads absolute RF whenever the receiver's frequency is known, honouring sideband and falling back to audio when there is no dial to map against -- the half the owner asked for twice. Delivered in full: the main axis reads absolute RF whenever the receiver's frequency is known, honouring sideband and falling back to audio when there is no dial to map against, and the separator carries the audio-offset scale marked AF, drawn only where the two scales genuinely differ -- on direct IQ the frames already arrive in RF, so a second scale there would reprint the same numbers. Tick spacing is chosen from the panel width against the plated label width so labels cannot collide. |
| CALL-002 | todo | Add delayed callsign detail card | Hover delay and press-hold show live signal/context plus asynchronous log and prefix enrichment without initiating QSO. |
| CALL-003 | active | Persist and enforce exact callsign ignore list | Core normalization and TX denial are implemented; persistence and filtering in display/queue models remain. |
| OBS-001 | active | Add pipeline telemetry | Pre-verification candidate/Morse-likely counts and a rejection-reason tally are exposed as an opt-in, once-per-second "Diagnostics" toggle (off by default) in the decoder panel after an always-visible version proved too flickering; overruns, sequence gaps, queue depths, DSP latency, and dropped display frames remain to add. |
| OBS-003 | active | Add operator-controlled diagnostic capture bundles | A "Debug capture" control records raw live audio (WAV) and per-track private diagnostics (JSON lines, 1 Hz, every track including unverified, now also carrying RX/TX radio frequency and split state, bounded completed turns, and strong-evidence current-sender/cadence fields on every line) to a timestamped folder, capped at 5 minutes, requiring explicit start and never silent. The control now also lives in Settings → Decoder with a button that opens the capture folder in the operator's file manager, and the auto-stop duration is a persisted 30–1800 second setting rather than a fixed five minutes; the decoder-panel status line reports the configured limit instead of advertising 300 seconds. Remaining: conditioned/spectrum frames, overruns, a review step, and credential/private-identifier redaction before export. |
| OBS-002 | todo | Add operator-accessible native crash diagnostics | Windows minidumps and macOS/Linux crash-report guidance identify build/profile/backend without exposing station secrets; diagnostic export is documented and tested. |
| CFG-001 | active | Implement named station profiles and setup helper | Versioned isolated persistence, UI create/select helper, per-profile wizard, and `--profile` selection exist; audio/logger/remote pages and migrations remain. |
| CFG-002 | todo | Enforce cross-process hardware ownership | Named OS locks prevent serial/audio/SDR devices from being opened by two active profiles and report the owning profile. |
| CFG-003 | todo | Build a station connection-profile wizard | Create and recall a named, generic station topology instead of shipping hard-coded equipment presets. The wizard binds RX source/device/tuner/input, antenna, sample rate, RF bandwidth, gain, decoder window, monitor/audio route and RX offset independently from the TX radio/provider/VFO, CW mode, TX offset, keying route and safety acceptance. Its current radio page projects only fields owned by the selected integration: OmniRig slot/native setup, rigctld endpoint/VFO/write permission, or CAT4OM service identity; local serial framing is reserved for an implemented direct-CAT provider. It explicitly models half- or full-duplex operation, including one radio plus its sound-card audio, an SDR receiver paired with a separate transmitter, and independent transverter offsets. Validate device ownership, frequency domains, unavailable capabilities, conflicting routes and incomplete TX safety gates before saving or activation. Profile switching applies the complete receiver/radio configuration atomically or rolls back with a clear diagnostic. The rewritten examples in `docs/manuals/configuration-reference.md` are public acceptance scenarios, not built-in presets. |

## P1 — M2 multichannel CW decode

| ID | Status | Item | Acceptance |
|---|---|---|---|
| DATA-001 | todo | Register the located CC0 pileup WAV | Manifest records source, CC0, checksum, audio format, preprocessing, and storage location. |
| DATA-002 | active | Build deterministic synthetic CW corpus | A reproducible generated-from-scratch PCM corpus now covers 8–55 WPM, manual timing variation, Farnsworth spacing, shaped edges, fading, drift/flutter, receiver gain/compression, hum, impulses, nearby CW, steady/AM carriers, exact key-run annotations, checksums, and profile-grouped leakage-safe splits. Generated contextual calls and a one-minute no-carrier hard negative feed executable quality gates. The bounded checksum-bound TSV sidecar remains compatible with v1; v2 adds sorted reviewed-coverage intervals, including explicit event-free no-CW spans. Uncertain events protect a matched track but do not enter scores. Replay separately reports published-call and transcript-extractability precision/recall, and counts false publication/callsign episodes only inside reviewed coverage. Add broader receiver/audio-path simulation, exact and near-exact co-channel pileups, legally reusable reviewed annotations, a locked blind receiver pack, and versioned corpus releases. |
| DSP-002 | active | Detect and track candidate CW tones | The bounded full-passband bank provides sub-bin peaks, bounded gap/drift prediction, nearby-candidate suppression, numerical-floor and FFT-resolution-aware near/far prominence guards, and private-candidate expiry. Each track separates an immutable association origin, adaptive DSP center, and fixed-width presentation center: robust evidence corrects first-verification bias, while sustained coherent motion follows through deadband, slew, dispersion/drift, and absolute-origin guards without moving identity/color. Saturated admission replaces only weak unmatched unverified occupancy, established identities reject large or cumulatively walking innovations, decoded/Morse-likely candidates survive normal word gaps, and automatic candidates reuse one identity per configured separation cell across frames so keyed FFT sidelobes cannot clone a carrier. Verified/operator identities reserve first. Two-sided noise references feed an adaptive per-track key envelope; coherence is a bounded spectral-concentration measure. Candidate → Morse-likely → verified → lost transitions combine recent spectral, edge, cadence, known-symbol, timing, confidence, and character-distribution evidence with separate enter/six-second exit hysteresis. A cadence-confirmed track held in implausible unverified text reacquires only its timing decoder instead of poisoning a later transmission at the same carrier. Only verified tracks publish IDs/colors. Deterministic saturation/identity/recovery/presentation tests and hard-negative verification benchmarks pass; native capture replay and expanded frequency/match/activity diagnostics support field audits. Add legally reusable recordings, held-out threshold/confidence calibration, stronger quantile/envelope estimation, and measured publication/character-error targets. |
| DSP-003 | todo | Add bounded per-channel DSP worker pool | Preserves channel order, sheds lowest-priority work, and passes overload soak tests. |
| CW-001 | active | Implement explainable adaptive timing baseline | Every detected passband track converts adaptive per-track key-envelope evidence to smoothed key probability and evaluates nine bounded 8–60 WPM timing hypotheses. The leader remains provisional during acquisition; every fixed anchor continues processing afterward, and sustained-silence/end-of-input boundaries reselect the best complete path rather than making the early choice irreversible. Association loss drains acoustic state but becomes a semantic turn only when the longer silence test passes, preserving slow word gaps. A separate bounded run-length fit estimates acoustic WPM directly from 1:3 marks and 1:3:7 gaps; pairing each mark with its following gap makes reported/per-sender WPM resilient to hand-key weighting, while a separate original mark/gap estimate continues to control filter, recovery, and lattice decisions. Up to eight explicitly identified senders retain separate cadence summaries; a supported prior may nudge only an already compatible live estimate by 30 percent. Character/timing/cadence quality and unknown fractions use bounded recent windows. The dependency-free event lattice exposes up to four observation-scoped alternatives plus a separate append-only consensus. Provisional commitment now stays at least one second and six later observations behind the newest evidence, so later spacing can refine a bounded suffix; a brief association loss preserves it, while sustained silence or explicit flush finalizes it. Literal/refined callsign disagreement is ranked across both paths, and a refined-only deglued prosign cannot establish a label alone. At a completed turn, bounded context may choose only a competitive path with the exact same non-whitespace characters and repair a small set of word boundaries. Remaining: held-out confidence calibration on reviewed receiver audio, capture-derived jitter/edge fixtures, and automatic close-carrier/co-channel separation. Validate every extension against `PERF-001` CPU/state budgets and native capture replay. |
| CW-002 | active | Add compact causal learned likelihood path | The independent experimental causal GRU still predicts only key-down and target-channel-CW probabilities, with explicit recurrent state, deterministic training/evaluation, temporal-fragmentation metrics, anti-aliased WAV inference, and checked ONNX export. A separate optional desktop refinement boundary now accepts an operator-supplied character model: strict metadata/tensor validation, per-architecture ONNX Runtime packaging, bounded 30–50 Hz stable-center lanes, asynchronous latest-window load shedding, timestamped CTC hypotheses, stale-generation rejection, and append-only overlap consensus are implemented. At most four verified, Morse-likely, or manually selected tracks are refined. A structurally valid callsign confirmed across overlapping windows may complete verification only after the same track independently passes carrier, keyed-edge, cadence, coherence, and sustained-entry gates. Model output remains separately labeled and cannot create a carrier, rewrite raw text, keep silence active, or control TX. No character model is bundled or downloaded. Remaining: qualify models on a locked legally reusable receiver corpus, publish CER/callsign/no-CW/resource gates, improve segment/noise abstention, train a fully independent artifact, and retain the deterministic fallback. |
| CW-003 | active | Add bounded multiple-pass weak-signal refinement | Completed turns now re-decode the retained 128-run physical lattice at up to nine distinct live timing hypotheses. A dependency-free selector requires aligned intervals, bounded symbol/observation differences, no confidence loss, material absolute and relative acoustic-cost improvement, clean extension of the committed boundary, and abstention on contradictory near-ties; it never changes provisional live text. Remaining: retain a bounded rolling three-width receive-feature window, retry filter/track hypotheses under load shedding, publish pass provenance, and prove receiver-CER improvement on reviewed coverage. |
| CW-004 | active | Separate co-channel pileup operators and cancel interference | Each completed turn can now carry a bounded, fail-closed fingerprint derived from its exact timestamped physical key runs: dit/dah and gap counts/medians, weighting, normalized timing residual, evidence interval, and duration-weighted confidence. It deliberately carries no sender, callsign, or carrier identity and cannot affect attribution. Remaining: combine it with aligned sub-bin carrier/phase drift, edges and fading, calibrate fingerprint distance on reviewed overlaps, implement a bounded two-then-three-source model, subtract only when residual/decode scores improve, retain original evidence, and report unidentifiable overlaps as ambiguous. |
| CW-005 | todo | Add optional coherent receive diversity | Synchronized receiver/antenna inputs can contribute spatial or confidence diversity, but bad alignment or a weak source must never degrade the best single-input held-out result. |
| CW-006 | done | Recognize well-known CW patterns as verification evidence | A recognized prosign/Q-code/contest token (`CQ`, `TEST`, `599`, `5NN`, `TU`, `UP`, and similarly distinctive ones — deliberately excluding short/common ones like `K`/`DE` that noise can hit by chance) appearing in a track's accumulated text is strong independent evidence of genuine Morse, distinct from the aggregate character-confidence score. Motivated directly by real debug-capture data: a real contest track's text contained a legible `TEST` yet never verified because `timingQuality` (see `DSP-002`'s known defect below) stayed under threshold for the track's entire lifetime. Add as an additional verification path (pattern found + minimal supporting evidence → verify) rather than replacing the existing gates, and calibrate/test the token list against real noise captures so it cannot reopen the noise-verification problem `DSP-002`'s plausibility gate closed. Implemented as an additional path: a recognized whole token (`CQ`, `TEST`, `599`, `5NN`, `QRZ`, `TU`, `UP`; short common ones excluded) may satisfy the three character-quality gates, but never the requirement to have decoded enough symbols, and never before the carrier, keyed-edge, cadence and coherence gates have passed, so it cannot verify a silent channel. Measured across all 22 captures: callsign recovery unchanged at 8/9, no false callsign on the four recordings containing none, published tracks identical on every capture but one, where a garbage fragment merged into the OK5OO identity instead of standing as a separate track. The motivating failure no longer reproduces on this corpus -- the tracks whose text reads `TEST` already verify -- so the path currently changes nothing and stands as a safety net; `pattern_verified_tracks` in the verification diagnostics counts how often it is actually needed, and being non-zero on air is the evidence for keeping it. |
| CW-007 | active | Operator role modes: runner and search-and-pounce | The application has no notion of the operator's own role, which weakens stream callsign attribution. Runner: the operator calls and expects answers, needing split working (listening away from the transmit frequency) and pileup reading where many stations answer at once and each repeats only its own call. Search and pounce: the operator hunts stations that are calling, the common case, where the monitored stream is a runner whose own call is the one to label. Motivating measurement: role scoring already picks the transmitting station in nine of eleven realistic exchanges (both reply directions, contest CQ, split runner, pileup caller repeating), but fails where `TU` is ambiguous — it precedes the runner identifying itself (`TU IU0LFQ`) and equally the station just worked (`TU DL1NKB`), both scoring 6, so a run can label the worked station instead of the runner. Knowing the operator's role and own callsign resolves that directly: in search and pounce the monitored stream is the runner, and a call the operator's own station sends is never a stream label. Extends `CALL-001`'s segment-roles item; the own callsign is already configured under Settings → Station and stable text matching it is already detected. Callsign attribution is implemented: Settings → Station selects Monitoring (default, no assumption), Search and pounce, or Running, and the role reaches both decode paths. Hunting, an unambiguous runner context outranks the ambiguous `TU`; running, a repeated bare call outranks one introduced by a `CQ` that belongs to another transmission. The operator's own callsign is removed from candidate scoring rather than only blanked afterwards, so the station actually being heard still gets labelled. Measured on the ambiguous case the item describes: `5NN TU DL1NKB OK5OO UP K` labels DL1NKB -- the station just worked -- with no role, and OK5OO, the split runner, when hunting. Remaining: split working, pileup reading where many stations answer at once, and role-aware behaviour beyond callsign attribution. |
| PERF-001 | active | Build decoder accuracy/resource benchmark gate | Deterministic gates cover zero primary and consensus edits across 8–55 WPM, a compressed-gap callsign repaired by consensus, append-only long-stream truncation, speed acquisition/change, no-CW false characters, clean/30 WPM/weak verified-track acquisition, five interference hard negatives, a maximum 0.20 real-time resource factor, and a conservative decoder-state estimate capped at 256 KiB including bounded turn/cadence state. A generated production-path gate additionally enforces zero CER/WER, exact callsign precision/recall, bounded first provisional/stable and verified-publication latency, and zero false stream/callsign publications per no-CW minute. A disjoint receiver-path holdout adds manual weighting, jitter, drift and fading, gates CER <=0.55, WER <=0.95, exact-call precision 1.0/recall >=0.33, no wrong or hard-negative publications, non-append stability, and <=8 s publication latency; measured CER 0.458, WER 0.786 and recall 0.333 remain explicitly immature. Nine steady-state hypothesis update deep copies are eliminated, with a 70.7% reduction in the isolated inner operation; the outer presentation snapshot remains. An alternating-simplex fixture requires two explicit senders with separate 18/30 WPM cadence ranges, and association suspension must preserve a slow word gap while completing a later sustained absence. Character-model scheduling consumes a trivially-copyable bounded lane snapshot instead of copying every full decoder hypothesis per update. Raw-bank tests cover simultaneous tones, adjacent rejection, and selected-track monitor isolation; the threaded live fixture requires keyed Morse. Receiver reports now distinguish timestamped published-call accuracy from transcript extractability and limit false-publication/callsign rates to half-open episodes inside explicitly reviewed coverage. Extend with calibrated receiver SNR curves, co-channel separation, more reviewed receiver annotations, true platform peak memory, and overload behavior. |
| PERF-002 | active | Profile and optimize the live receive pipeline | Unverified proposals, to be accepted only after before/after measurements on representative audio and multi-MHz SDR inputs: expose per-stage wall time, queue depth, lateness, allocation and drop counters; replace full waterfall-image uploads with a scene-graph texture ring/row update; remove remaining per-frame allocation and QVariant conversion at the DSP/UI boundary; and add bounded overload control that preserves decoder continuity while reducing overview FPS/averaging before receiver samples are dropped. Evaluate vectorized DDC/FFT kernels only if profiling shows those stages dominate. Every optimization must retain decoder quality, full capture-corpus results, platform warning/sanitizer gates, and deterministic output contracts. Instrumented and partly delivered: `cwa_receive_profile` reports per-stage wall time against tracked signal count, and a track that will not be decoded is no longer filtered at all. Measured at the configured maximum of twenty-four tracked signals, the sample stage fell from 21.0 s to 13.6 s per twenty seconds of audio, taking the real-time factor from 1.09 to 0.71, with mean character error unchanged. Remaining and worth roughly an order of magnitude more: take the centre-localisation ratio and the noise reference from the transform the spectrum stage already computes so that one filter cascade runs per decoded track instead of five. Remaining also: waterfall texture ring updates, per-frame allocation at the DSP/UI boundary, and bounded overload control. |
| CALL-001 | active | Extract and rank callsign candidates | A conservative normalized letter+digit token is exposed only after its track is verified, a stable word gap confirms the complete token, and exchange context (`DE`, `CQ`, `TU`, callsign-before-`UP`) or exact repetition supports it. Runner-identifying context outranks a repeated standalone caller; lone call-shaped noise/report fragments remain hidden. Exact published-call precision/recall is now mechanically gated on generated contextual/repeated full calls. The timing layer preserves bounded `?`/gap alternatives and append-only acoustic consensus separately. Completed-turn context can repair only missing boundaries among competitive paths with identical decoded characters. Current-sender attribution additionally requires an explicit two-call handover or calling-station self-identification and abstains on conflicting final evidence. Add per-character alternative alignment, frequency-scoped repetition, richer segment roles/provenance, ranked callsign suggestions, optional external validation, and calibrated precision/recall including portable and near-miss calls on real audio. Raw acoustic text must remain available and a suggestion must never silently replace it. |
| CALL-006 | active | Maintain frequency-anchored decoded observation lifecycle | Tracks retain stable IDs/colors through keyed gaps and silence, update overlays and operator-selected sessions in place, and expire after a profile-configurable hold (Settings → Display, default 30 s, maximum 300 s). Retention preserves identity/text but cannot assert active/keyed state; explicit source/prefix provenance prevents simultaneous nearby tracks from overwriting one observation and carries a bounded 2,048-character transcript exactly once across a genuine replacement. A replacement never inherits a confirmed callsign and must establish station identity from its own acoustic suffix. Concurrent published identities own distinct colors, while a later reacquisition reuses its unoccupied five-minute frequency-color lease. Composed presentation text is never rescored as raw callsign repetition. Leases follow known RX retunes. Linked live radio audio can show checked actual RF using provider state, transverter offset, CW pitch, and sideband direction; add a visible/configurable lost state, viewport-independent RF reacquisition, and callsign-level identity. |
| DSP-004 | todo | Add operational DSP conditioning | Configurable noise blanker, AGC, key-click suppression, mute, and 20–700 Hz monitor filter have replay tests and bypass paths. |
| DSP-006 | active | Replace the hard two-level keying envelope decision | The first bounded separation-prior slice is implemented. The responsive hard-assignment tracker remains for attack, fading, and manual weighting, but a 512 ms allocation-free amplitude history periodically anchors it to robust space/mark modes only when both populations have support, at least 6 dB power separation, and at least 70% explained variation. Plain responsibility-weighted soft/EM assignment remains rejected: it measured 0.3884 against 0.2946 and collapsed at 12 WPM/12 dB because ambiguous samples pulled both levels together. On a portable waveform shared by MSVC, libc++, and libstdc++, the conservative robust anchor improves paired full-surface CER from 0.3070 to 0.2579, reduces wrong surface callsign assertions from 40 to 36, and holds receiver recovery at 8/9 with 0/4 false calls on no-CW recordings. Diagnostics expose separation, explained variation, and anchor acceptance. The nine per-frame hypothesis update copies responsible for the immediate performance debt are now eliminated. Remaining: replace the responsive hard assignment with a fully probabilistic separated-state estimator, calibrate on annotated held-out receiver audio, and preserve these safety/resource gates. Three replacements for the two-level decision have now been measured and rejected, and the reasons are recorded so a fourth attempt is not made blind. Restoring the omitted normalising term of the Gaussian ratio made every seed set worse. The exact Rayleigh and Rician envelope densities were worse at every independence scale swept, with no interior optimum. The evidence chain itself is not miscalibrated: it reduces to the sigmoid of the ratio clamped to the evidence bound, which is the correct posterior for a ratio in nats. The remaining direction is to change what is measured rather than which density is assumed of it, since the levels tracked are smoothed estimates of an already filtered envelope rather than the parameters of any of these models. |
| DSP-005 | todo | Add frequency and I/Q calibration | Manual/automatic correction, reset, diagnostics, and deterministic imbalance fixtures pass. |
| CALL-004 | todo | Add validation, watch, and band-plan policies | Configurable validation levels, allocation/pattern checks, master-call data, watch list, and CW-segment filtering are independently testable. |
| CALL-005 | active | Add optional provider-based callsign prediction and validation | The dependency-free core models immutable raw hypotheses and a bounded local `master.scp`/Call History index. Settings support operator-selected files and an optional managed Super Check Partial `MASTER.SCP` cache. The updater discovers provider metadata, identifies CW Buddy, performs conditional HTTPS checks no more than daily plus explicit on-demand checks, validates bounded data before atomic replacement, and preserves the last valid copy on every failure. SCP data is downloaded at runtime and is not bundled. A separately marked `≈` suggestion appears only when at least two current competitive acoustic paths independently select the same strongest complete callsign and it is within two wildcard-aware substitutions, insertions, or deletions of a completed uncertain span. Ambiguity abstains. An exact list hit carries a `DB` badge; an absent stronger acoustic winner remains `AUDIO` and cannot be displaced by a weaker database candidate. It never changes transcript text, verifies CW, confirms a call, alerts on the operator's call, or influences TX. Remaining: richer character-difference rationale, indexed database-generated candidates outside the refresh path, and bounded authenticated directory lookup only where provider terms permit it. Database absence never penalizes a valid acoustic candidate. |
| CALL-007 | active | Correlate read-only DX-cluster/RBN evidence | Deferred until explicitly resumed. A profile-configured receive-only provider ingests documented DXSpider-style spots or a documented HTTPS activity API, normalizes callsign/frequency/time/mode, and expires stale data. Its optional overlay places a short horizontal frequency marker on the spectrum/waterfall separator with the callsign below on the waterfall side: a square before the call identifies RBN evidence, a circle after it identifies cluster evidence, and both appear when both independent sources agree. Operators can hide all external spot labels or select RBN and cluster sources independently. Source, spotter, age, frequency delta, and confidence remain available beside—not inside—the immutable raw transcript. Spot evidence may rank only an acoustically compatible candidate and cannot by itself verify CW, confirm a callsign, distinguish iso-frequency senders, post a spot, or initiate TX. Use TLS where supported; legacy Telnet requires explicit opt-in, bounded reconnect/rate limits, credential-safe diagnostics, and no commands beyond login/read filtering. Resumed by the repository owner and largely delivered: a bounded expiring spot registry in the dependency-free core, a receive-only HTTPS provider, the overlay on the spectrum/waterfall separator with the square-before and circle-after source marks, an operator settings section, and capped corroboration inside the existing provider budget. Remaining: a DXSpider-style telnet transport alongside the HTTPS one, and honouring the per-source toggles in the overlay as well as at ingestion. |
| CALL-008 | todo | Declutter callsign markers in dense bands | Prevent decoded-stream and external RBN/cluster callsigns from overlapping or hiding the RF location they annotate. Use deterministic collision lanes, bounded label displacement with a retained frequency leader, source/age/confidence priority, viewport-edge handling, and density-based collapsing. Never merge distinct callsigns or frequencies; hover or an explicit reveal action must expose every collapsed candidate and its provenance without changing decoder confidence. |

## P2 — M3 radio and guarded transmission

| ID | Status | Item | Acceptance |
|---|---|---|---|
| CAT-001 | active | Implement Hamlib serial CAT adapter | The first portable provider connects to a local rigctld service in mandatory VFO mode, publishes only complete authoritative RX/TX frequency, mode, VFO, and split polls, and routes capability-gated writes through the shared operating controls. It is restricted to loopback because raw rigctld has no authentication/TLS; remote access requires a locally terminated secure tunnel, and the adapter exposes no PTT/KEY API. CW Buddy therefore shows endpoint/VFO/write controls but no local serial framing, which belongs to rigctld. Remaining: direct model enumeration/configuration, reference-rig hardware acceptance, and optional in-process Hamlib/direct-serial packaging. A future direct-serial editor must use the bounded 1200--115200 baud list and explicit validated data-bit, parity, stop-bit and flow-control choices already represented by the settings model. |
| CAT-002 | active | Implement Windows OmniRig frequency adapter | Settings select Rig 1/2, open native configuration, and poll authoritative RX/standby-TX frequency, active VFO, split, and mode through COM. Writes require the matching advertised writable mask; RX/TX frequency target the resolved VFO property, split uses only explicit capability, and the inactive VFO mode is retained only after that VFO has actually been observed because OmniRig exposes no independent A/B mode properties. An unseen mode stays unknown and is never copied from the other VFO; the provider-neutral operator TX target remains available but unconfirmed. Richer PTT diagnostics and both-radio hardware tests remain. |
| CAT-003 | active | Implement split and transverter frequency domain | Checked integer-Hz RX/TX resolution, independent signed offsets, profile persistence, and a dependency-free provider-neutral state/command contract are implemented. The compact faceplate shows grouped actual-RF RX/TX digits plus authoritative VFO, split, and RX mode. Its independently persisted TX-mode target is restricted to CW/CW-R and is kept distinct from provider observation: every backend applies it only through advertised capability and the UI marks it confirmed only after matching hardware readback. Missing readback never becomes a fabricated hardware value or an unusable operator `?`. Capability-gated RX/TX entry, mode and split controls use checked inverse offsets before provider commands; a separate frequency-only A=B action now sends the exact checked RX actual RF to TX through OmniRig, Hamlib rigctld, or CAT4OM, enables split through capability when needed, fails closed on unknown/invalid state, and never copies RX mode. Live RX retunes re-center tracked signals with sideband-aware mapping. The hardware arming gate now requires and snapshots authoritative TX RF, matching CW/CW-R target-mode readback, and known split state; any subsequent change disarms or emergency-releases. Remaining: logical cross-device VFO A/RX and VFO B/TX presentation for SAT-001, direct serial CAT, setup preview, Doppler tracking, and hardware tests. |
| CAT-004 | active | Implement CAT4OM network frequency provider | Native 1.x handshake, observer/control connection, password proof, pushed state, ownership, capability checks, reconnect, Settings fields, and core protocol tests exist. Operating-panel RX/TX frequency, mode, and split writes require master ownership plus the corresponding advertised command and explicitly target the opaque provider VFO while pushed state remains authoritative. Live service integration tests and a protocol extension for actual transmit/PTT state remain. |
| CAT-005 | todo | Add Icom IC-7300 and IC-7610 radio support | Add both rigs through the provider-neutral CAT boundary (Hamlib/direct CI-V and compatible external providers), with configurable CI-V address/baud, USB audio-link guidance, online/capability discovery, RX/TX frequency, mode and split readback/control, and safe inactive PTT/KEY initialization. Unit tests use protocol mocks; documented hardware acceptance verifies reconnect, VFO selection, split operation, and read/write behavior without unintended transmission before either model is listed as supported. |
| CAT-006 | todo | Populate a data-driven catalog of well-known radios | Enumerate manufacturer, model, backend version, support status, and advertised capabilities from the bundled/selected Hamlib release at runtime, with searchable selection and stable saved model identity. Treat Hamlib NET rigctl, Flrig, OmniRig, CAT4OM, and future control programs as provider backends rather than duplicating static rig lists. Never claim that catalog presence proves every command works: capability-gate frequency/mode/split/PTT/KEY, preserve safe inactive serial lines, show backend status, allow tested per-model overrides, and maintain mocked plus representative hardware acceptance results. |
| CAT-008 | todo | Put every CAT provider behind one adapter interface | Radio control must behave identically through OmniRig, Hamlib, CAT4OM and any direct-serial backend added later; the owner states this as a standing rule. The semantics are already provider-neutral and shared -- `RadioState`, `RadioCapability`, `validate_radio_command`, `resolve_frequencies` and the frequency-plan helpers all live in the dependency-free core -- and Hamlib and CAT4OM are properly encapsulated behind client classes returning a complete `radioState()`, each addressing VFOs explicitly. OmniRig is not: roughly two hundred lines of COM access sit inline in `app_settings.cpp` with no adapter, and about fifteen call sites branch on `frequency_backend_index_ == 0/1/2` rather than calling one interface. Note that there is no adapter to move OmniRig behind: `HamlibRigctldClient` and `Cat4OmClient` are both `final : public QObject` with no shared base, and converge only on `radioState()`, `connected()`, `canWrite()`, `statusText()` and `setRx/TxFrequency`. They diverge everywhere else -- CAT4OM publishes `canSetFrequency/TxFrequency/Mode/Split()` and `requestOwnership()` and takes a VFO on `setSplit`/`setMode`, Hamlib has none of those and splits mode into `setRxMode`/`setTxMode`. So the work is three parts: define a `RadioProvider` interface that does not yet exist, retrofit both existing clients onto it, and write an OmniRig adapter from the inline COM. Resolve the divergence by letting the neutral `RadioState`/`RadioCapability` carry the capability answers rather than per-provider `canX()` methods, since `validate_radio_command` already consumes them; ownership is CAT4OM-specific and belongs in its own construction rather than in the shared interface. Then delete the index branching. Behavioural rules belong in core as pure functions rather than inside any backend -- the COM code compiles only on Windows, so anything left in it is untested on every other platform, and CI is the only check. Worth doing before a direct-serial adapter is added rather than after, because each new provider re-opens every branch. Raised after `omni_rig_active_vfo_is_receive_frequency` fixed the receive frequency following the selected VFO: the fix was correct but had to be verified against the other two backends by reading them rather than by construction. |
| RIG-001 | active | Persist multiple named rig profiles | CAT/keying/framing/poll/display settings are isolated by station profile; full device settings and safe live switching remain. |
| KEY-001 | active | Implement cross-platform RTS/DTR adapter | A direct serial adapter owns an explicit port, rejects unsafe same-line and active-low configurations, initializes KEY then PTT inactive, releases in KEY-then-PTT order, and has deterministic fake-backend coverage. The application controller schedules confirmed immutable Morse plans at fixed or selected-RX-adaptive WPM, cancels synchronously, and operates TUNE through an independent non-extendable worker deadline. A writable checkbox can no longer validate hardware: after explicit radio-disconnected confirmation, a bounded exact-port probe must measure inactive state, RTS→CTS and DTR→DSR separately, and final release before storing configuration-bound evidence. Discovery never toggles unknown ports. Remaining: execute the physical fixture on Windows/macOS/Linux and document/complete end-to-end minimum-power dummy-load acceptance. |
| OPS-001 | todo | Add first-level operating modes | Put named Standard, PileUp Chaser, PileUp Slicer, and Runner workspaces in the left rail. Standard preserves today's neutral behavior. Chaser first identifies and fully decodes the runner, then uses bounded callsign/report decoding on pileup lanes to associate each completed QSO with the selected caller's frequency; while the runner is working another or an ambiguous caller, local TX is inhibited and that slice becomes learning evidence. It requires an observation-only learning phase before TX, configurable time/QSO-count relearning for half-duplex reception, and continuous learning when independent full-duplex RX remains available. Slicer ranks quiet eligible slots from recent occupancy without claiming the runner listens there. Runner models local CQ/listening cycles and callers in simplex or split. Suggestion is the default; automatic VFO B positioning is separately enabled per session, stops on stale/ambiguous evidence or while keyed, uses only provider-neutral checked commands/readback, and has no PTT, KEY, arming, or message-send API. Expose state, confidence, coverage, evidence age, and every retune reason. See `docs/operating-modes.md`. |
| QSO-001 | active | Define declarative workflow/panel schema | A dependency-free validated profile model now separates neutral monitoring, open-ended ordinary/general CW, and individually defined rule-derived contest exchanges, with typed fields, role transitions, field-scoped aliases, and inert macro metadata rather than executable scripts. Remaining profiles include DX pileup, special events, and beacons; application/UI state and runtime trust must keep acoustic, provider-suggested, operator-accepted, and exact-TX-confirmed calls distinct. Suggested/automatic replies require explicit per-profile enablement, exact-call/context confirmation, armed TX, cancellable preview, maximum-key-down, and emergency release. |
| QSO-002 | active | Implement operator-confirmed QSO workflow | The receiver card can select only an exact decoded callsign; the operator must retype it, then prepare and retype normalized own-call, editable report/exchange, one of four profile quick macros, or free text before a standard Morse plan exists. Auto-QSO recognizes exact own-call/listening cues, while a near own-call must repeat twice, and both paths create only a visible proposal. The controller connects the confirmed plan to measured-loopback hardware only after provider-confirmed station state, exposes physical readiness, supports synchronous cancellation, gives emergency release permanent placement, and reports worker-clock elapsed/remaining time and progress for messages and TUNE. Remaining: declarative per-conversation macro expansion, cancellation grace countdown where required, and end-to-end physical dummy-load acceptance. |
| QSO-004 | active | Add split-pileup operating view and TX-slot assistance | A selected runner carrying checked absolute RF can be explicitly anchored to the configured CW reference tone through the provider-neutral RX-write route; TX, split and mode remain unchanged, so an ordinary UP pileup is presented to its right. Once authoritative RX/TX VFO, mode and split control plus selected-track monitoring are available, rank genuinely quiet split frequencies and show an operator-confirmed TX suggestion. OPS-001 owns the later learning lifecycle and explicit per-session option to place VFO B automatically. Decoder ambiguity, stale reports, occupied slots, another caller's active exchange, or missing coverage must abstain; assistance never keys TX. |
| QSO-003 | active | Notify when the operator's own callsign is decoded | An exact normalized match in stable decoded text is highlighted, labels **YOUR CALL HEARD**, and flashes the open decoder card for five bounded pulses. Add configurable visual behavior plus opt-in audio/remote notifications and repeat/rate limiting. An optional closing macro may be queued only when the QSO context matches, auto-reply is explicitly enabled and armed, all TX guards pass, and the operator can cancel before transmission. |
| QSO-005 | active | Model alternating operators on one simplex carrier | A completed, structurally plausible `CALL1 DE CALL2` handover records both distinct participants and labels the retained decoder card as one QSO. Sustained silence or explicit end-of-input creates bounded turns; shorter association loss cannot split a slow word gap. Explicit two-call/calling-station evidence may identify the sender; ambiguity abstains. Up to eight identified senders retain cadence summaries, and a supported prior may nudge only a compatible timing neighborhood. An exact-run timing fingerprint is retained with its completed turn but remains deliberately independent of sender attribution. Remaining: calibrate cross-turn fingerprint association with aligned carrier evidence, operator-confirmed attribution, richer provenance, truly independent per-turn timing state, wider ragchew/contest/pileup validation, and co-channel separation. |

## P2 — M4 logging and SDR

| ID | Status | Item | Acceptance |
|---|---|---|---|
| LOG-001 | todo | Implement durable logging outbox | Records survive restart and retry state is visible. |
| LOG-002 | todo | Implement Log4OM 2 UDP ADIF sink | A test QSO is accepted by configurable Log4OM inbound ADIF service. |
| LOG-003 | active | Maintain ADIF conformance readiness | ADIF 3.1.7 satellite/split fields, exact frequency calculation, full band mapping, and policy exist; validated ADI/ADX import/export, official pinned fixtures, independent parser, and release report remain. |
| LOG-004 | active | Resolve station equipment by actual-RF band | Ordered ADIF-band rules and `MY_RIG`/`MY_ANTENNA` cross-band serialization are tested; profile rule editor, persistence, overlap diagnostics, and logger acceptance remain. |
| CAT-007 | active | Point the transmit frequency on radios with no writable TX VFO | Ctrl+left-click sets the transmit frequency through a provider property for the TX VFO, and is refused when the radio publishes none. Many rigs -- the FT-450D among them -- have no CAT command for the transmit VFO at all: the operator sets it by selecting VFO B, writing the frequency, and selecting VFO A again. Doing that from software is a stateful sequence over somebody's transceiver, and a failure partway leaves the rig receiving on the wrong VFO, so it must be designed rather than bolted on: an explicit capability describing the swap, a readback confirming each step, restoration of the original VFO on any failure, and a refusal to start the sequence at all while transmitting. A first, much simpler case is now handled: a radio publishing no VFO identity at all was treated as having no transmit VFO, so the control was refused even where the parameter mask said the second VFO was writable -- the FT-450D's situation, and it has the commands for both VFOs. With split on, A receives and B transmits by convention, claimed only where the mask agrees. Remaining is the genuinely hard case: a radio with no writable per-VFO frequency property, where setting transmit means selecting the other VFO, writing, and selecting back. Until then the gesture states that the radio does not offer the control instead of doing nothing silently. |
| SAT-001 | todo | Add complete satellite/transverter operating profiles | A named profile stores independent signed RX/downlink and TX/uplink transverter offsets, radio dial versus actual-RF presentation, radio/control backend, antenna and converter-chain descriptions, and optional satellite defaults suitable for full-duplex operation such as QO-100. RX and TX bindings are independent: a profile can receive from one radio, SDR, audio device or audio channel while transmitting through another radio/control provider and, where applicable, another audio device/channel; it must not impose a shared-device or simplex assumption. The faceplate presents these as logical `VFO A / RX` and `VFO B / TX` endpoints even when A and B belong to two different physical devices, and names each bound device rather than implying both VFOs live in one rig. QSO logging resolves the profile at contact time and emits the applicable ADIF fields: exact `FREQ`/`FREQ_RX`, `BAND`/`BAND_RX`, `PROP_MODE=SAT`, `SAT_NAME`, `SAT_MODE`, and local-station `MY_RIG`/`MY_ANTENNA`; it never puts local equipment into contacted-station `RIG`. The editor validates frequency arithmetic, ADIF dependencies/enumerations, overlapping equipment rules, missing satellite identity, device/channel ownership, and unsafe or ambiguous RX/TX mappings before CAT, audio, TX, or logging use. Cross-link implementation with CAT-003, LOG-003, LOG-004, and the audio/SDR adapters rather than creating separate frequency or ADIF models. |
| INT-001 | active | Add read-only DX-cluster spot service | Verified calls can be served with CQ-only filtering, authentication option, bounded clients, and loopback-safe defaults. The receive-only ingestion half is delivered through the spot provider and registry; serving spots outward remains. |
| INT-003 | done | Ingest spots from a real feed | The HTTPS provider accepts a document shape (`callsign`/`frequencyHz`/`time`) that no public service emits, so it currently has no endpoint it can parse. Probed 2026-09-11: DXHeat (`https://dxheat.com/source/spots/`) sends `DXCall`/`Frequency`/`Spotter`/`Time`+`Date`; POTA (`https://api.pota.app/spot/activator`) sends `activator`/`spotTime`/`source`, including RBN-relayed CW spots carrying SNR and WPM; DXSummit answers over plain HTTP only, with no HTTPS host, so the provider refuses it; SOTA reports frequency in MHz at 0.1 resolution, far coarser than the marker tolerance. Needs a per-source adapter, and a source list held in `dictionaries/` rather than compiled in, with a custom entry and a reachability check whose result is cached rather than probed on every start. |
| INT-004 | active | Add a DX cluster telnet client | The cluster network is telnet; HTTPS reaches only aggregators. A telnet client is the only route to DXFun, VE7CC, W3LPL, K3LR, OH2AQ and, importantly, live RBN (`telnet.reversebeacon.net:7000`), which has no public JSON API at all and which the existing reverse-beacon setting therefore cannot currently feed. Unlike the HTTPS provider this cannot be strictly send-nothing: a cluster login requires sending a callsign, in clear, over an unencrypted socket, and that has to be stated to the operator rather than buried. Clusters are a shared volunteer resource, so the client must hold one connection, back off on failure instead of reconnecting in a loop, and never issue commands the operator did not ask for. Spots parse into the same `CwSpot` values the registry already holds, and the existing prohibition stands unchanged: a spot corroborates, and can never supply or rewrite a decoded callsign. Delivered: the client, the operator-editable server list, band filtering on the server where the syntax was verified and on arrival always, and the settings section. Remaining: verify a band-filter syntax for CC Cluster and AR-Cluster nodes against a live server, A cluster login may now carry an SSID: the station callsign is composed with an operator-chosen 0-99 suffix, validated in the login path alone so `CallsignPolicy` -- which the decoder shares to judge callsigns on the air -- is unchanged. |
| INT-002 | todo | Add UDP spectrum export | Versioned timestamped spectrum frames interoperate with a documented logger/contest consumer fixture. |
| REC-001 | active | Add interoperable audio/IQ recorder | A dependency-free PCM16 WAV writer exists (round-trip tested against the existing WAV reader) and is used by the OBS-003 debug capture; RF64, IQ, metadata, rotation, looping, and a dedicated operator-facing recorder UI (independent of debug capture) remain. |
| SDR-001 | active | Add SoapySDR stream adapter | The RX-only adapter and official-package integration enumerate modules/devices, query standard sample-rate/RF-bandwidth/antenna/gain capabilities, choose supported values, read CF32 channel 0, preserve absolute RF, and produce validated timestamped IQ blocks with distinct telemetry. Discovery groups operating variants by stable physical receiver identity while retaining the selected opaque driver mode. A bounded-rate overview retains the complete acquired passband; a separate shared DDC, anti-alias filter and decimator feeds only the configured 2–24 kHz CW decoder window instead of processing every track at the hardware rate. Wheel zoom, middle-button pan, full-span reset, right-click window recenter/probe, CTRL+Right-drag decoder-window selection, waterfall-edge SDR stepping, and the operational SDR faceplate make the wide view operable without returning to Settings. Center-only changes retune the active RX stream without reopening it. Optional bidirectional radio synchronization uses provider-neutral command/observation separation and a bounded signed SDR LO offset without touching TX. Windows/macOS packages carry the redistributable runtime; Linux packages bundle it or declare the module dependency. Every staged package must load the RTL factory without hardware. Remaining: expose complete telemetry and overload state in Diagnostics, add only capability-backed driver-specific controls, capture IQ under REC-001, measure wide-passband CPU limits, and complete physical-device acceptance. |
| SDR-002 | active | Validate RTL-SDR | RTL-SDR is supported through the common SoapySDR RX boundary without another SDR application. Official packages now provide the SoapySDR/SoapyRTLSDR/librtlsdr/libusb closure with license provenance and module-load tests. Remaining: pass live RTL-SDR acceptance on Windows, macOS, and Linux, including USB-driver guidance, frequency accuracy, sustained overflow behavior, hot-unplug, reconnect, and selected-stream monitoring. |
| SDR-003 | active | Validate SDRplay 3 | SDRplay uses the common SoapySDR RX boundary. Windows packages now carry a pinned MIT-licensed SoapySDRPlay3 bridge and locate the registered external SDRplay Hardware API 3.15 runtime; the proprietary API/service/driver remain operator-installed and are never redistributed. macOS and Linux still require a compatible external SoapySDRPlay3 module. Discovery reports module dependency failures rather than silently hiding them. One physical RSPduo/serial is shown once, with Single Tuner as the recommended one-channel operating mode and Dual Tuner/Master variants available separately as advanced configurations; CW Buddy currently consumes channel 0 only. Its exposed antenna selector chooses the tuner/input supported by that mode. Supported effective low sample rates delegate hardware decimation to the SDRplay driver. Remaining: pass live RSP acceptance on Windows, macOS, and Linux, including live retuning, operating-mode/input matrix, service/version mismatch, minimum sample rate, overload, reconnect, and selected-stream monitoring. |
| SDR-005 | todo | Bridge software-defined receivers that do not present as a driver | Some vendor receiver applications own the hardware themselves and expose no enumerable driver, so the device cannot be discovered the way a directly supported receiver is. Reaching one requires a plugin installed into that application plus a local IQ and control transport, which is a different integration shape from the driver-enumeration path everything else uses. Scope the transport, the plugin boundary, and whether the vendor's terms permit distribution before committing to it. Requested by the repository owner after confirming the hardware itself works under its vendor application; note that both cannot own the same receiver simultaneously. |
| SDR-004 | todo | Add Analog Devices PlutoSDR receive support | Discover local or remote ADALM-Pluto contexts through the official cross-platform libiio API, select the RX streaming channel, configure center frequency/sample rate/RF bandwidth/gain without assuming TX ownership, and feed timestamped complex-IQ blocks with overflow and reconnect diagnostics into the shared SDR source boundary. Package or locate libiio per platform with license/runtime validation; test against a mock IIO context and a documented USB/IP hardware fixture before declaring support. A future explicitly armed Pluto TX path is separate and must pass the normal transmit safety gates. |
| NET-001 | todo | Implement cached network receiver directory | Normalized entries filter by band/frequency, location, protocol, and availability; provider terms and refresh limits are documented. |
| NET-002 | todo | Implement KiwiSDR WebSocket sample source | Receives permitted audio/IQ/waterfall with identity, capacity handling, sequence telemetry, and bounded reconnect. |
| NET-003 | todo | Add browser/virtual-audio receiver handoff | Browser-only receiver entries tune via supported URL parameters and guide audio-device selection without private protocol use. |
| NET-004 | todo | Evaluate OpenWebRX adapter | Implement only against a documented stable interface with replayable protocol fixtures. |
| NET-005 | todo | Guard remote-RX/local-TX frequency linking | Local rig retune requires explicit action and confirmation; network sources can never acquire TX ownership. |

## P3 — release engineering

| ID | Status | Item | Acceptance |
|---|---|---|---|
| PKG-001 | active | Produce signed Win64 installer | Hosted WiX/MSI generation, stable major-upgrade identity, numeric build revisions, branded executable/product icon, `CW Buddy` Start-menu program group, desktop shortcut, and stable download naming are implemented. The finish page offers to launch the app, leaving the option unchecked on clean installs and selecting it for interactive upgrades. Running-process closure uses WiX's standard execute sequence with a bounded wait; CI rejects UI-sequence invocation or the wrong WiX binary and verifies the close contract, conditional default, and launch target through PowerShell 7-compatible reflected COM access. Clean Windows 11 install/upgrade/repair/uninstall runtime tests, migration from the out-of-support WiX v3 toolchain, Authenticode signing, and signed update metadata remain. |
| PKG-002 | active | Produce macOS bundle and Debian/Ubuntu package | Hosted builds deploy Qt/QML runtime files and publish portable Sonoma 14+ Apple silicon/Intel artifacts plus a CPack `.deb`; CI verifies required macOS plist identity/version fields, the complete bundle resource seal, the Mach-O 14.0 deployment target, and stable filenames; validate clean Sonoma and supported Debian/Ubuntu installs, then add Developer ID signing and notarization before release. |
| PKG-003 | todo | Publish signed Debian/Ubuntu APT repository | Signed Release/InRelease metadata, protected key rotation, version promotion, retention, and documented repository enrollment pass clean-machine tests. |
| PKG-004 | active | Add application update checking and guided install | A background check (disableable, ~4 s after startup) and Settings → About **Check for updates** compare the running version against the published manifest. **Download update** fetches this platform's artifact and verifies SHA-256 before saving. Continuous publication replaces binaries/checksums before publishing the manifest pointer, while the client retries transient 404/timeout/server failures with bounded backoff. In both the startup notice and Settings → About, verification status remains visible; once verified, **Open Installer** and the platform-specific reveal action replace the download control in place and hand the file to the OS rather than installing silently. Remaining: silent self-install-and-relaunch is intentionally deferred until Windows Authenticode and macOS notarization signing land (`PKG-001`, `PKG-002`). |
| DOC-001 | active | Maintain operator and hardware manuals | A user-manual index plus setup, settings, hosted-build, Debian/Ubuntu, and CAT4OM guides exist; every implementation change is CI-gated on manual/changelog/backlog updates; safe keying, workflows, diagnostics, and compatibility manuals remain. |
| DOC-002 | done | Render documentation diagrams instead of ASCII art | Every diagram under `docs/` is currently ASCII art in a fenced block. Replace them with rendered vector figures (source checked in and generated at build or docs time, so a diagram is never a binary blob nobody can edit), covering the signal path, the decoder stages, verification state transitions, and the UI layout maps. Keep the rendered output legible in both light and dark viewers, and keep a text alternative for accessibility and for terminal readers. Done for the five existing diagrams: the layer stack, the realtime data flow and the transmit-safety states in `docs/architecture.md`, the decoder pipeline in `docs/decoder-strategy.md`, and the renderer node tree in `docs/decisions/0001`. All are Mermaid, which keeps the source in the document, diffable and editable, renders as vectors in GitHub and the common documentation viewers, and needs no build step or checked-in binary. Each carries a prose description of the same content immediately below it, so a terminal reader or screen reader loses nothing. The operator guide also gained the UI layout map it lacked. |

## P2 — secure remote operation

| ID | Status | Item | Acceptance |
|---|---|---|---|
| REM-001 | active | Implement remote roles and station-wide lease domain | Role/message contracts and bounded exclusive lease manager pass dependency-free expiry tests. Extend the lease to cover every coupled RX/TX device and shared route in one station profile, while multiple authenticated observers remain concurrent; persist lease policy, never active ownership. |
| OBS-004 | active | Offer a live diagnostics stream for remote troubleshooting | Delivered: an emit-only line-delimited JSON stream of the capture's own records, once a second, on operator-chosen addresses selected from those the machine can bind; a Network settings tab; an indicator while listening; throughput counters describing whether the application is keeping up; a session log file behind `--log-file`/`CWA_LOG_FILE`; and a waterfall rendering switch so a station serving diagnostics need not draw one. The stream never reads from a client, because this process holds transmit and an input path here would be a second, weaker way to reach a radio; a peer that sends is disconnected. Remaining, and stated in the settings page rather than implied away: there is no per-client authentication, since authenticating a client means reading what it sends. The token gates which addresses may be bound, so a routable address cannot be offered without one, but anything that can reach a bound address receives the stream. Proper access control belongs to REM-003/REM-004 -- TLS 1.3 and per-client certificates -- as an observer subscription rather than a second remote surface with a weaker security model. Since delivered: per-client token authentication, read as one bounded line and compared in constant time, and a peer allow-list of addresses or CIDR subnets checked before a byte is exchanged. Remaining: the stream is unencrypted, so the token crosses in clear, and one shared secret cannot revoke a single reader. Those belong to REM-003/REM-004 with TLS 1.3 and per-client certificates. Until then the honest guidance is a trusted network or a tunnel, never a port forward. |
| OBS-005 | todo | Carry receive audio to a remote observer over UDP | Requested by the repository owner: stream the station's audio alongside the diagnostics records so a remote operator can save it or listen to it. Authenticate on the existing diagnostics TCP connection, then start the audio on request over UDP -- the media plane separate from the control plane, which is how RTSP and SIP are built and is the right shape here. UDP is correct for this: losing a packet of audio is better than delaying the rest of it, and a retransmitted sample is worthless by the time it arrives. Three things this must settle before it is written, each a direct consequence of what has already been learned in this file. First, it breaks the diagnostics service's stated safety argument -- that no byte from a peer can ever select an action -- because starting a stream *is* a peer selecting an action. That is acceptable only if the request is a single bounded message drawn from a closed set of options that cannot reach the radio, the settings or the decoder, and only if the header stops claiming otherwise and says what is actually true. The claim in the source is a promise to the owner, not decoration. Second, UDP has no backpressure, which is precisely the fault that grew the application to 668 MB and had to be killed. A sender that emits at the rate audio is produced, to a receiver that may be gone, is the same mistake with the queue moved into the socket. Bound it at the sender by construction, drop rather than buffer, and count what was dropped. Third, audio leaving the station is a larger disclosure than telemetry: it carries every signal in the passband, not just what this application decoded. It needs the same explicit, visible consent as binding a routable address, an indicator for as long as it is sending, and it must never start merely because a peer asked. Encryption stays where it already is: unencrypted on a trusted network or a tunnel until REM-003 brings TLS 1.3 and per-client certificates, at which point this becomes a subscription on that transport rather than a fourth remote surface. |
| CW-006 | todo | Name a station the decoder recovers correctly only once | On a recorded DX pileup the one clearly identifying station sends its call about eight times and the decoder recovers it exactly once, in both parallel texts. `CallsignPolicy::best_complete_in_text` never offers that reading: its acceptance floor is 3 and a callsign-shaped token earns exactly 3 simply for standing third after a `CQ`, so a one-element misdecode in that position outranks the correct call standing alone with no adjacent context word. Raising the floor to 4 measures 26 correct and 5 wrong on the decoder surface benchmark against 26 and 6 today, but does not reach this case, because the competing reading there sits immediately after a `CQ` and earns 5 honestly. Candidate: treat agreement between the literal and the lattice-refined text at token level as evidence in its own right -- the correct call is the only complete callsign both paths produced. The stream is published unnamed until then, which is the correct answer. |
| AUD-002 | todo | Refuse an ambiguous SDR device the way the audio input now does | Reviewed 2026-09-13 against the audio-input identifier fix. The SDR resolves on `driver:serial` and re-derives its variant id from live enumeration, so a reboot does not break it. Two gaps remain: `sdr/deviceId` embeds the enumeration index for driver families publishing no `mode` kwarg, which bites only a path opening from the persisted id before a discovery pass; and a driver reporting no serial degrades to `driver:label`, so two identical serial-less dongles collapse into one physical row whose modes are told apart only by index and both read "Default". That is the same ambiguity the audio path now refuses to guess at, and it should refuse there too. Rejected here and not to be added without a genuinely stable per-device key: remembering which of several same-named devices was chosen by its position among them -- that ordinal carries information only when the names collide, and there it records enumeration order rather than identity, produced by the same subsystem whose identifiers just churned. |
| UI-011 | todo | Hold an identified stream in the waterfall longer than an unidentified one | Requested by the repository owner: a stream that has decoded and whose callsign is verified should persist about twice as long, keeping its region active, while a poorly decoded or unverified stream keeps the standard time. Attempted as a multiplier on `decoded_track_retention_seconds` and reverted, because the retention value is load-bearing for more than expiry. Two things were learned and should shape the real attempt. First, verification decays while a station is not sending, so a rule keyed on the present state gives the longer hold to nobody: by the time it matters the track has already fallen into the unverified branch and its 0.75 s. Latching an "was identified" flag fixes that but is not sufficient. Second, making that flag take precedence changed when a track may be replaced, which broke the genuine-replacement and inherited-prefix behaviour that `core_tests` guards -- an identified predecessor that lives twice as long is no longer displaced by its successor, so identity inheritance silently stops working. So the design has to say what happens to replacement, not only to expiry: whether an identified track that is being displaced by a stronger signal on the same frequency should yield, and whether the longer hold belongs in expiry at all rather than in the display's own retained-observation path, which already keeps a faded marker after a track has gone. Acceptance: the longer hold is demonstrated by a test that fails without it, and the existing replacement and inheritance assertions still pass. |
| REM-002 | todo | Define and generate versioned wire schema | Implement the envelopes, epochs, sequences, idempotency, limits and compatibility rules in the secure remote-operation specification; tests reject unknown major versions and preserve only explicitly compatible optional fields. |
| REM-003 | todo | Implement mutually authenticated secure WebSocket station/client adapters | Require TLS 1.3, valid per-client certificates, station pinning and encrypted control/event/media outside loopback tests; size/rate/connection limits and malformed-frame tests pass. |
| REM-004 | todo | Implement local pairing, roles, revocation, and key storage | Physical/local approval, unique client certificates, observer/operator/admin permissions, station pin rotation, immediate revocation and OS-backed credential lifecycle pass integration tests on every OS. |
| REM-005 | todo | Add full snapshot/delta reconnect protocol | Epoch/sequence gaps trigger resnapshot; control, arming, confirmation, and queued TX never resume implicitly. |
| REM-006 | todo | Stream Opus receive audio | Jitter buffer exposes latency/loss; audio degrades independently of control and decoder events. |
| REM-007 | todo | Add remote spectrum/event/IQ subscriptions | Bandwidth profiles are enforced with bounded queues; IQ is opt-in and capacity-controlled. |
| REM-008 | todo | Implement station-local idempotent CW scheduler | Complete messages retain timing under network jitter; duplicates, disconnects, lease loss, and limits are safe. |
| REM-009 | todo | Add remote security audit and fault-injection suite | Certificate/pin failures, unauthorized roles, resource exhaustion, loss, delay, reorder, duplicate, reconnect, crashes, lease takeover and device removal never disclose unauthorized state, duplicate TX, starve control, or leave lines asserted. |
| REM-010 | todo | Document VPN/reverse-proxy deployments | LAN/VPN setup is supported; public raw port forwarding is explicitly rejected. |
