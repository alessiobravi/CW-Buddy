# Changelog

All notable changes to CW Buddy are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and releases will use
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- A configured SDR LO offset is no longer applied twice to every frequency a
  direct-SDR session reports. The offset tunes the hardware to `rx + offset` and
  leaves the decode window at `rx`; the settings clamped it to one margin below
  Nyquist while the publisher that checks the window against the acquired
  passband measured its reach with a margin a kilohertz larger. Every offset
  large enough to have been clamped therefore arrived judged out of reach, and
  the window was moved onto the acquisition centre -- which is the operator's
  frequency plus the offset. Decoded streams and the spectrum axis then read one
  whole LO offset high: 83 kHz at 192 kS/s, 112 kHz at 250 kS/s, 987 kHz at
  2 MS/s, permanently, while the settings pane went on displaying the frequency
  it was configured with, because the correction was made downstream of it. The
  margin is now one definition both sites read. A window that merely overhangs it
  -- a decode region dragged wider than the passband leaves room for -- is pulled
  just inside rather than discarded; one that is not in the acquired passband at
  all, such as a stored 20 m default against a receiver on 40 m, still falls back
  to the capture centre, because nothing about it can be salvaged.

- Tuning across a band no longer walks the waterfall history out from under its
  own frequency scale. Retained rows are slid by a whole number of bins on each
  retune, and the part of the step that would not fit was discarded -- not as
  noise, but as the same fraction in the same direction every time. At 2 MS/s
  over 16384 bins a 1 kHz click leaves about 23 Hz behind, so tuning across a
  band accumulated kilohertz of skew in the history while the live top row and
  the axis stayed correct, parking a recognisable signature where no signal is.
  The remainder is now carried and spent as soon as it amounts to a whole bin,
  holding the history within half a bin of the dial however far an operator
  tunes.

- A retune no longer labels samples with a frequency they were not received on.
  Moving the receiver does not empty what the device has already queued, and
  those samples were handed out stamped with the new centre frequency. The
  display smear that causes is momentary; the consequence is not, because a track
  discovered inside such a block fixes its identity origin at that frequency and
  its reported frequency stays clamped to it for the rest of the track's life --
  so a band jump mislabelled a station by the whole jump, permanently. The queue
  is now dropped across a retune, above the backend boundary so every receive
  provider behaves the same way, and the gap is marked as a discontinuity so
  filter state is rebuilt rather than joined across it.

- An SDR LO offset set while Radio Sync is off now takes effect at once instead
  of waiting for the next click of the dial, so an operator tuning by hand is no
  longer left looking at the LO spur they had just asked to move. An RX
  transverter offset changed while Radio Sync is on rebuilds the window
  immediately rather than at the next CAT poll. The acquisition centre and the
  decode window are also persisted together when the faceplate VFO moves them,
  because only their separation is meaningful.

- Every frequency a direct-SDR session reports is now read off the decode window
  the channelizer is actually carving out, rather than off the one last asked
  for. The two are separate values on purpose -- a window the channelizer refuses
  stays unapplied so a later block can retry -- and detection was taking its bins
  from the applied window while addressing them by the requested one's
  coordinates. Where the two differ the slice collapses onto a sliver at one
  edge: detection is handed filtered noise and the decoder falls silent while the
  overview spectrum keeps painting normally, which is the fault an operator
  cannot diagnose from the display. The region monitor had the same split, and
  its cost is audible rather than silent -- the single-sideband shift is half the
  region's width, so demodulating at the requested width moves every station to a
  different pitch while the decoder, reading the same samples, still reports it
  correctly.

- A decoded stream's RF is no longer read up to one spectrum bin high. The
  channel bank takes the two frequencies it is given as the centres of the first
  and last bin; the direct-IQ path passed the upper edge of the band instead,
  stretching the grid by one bin across the decode window. Every track read high
  in proportion to its distance above the window's lower edge -- about 7 Hz at
  the top of a 24 kHz window. The audio path had always used the other
  convention and was unaffected.

- The live diagnostics record states every reference a reported frequency is
  built from, so a station reporting signals at the wrong frequency can be
  localised from one record. `decoderWindow` gains the slice detection was
  actually read at; a new `acquisition` block carries the centre and sample rate
  the receiver was asked for beside the ones it reports on the samples it
  delivers. A receiver that lands somewhere other than where it was sent moves
  every track and every axis label by the difference while both decode-window
  readings stay exactly as configured, and nothing recorded that until now.

### Changed

- A station whose callsign both decode paths read is named from that agreement
  alone. The literal and the lattice-refined texts are independent readings of
  one transmission, so a complete callsign on an allocated prefix standing as a
  whole token in both is evidence about the signal, and it now outranks the
  positional rules -- a call beside a `CQ`, or two or three words after one --
  which only guess at where a call sits in a transmission. Where two such agreed
  callsigns compete the one read earliest names the stream: the decoded text is
  append-only, so that is the one the two paths have agreed on for longest while
  the other has been agreed on for an instant. An explicit `CALL1 DE CALL2`
  handover still outranks both.

  That ordering rule is not cosmetic. The refined path is derived from the same
  acoustic evidence, so any stretch refinement left alone agrees trivially, and
  on a recorded pileup agreement alone ties three candidates at equal score. On
  that capture the one clearly identifying station is now named `EH3ST` from
  t=67.9 s to the end of the 133 seconds; before this it was never named at all,
  because the correct reading was never offered.

- The evidence a decoded token must carry before it can name a stream went from
  three to four. Three was exactly what a callsign-shaped token earned for
  standing third after a `CQ`, the weakest positional rule there is; admitting
  that on its own costs a wrong callsign and gains none -- 26 correct and 5 wrong
  on the decoder surface benchmark against 26 and 6. A call two words after a
  `CQ`, or one the same text read twice, still qualifies.

- When only one of the two decode paths produced a text, that path must read a
  callsign twice before it names a stream rather than once. With nothing to agree
  with, a lone token vouched for only by a neighbouring `CQ` is the weakest
  evidence in the system, and it arrives exactly when the other reading is
  unavailable to contradict it. On the pileup capture the literal path's timing
  gate closed for the last second and a half and a single refined `E5Q` beside a
  `CQ` took the stream away from the name both paths had agreed on for the
  preceding minute.

### Added

- The live diagnostics record states what the receive worker is listening to and
  decoding, rather than leaving both to be inferred from the settings.
  `regionAudio` now carries all three demands its `wanted` flag is the union of
  -- the monitor mode as the worker itself holds it, whether a remote observer is
  subscribed, and whether a capture is running -- so a record showing no region
  audio says which one is missing instead of only that none is present. A new
  `decoderWindow` block reports the window the channelizer actually accepted
  beside the one last requested: a window it refuses is deliberately left
  unapplied so a later block can retry, and until now that was indistinguishable
  from a window in force, because settings only ever hold the request. Two faults
  this week were diagnosed by asking an operator what Settings showed, which
  answers a different question.

### Changed

- A monitored stream is published without a station name while the only two
  callsigns read from it disagree and neither has been read twice. Two single
  readings that contradict each other are evidence that the copy is not good
  enough to read a call out of, not evidence for whichever was read last; the
  name appears as soon as one of them is confirmed. On a recorded DX pileup this
  is the difference between labelling the one clearly identifying station with a
  callsign that was not its own and leaving it unlabelled.

- The one-keyer-or-several test now reads the decoder's acoustic cadence fit
  alongside the keying-speed ratio measured at two filter widths, and either
  measure refusing a channel withholds its text. The two are complementary
  rather than redundant, which is why both are kept: on a recorded pileup the
  ratio produced no speed estimate at all for 41 of 48 published carriers, while
  the cadence fit refused 42 of 48 and separated the two resolvable stations
  cleanly -- and on a synthetic two-carrier fixture the ratio refuses where the
  cadence fit does not. The cadence reading applies only where at least two other
  carriers share the neighbourhood, so a lone station with poor copy keeps its
  transcript.

- Audio inputs the operating system describes with identical words are numbered
  #1, #2 and so on in Settings and in the setup wizard, with a note explaining
  the numbering shown only while such a collision exists. When the saved
  identifier is gone and several inputs answer to the saved name, live audio
  refuses to start and names the numbered candidates rather than choosing
  between them: nothing distinguishes those devices any more, so a choice would
  be a guess, and the wrong one would put a different radio on the decoder with
  nothing on screen to say so.

### Added

- A recorded SigMF capture can now be scored, not only played back.
  `cwa_capture_replay` accepts either half of a `.sigmf-data`/`.sigmf-meta` pair
  alongside the WAV clips it already read, and routes the wide complex block
  through the same two-stage chain the live receiver uses -- overview transform,
  subband decimator, decoder transform -- before the channel bank sees it, so a
  replayed score is comparable with what the operator saw. A capture block fed
  straight to the decoder is not audio: every filter and timing constant in the
  bank is sized for the decode region rather than for the hardware passband.

  The decoder window is chosen per capture, defaulting to the recording's own
  centre frequency and overridable, because the centre is frequently not where
  the signals are: a receiver watching a pileup 16 kHz down from where it was
  tuned records a capture whose centre holds nothing. A window whose slice
  leaves the acquired passband is refused up front, naming the frequencies that
  do not fit -- the decimator's own answer is to accept no samples while the
  overview spectrum keeps painting normally, which reads as a decoder that
  silently stopped and is not one.

  The report names the sample rate, every capture segment, the recorded stop
  reason and whether the file ends mid-sample, so a short recording an operator
  ended is no longer indistinguishable from one that failed. An annotation
  sidecar's digest covers both halves of the pair, since the sidecar carries the
  rate and segment centres that decide what a track's absolute RF means.


### Fixed

- A decode region dragged on the spectrum is applied as the rectangle that was
  drawn, not as the coordinate the release event reports. The release handler
  overwrote the pointer position it had tracked through the drag with the one the
  release carried, and a release reporting the press position collapses the
  gesture to a click: the width silently fell back to the one already in force
  and the region centred on the release point, so the box just drawn was
  discarded while the region still appeared to move to where the button came up.
  Nothing is lost by ignoring that coordinate -- a position the release can
  report that the drag never tracked is a position no rectangle was ever drawn
  at.

- Starting a receiver restates the monitor selection to the receive worker,
  alongside the spectrum configuration and the decode window it already
  restated. Nothing reconciled the two copies of the monitor at a start, which is
  the one moment they can part company: a start clears the region demodulator and
  its counters, so the demand has to be repeated on the other side of it.
  Previously the only thing that ever set a monitor mode on that worker was the
  operator pressing a listen control after reception had already begun.

- Live audio finds its input again after the operating system reissues the
  device identifier. A restart, a driver reload or a different USB port can all
  change the identifier while the interface stays plugged into the same radio,
  and that identifier was the only thing remembered about the operator's choice,
  so reception refused to start with "The selected audio input is unavailable".
  The device's own description is now stored beside the identifier and used when
  the identifier is gone: an input that uniquely carries the saved name is
  adopted, the recovered identifier is written straight back so the next start
  matches on the identifier again, and the status line reports that the
  application followed the name rather than the hardware.

- The application no longer freezes for seconds at a time when several stations
  are decoding. Every publish of the decoded-channel model re-derived two things
  for every visible stream on the thread that draws: whether the operator's own
  callsign appears in the transcript, and the advisory callsign suggestion. The
  first uppercased a copy of the whole cumulative transcript and split it, so
  its cost grew for as long as the application stayed open. Measured over 24
  streams at 23 publishes a second with the transcripts an hour on a busy band
  produces, the two cost about two seconds of interface time for every second of
  reception -- which is why an empty band was responsive and a full one was not.

  Both are now derived only when the evidence they are drawn from changes, and
  the transcript scan no longer copies, compiles or allocates. The same publishes
  cost between 2 and 7 milliseconds when no stream changed, and 63 milliseconds
  with every stream changing on every publish. The suggestion an operator sees is
  unchanged: the function producing it is not modified, and the scan is checked
  against the implementation it replaced across a corpus of transcripts.

  Worth recording because it was the obvious answer and it was wrong:
  constructing the regular expression inside the loop, which is what a reviewer
  names first, is 0.4% of what that line costs, and hoisting it recovers nothing
  measurable. The advisory search, the other candidate, is bounded by its own
  2048-character window and never exceeded a fifth of the total.

- A failure that stops reception is published whole instead of being cut off
  mid-word. The status line is a single elided row, which suits a running
  commentary and not a failure: "Live audio error: The selected audio input is
  un..." lost precisely the half that said to reconnect the device or choose
  another. Blocking failures -- a recording that will not open, an audio input or
  SDR that will not start, permission denied, no SDR device chosen -- are now
  carried in full alongside the status line, and cleared when reception next
  starts or is retried. A refused SDR retune is not treated as blocking: it stops
  nothing, and an error that stopped nothing has not earned a dialog.

- The Ctrl+Right drag that sizes the SDR decode region now measures the gesture
  in pixels rather than in hertz. Whether a gesture is a click or a drag is a
  fact about the pointer, and hertz per pixel is precisely what the spectrum
  zoom changes: at full span on a two-megahertz capture the old 250 Hz threshold
  was a sixth of a pixel, so a right-click that slipped at all resized the
  region; zoomed in far enough to see individual CW signals, a deliberate
  forty-pixel drag measured under the separate 1000 Hz threshold that chose the
  centre, so the region was set to the floor width and placed where the button
  came up instead of across the box that had just been drawn. One threshold now
  decides both, and the centre is always the middle of what was dragged. The
  width the drag will apply is shown while the pointer moves, so the 2 kHz floor
  and the 24 kHz ceiling correct the gesture visibly instead of silently.

- The OFF, RX and REGION listening buttons now show the monitor mode actually in
  force. They were checkable, so each toggled its own lamp the instant it was
  pressed, before the handler ran -- and the controller returns without
  announcing anything when handed the mode already selected. Pressing REGION
  while region listening was on therefore turned the lamp off and left the audio
  playing, and pressing it again turned the lamp back on without changing
  anything either.

- The receiver toolbar now shows what the monitor is doing, and why it is not
  doing it. Every reason a listening request fails -- region listening asked for
  without direct SDR reception, a monitor output that cannot carry the region's
  48 kHz mono float audio, an output that will not start or will not accept
  audio -- was already recorded and displayed nowhere, so a refused request and
  a silent output looked exactly like a button that does nothing.

- Cluster and reverse-beacon callsigns stay inside the spot bar. The callsign
  plates were positioned separately from the bar and nothing clipped them to it,
  so on any platform whose 13 px line box runs to 16 px or taller the callsign
  hung several pixels below the band that exists to contain it and was drawn
  over the waterfall. The bar and the plates are now sized from one measurement
  of the same font, and the plates are drawn inside the bar. The audio-offset
  ruler below them and the area a spot can be pointed at are measured from the
  bar rather than counted out again -- the ruler's ticks were landing inside a
  callsign plate, and the lower half of a callsign produced no tooltip.

- Cluster and reverse-beacon labels reserve the room a callsign really needs,
  measured from the font that draws it. The estimate it replaces charged 7.4 px
  per character, short for the DemiBold capitals a callsign is set in, plus
  eleven pixels apiece for two evidence marks that have not sat beside the
  callsign since they became the stripe underneath it -- so a spot reported by
  both sources reserved eleven pixels it did not use and lost a neighbour's
  label that would have fitted, while a spot with no stated source reserved ten
  pixels too few and two callsigns could print into each other.

### Added

- The decode region can be listened to as ordinary audio -- the whole region,
  not one selected stream. A **REGION** setting joins OFF and RX on the receiver
  toolbar while direct SDR is the source, and the same audio is carried to a
  remote observer and written into a debug capture. Every signal in the window
  is heard together, each at the pitch its own offset from the region centre
  gives it, so a higher tone is a station higher in the region. That is
  deliberately different from a decoder card's speaker, which narrow-filters one
  carrier and re-pitches it to the configured reference tone.

  The region is demodulated once and shared by all three consumers. Real audio
  sampled at Fs carries only 0 to Fs/2 while the complex stream it comes from
  carries its whole sample rate, so a region W wide needs at least 2W of audio
  or its top half folds onto its bottom half and two stations at opposite ends
  of the window arrive at the same pitch. The rule is the smallest of 48, 96 and
  192 kHz that is at least twice the region width. Because the source is
  analytic there is no image: the region is shifted up by half its width and the
  real part taken, and a station keeps its true position in the window. Nothing
  is demodulated unless something is listening -- region listening selected, an
  observer actually being sent audio, or a capture recording -- and it costs
  1.9 ms of processor time per second of audio at the full region width.

- Receive audio reaches a remote observer over UDP on the same port as the
  diagnostics stream. The observer authenticates on the existing TCP control
  connection and asks with a single fixed request; audio is sent only to the
  address that connection came from, only while the operator has explicitly
  allowed it, and stops when the connection closes. Datagrams are a 28-byte
  self-describing header then mono 16-bit PCM. The sender is bounded and drops
  rather than buffers, with the count published in the diagnostics record and in
  each datagram header, so a gap in a remote recording can be told from network
  loss.

- A captured band can be read back. The application has written SigMF since IQ
  capture was added and nothing in the tree could open one, so the only
  recordings replayable through the decoder were single-station WAV files. That
  gap is why two recent measurements could not be judged at all: a
  callsign-stability rule measured inert across the corpus because every clip
  holds one station whose single identification dominates its text, and a
  proposed bound on the refinement passes had nothing to bound because across
  seventeen transmission boundaries not one accepts a refinement. Neither result
  says the change is wrong; both say the corpus cannot contain the answer.
  `IqReplaySource` is the exact inverse of the writer, hands blocks back shaped
  as a live receiver delivers them, parses the sidecar strictly with no JSON
  dependency, and refuses what it cannot understand with a reason rather than
  guessing a rate nobody recorded.

### Changed

- The decoder follows 64 simultaneous carriers instead of 24, configurable to
  128. The old limit was not a decision: the display held 24 hand-written
  colours, the colour-lease table was sized to that palette, and the track cap
  was sized to the leases -- so how many stations could be decoded was set by
  the length of a list of hex strings. A capture of a real pileup holds about 50
  stations in a 6 kHz window at a median spacing of 70 Hz, all of them real, so
  half that band could not be tracked at any setting; live diagnostics also read
  `tracks: 24` pinned flat, which looked like a busy band and was the bank
  saturated against its cap. The new default is a measured budget -- under a
  twentieth of one core, no audio block over a fifth of its period -- not a
  round number.

- Track colours are generated rather than read from a table, so the palette no
  longer limits how many stations can be tracked. Hues are placed a golden angle
  apart in CIE L*a*b*, with lightness and chroma held inside a band that stays
  readable over a dark waterfall, which keeps any two tracks distinguishable for
  any number of them. The palette is randomised at each launch and fixed for the
  life of a run, so a frequency heard again inside the five-minute
  colour-identity window still looks like the same station.

- A stream that identified itself keeps its place on the waterfall about twice
  as long as one that never did. The longer hold is in the display's retained
  observation, not in track expiry: a track owns a decoder, a bank slot and a
  frequency cell that a genuinely new station has to be able to take over, and
  an earlier attempt that held an identified track alive twice as long stopped
  that takeover and silently broke identity inheritance.

- The diagnostics stream protocol version is now 2. It accepts two request lines
  from an authenticated client, so `emitOnly` in the greeting is `false`; a
  version 1 reader should refuse the stream rather than rely on a withdrawn
  guarantee.

- The decode region is capped at 24 kHz. The region is not only decoded, it is
  also what an operator listens to, and 48 kHz audio carries 24 kHz of bandwidth
  and no more. Capping it means the decode region *is* the listen window -- one
  number, no second concept, and nothing that can be decoded but never heard.

### Fixed

- A crowded band produced a transcript for every carrier in it, and almost all
  of them were noise. In an operator capture of a DX pileup -- about fifty
  stations in six kilohertz, median spacing 70 Hz, minimum 52 Hz -- one station
  was isolated and decoded cleanly while the rest were calling it from closer
  together than any filter can separate. The sum of several keyers is not Morse,
  so what reached the transcript list was roughly forty pages of nonsense and
  one real contact. Narrowing the filter does not help and was measured not to:
  at 30 Hz the timing statistics look plausible again and the decoded text stays
  noise.

  Each track's keying speed is now estimated twice, through the narrowest and
  the widest analysis filter it is already carried in. One keyer reads nearly
  the same speed through both, because widening admits more noise but no more
  keying; several superimposed do not, because the wider filter admits the
  neighbours, their envelopes beat, and their runs fragment. On the capture, of
  twenty-one carriers measured exactly one read as a single keyer -- 20.6
  against 24.4 words per minute, a ratio of 1.19 -- and it is the one that
  decodes. The next-nearest read 1.83 and the rest ran to 3.50.

  A track that reads as several keyers keeps everything an operator tunes by:
  detection, frequency, marker, colour, signal level and key activity. It stops
  publishing open-ended text and a station name, neither of which the signal
  supports, and reports `unresolved-keyer-overlap` as the reason. The verdict
  needs about a second and a half of consistent evidence to change in either
  direction, so a pileup thinning for one word does not flip a stream between
  copy and silence. This is a statement about effort spent so far rather than a
  permanent one: carriers this close can be separated by solving neighbours
  jointly rather than filtering them apart, so a refused track records how
  crowded its neighbourhood is and by how much its estimate diverged, and that
  work can be aimed where it would help. Clean single stations are unaffected,
  which was the requirement -- the surface benchmark still reads the same
  twenty-six callsigns correctly.

- A stream that had identified kept changing its name. One station read `EH3ST`
  cleanly eight times in a minute and `EH3S`, `EH3SN`, `EHSMST` and `EG7S` once
  each in between; the label followed every one of them in turn. The name was
  recomputed from scratch on every snapshot and written straight over the name
  already earned, so one momentary misdecode both renamed the stream and became
  the new memory, persisting into the silence that followed. The name now
  carries its evidence: an empty reading changes nothing, because a station that
  stopped sending is not evidence a different one arrived; one clean
  identification still names a stream immediately, but a name read twice becomes
  established and a disagreeing reading must itself be read twice before it
  takes over. Replacement still clears name, evidence and challenger together.

- The decode region snapped back to the settings value after a restart. A width
  set by dragging took effect immediately and held for the session, but only the
  settings dialog's Apply ever wrote the region to disk and the main window has
  no Apply to press. The region is now committed the moment it is chosen, from
  wherever it is chosen. One writer, the two keys the region already used, and
  no session-only copy to disagree with the saved one. The drag reports once on
  release, so one selection is one write; a receiver following the radio's VFO
  still does not write as it tracks.

- A debug capture of a direct SDR source contained no audio anybody could listen
  to. The capture wrote either `audio.wav` or the SigMF IQ pair, and an SDR
  session wrote the IQ -- so an operator recording a signal that would not decode
  was left with a file they could analyse and nothing they could play. An SDR
  capture now writes the demodulated decode region to `audio.wav` alongside the
  IQ. The IQ recording is unchanged and the capture still refuses to mix sample
  kinds within one file: this is a second file, not a change to the first.

- The decode window was nearly invisible. Its transparency was expressed twice
  -- an 8.6% alpha in the fill colour and an item opacity of 0.34 -- which
  multiplied to under 3%. Worse, an item opacity dims its children, so the
  border and the label faded with it: the two parts that carry the meaning were
  attenuated along with the tint that only has to hint. The fill stays
  translucent enough to read the spectrum through; the edge and the label are
  drawn at full strength.

- Decoding got steadily more expensive the longer the application was left
  running, so a session that started responsive ended up having to be killed.
  Reconstructing the word gaps in the active transmission -- trying every token
  against the exchange vocabulary and testing it for being a plausible callsign
  -- was redone from the beginning on every sample block, for every track, over
  a transcript that only grows. Profiling a run at two dozen tracks put 47% of
  all decoder time in that one call. The reconstruction is now remembered
  against the text it was computed from and recomputed only when a character is
  actually decoded, which is a few times a second rather than thirty-one. Not a
  character of the result changes: the function is pure and the input is
  compared exactly.

  Measured on the same fixture before and after, decoding one track for two
  minutes: total cost fell from 122 ms to 58 ms per second of audio, and the
  median block, which had climbed sixteenfold across the run, is now flat. At
  two dozen tracks the total fell by 40% and the fourfold rise in the median
  over three minutes is gone. A test decodes for two minutes and compares the
  median block of the last fifth of the run with the first; it fails at a ratio
  of 10 against the old behaviour and passes at 1.3.

  What remains, and is not fixed here: the worst blocks are still much more
  expensive than the median, and that tail still grows. It is the timing
  refinement at the end of a transmission, which decodes the event lattice once
  per candidate speed. That is real work at a real boundary rather than a
  repeat, so it needs an algorithmic answer rather than a cache.

- Streams were labelled with callsigns whose country prefix does not exist. A
  single missed or added element turns a real prefix into an impossible one,
  and nothing downstream of the decoder could tell the difference. A dictionary
  of the ITU's allocated prefix blocks -- `dictionaries/callsign-prefixes.txt`,
  seeded into the operator's dictionary directory on first run and editable
  like the others -- now decides whether a token could name a country at all. A
  label whose prefix falls in no block is refused, leaving the stream unnamed
  until the station identifies cleanly, which is the same honest answer already
  given when a caller sends only the operator's own callsign. The transcript is
  untouched: only the name is withheld, so the operator still reads what was
  copied. Participants listed for a QSO are held to the same rule. The table
  errs towards admitting -- a missing or unreadable file admits every callsign,
  because refusing every station on the band would be far worse than the
  misdecodes this catches.

- The application became jerky when several signals were decoding at once,
  while the processor was plainly not busy -- which is why it never looked like
  a load problem. The session list compares each row with the one it holds and
  reports only rows that differ, but signal-to-noise, speed, drift, coherence
  and every confidence are continuously varying measurements, so every row
  differed on every update and every decoded card was re-evaluated -- transcript
  text layout included -- on the one thread that draws. Measured on a running
  station carrying two dozen tracks: narrowband coherence and the keying-level
  figures differed on 100% of consecutive samples, signal-to-noise on 98%, and
  the drawing thread was blocked 22 ms at the median second against a 16.7 ms
  frame, losing one or two frames a second without ever stalling outright.
  Measurements are now rounded to what the card can actually display -- a tenth
  of a decibel or of a word per minute, a hundredth of a confidence -- so a row
  that has not meaningfully changed compares equal and stays quiet. Nothing an
  operator can read is lost: half a decibel still reports.

- Open decoder sessions past the bottom of the panel could not be reached. The
  list scrolls and always could, but nothing said so: with no scrollbar an
  operator saw the cards that fit and no sign the rest existed. Reordering is
  unchanged -- it is a drag on the card's own grip, so flicking the list is
  unaffected.

- The wheel button did nothing. Panning was written for it but the hit area
  never accepted that button, so the handler could not run.

- The waterfall filled with black stripes and stopped reading as continuous.
  Bounding the frames handed to the display meant some are refused, and the
  waterfall fills any interval it received nothing for with a blank row -- so
  that a real break in reception reads as a break and the time axis stays
  honest. It had no way to tell "nothing arrived" from "what arrived could not
  be drawn", and painted a continuous band of signal as interrupted, reporting
  a fault in reception that had not happened. A frame now carries how many were
  refused before it: those intervals resynchronise the row clock without
  drawing a gap, while a genuine input stall is still padded as before. The
  in-flight bound is four rather than two, because the analyser can emit
  several frames from one drain and two refused frames during ordinary
  operation cost continuity for no gain -- any small bound ends the unbounded
  growth equally well.

- Memory grew without bound until the application had to be killed. Spectrum
  frames were handed to the thread that draws with no backpressure whatever. A
  wide IQ transform is 8193 bins and each frame carries two float vectors of
  them, so one queued frame is about 64 kB and thirty a second is two megabytes
  a second; when that thread fell even slightly behind, the queue grew, and the
  growth made it fall further behind. An operator watched memory climb from
  244 MB to 668 MB -- a little over three minutes of precisely that -- and then
  had to kill the application. At most two frames are now in flight and the
  rest are dropped, because a display frame nobody drew is worth nothing: there
  is no history to preserve in a frame that was superseded before it reached
  the screen. Every emission goes through one bounded publisher so a new
  emission site cannot reintroduce the fault.

- Starting SDR reception decoded nothing until the operator nudged the decoder
  window. The window is published before the first IQ block arrives, and the
  guard that stops an audio source being disturbed by SDR settings recorded the
  request without applying it -- after which every republication asked for the
  same values and was skipped as unchanged. Requested and applied are now
  distinct, and a pending window is applied on the first block of complex IQ.

- Restarting lost the receiver. The chosen source was not persisted at all, so
  every start landed in sound-card audio; and the saved SDR device, which was
  persisted, could not be used because the Start control is gated on an index
  into the discovered device list while discovery only ran if the operator
  opened the SDR settings page. A saved receiver was present and unusable at
  the same time. The source is persisted, discovery runs at startup when there
  is a receiver to restore, and a receiver that has gone falls back to audio
  with the missing one named.

- The application stopped responding while several signals were decoding, with
  the processor largely idle. The decoded-channel model was published once per
  drained block, and the drain timer runs every five milliseconds over as many
  as thirty-two blocks; each publication deep-copies the whole model across a
  thread boundary and the receiving thread rebuilds it again. A handful of
  simultaneous signals therefore buried the one thread that draws while the
  others had nothing to do, which is why the processor never looked busy. The
  model is a snapshot, so the latest one says everything the intermediate ones
  would have: it is now published after the drain rather than inside it, and no
  faster than an operator can see. Verification diagnostics, published on the
  same timer, are rate-limited too.

- The decoder region could be moved by dragging but never resized. The width
  was floored at six kilohertz and rounded to the nearest kilohertz, so any
  drag narrower than six kilohertz set the width it already had. Two kilohertz
  is the real floor -- the narrowest slice the decimator will accept -- and the
  width is now whatever was dragged, to the nearest hundred hertz. A drag too
  short to be a drag still means "put the window here" and leaves the width
  alone.

- Direct SDR reception decoded nothing, and switching back to an audio card
  left that decoding nothing too until the application was restarted. The
  decoder window is republished whenever anything on the SDR page changes, and
  following the radio's VFO makes that happen continuously; each republication
  tore down decoder state. No track ever survived long enough to be identified,
  which is why reception appeared to start working if the receiver was simply
  left alone -- once the settings stopped moving, the resets stopped with them.
  Worse, that teardown ran even while an audio card was the source, so an SDR
  setting nobody was using destroyed a working audio decode. The window is now
  recorded whenever it changes but acted upon only while complex IQ is actually
  arriving.

  Found from operator debug captures rather than reasoned from the symptom.
  Replaying the captured audio through a fresh decoder recovered the callsign
  and the full transcript, and replaying the captured IQ through the real
  receive path recovered the same station, which placed the fault in live state
  rather than in the signal, the decoder or the window. Resetting the decoder
  once every six hundred milliseconds in that replay reproduced the empty
  display exactly.

- The transmit frequency could not be set on a radio that publishes no VFO
  identity, even where the radio said plainly that the second VFO was writable.
  Every VFO role was derived from OmniRig's VFO parameter, so a profile that
  omits it left the application believing there was no transmit VFO at all and
  Ctrl+left-click was refused. The FT-450D is such a radio, and it has the
  commands for both VFOs in its CAT set, so a control it demonstrably has was
  unreachable. With split enabled and no identity published, A now receives and
  B transmits -- the convention every transceiver shares -- claimed only where
  the parameter mask agrees the property can be written, so a capability is
  never asserted the radio has not published. The same reading now gives the
  receive frequency a named VFO to come from rather than the selected one.

- Setting the transmit VFO on the radio moved the receive frequency. OmniRig's
  active-VFO frequency is whichever VFO the radio has selected, not the receive
  one, and on a rig that publishes no per-VFO property -- the FT-450D among
  them -- the receive frequency was read from it regardless. With split on,
  selecting VFO B to set it therefore dragged VFO A along, and with it the RF
  axis, the spot band filter and the decoder's frequency mapping. That reading
  is now trusted only where one VFO is in play; otherwise the last known
  receive frequency is held, because no reading beats one that is wrong exactly
  when the operator is working the other VFO.

- Right-click on the spectrum did two things at once: it pointed the received
  spectrum and opened a manual decode, so an operator asking for either always
  got both. Pointing stays on right-click; a manual decode is Ctrl+right-click.

- Retuning striped the waterfall. Sliding the rows to follow the receiver reset
  the conditioner's per-bin noise baseline, which takes about a second to
  re-converge, so every retune painted a horizontal band while it settled and
  tuning across a band left a row of them. The baseline now slides with the
  rows it belongs to.

- Ctrl+left-click did nothing at all on a radio that does not offer transmit
  frequency control, with no message anywhere, which reads as the feature being
  broken rather than as the radio not offering it. It now says so. Pointing
  transmit on rigs that have no writable transmit VFO needs a VFO-swap sequence
  and is tracked separately; it is not attempted speculatively, because a
  failure partway would leave the radio receiving on the wrong VFO.

- Moving the RF spectrum no longer loses the tracks. Making the decoder window
  follow the receiver, so that direct SDR reception decodes on any band, meant
  the window was republished on every retune -- and the worker rebuilt the
  whole decoder whenever it changed, destroying every track, transcript and
  identity each time the operator moved the radio. The samples jump at a
  retune; the stations do not. A track's frequency is absolute RF, so the
  signal path is restarted and the tracks are left standing, with those the new
  window no longer covers parked on the same rule a VFO move uses and revived
  when the receiver comes back to them. A republication asking for the window
  already in force now does nothing at all.

- Changing band left the frequency ruler showing the old band. The axis
  re-evaluated only on a source change, while everything it reads -- whether a
  radio is readable, its receive frequency, the sideband and the reference tone
  -- arrives on a different signal. On an audio card the spectrum's own bounds
  do not move with the dial, so nothing in the binding had changed and 40 m
  labels survived a switch to 20 m.

- A cluster login carrying an SSID would have connected and then never sent
  its callsign. The login was written to the socket through the shared
  callsign policy, which refuses a hyphen, so the composed name resolved to
  nothing and the node was left waiting at its prompt.

- Moving the VFO no longer costs a tracked stream. A signal carried out of the
  processed passband by a retune was expired on the ordinary retention
  timeout, exactly as if it had faded -- which was deliberate and wrong. The
  operator turned the dial; the station's position is known exactly, and
  turning back puts it at a computable place. Such a track is now parked
  rather than expired, held for three minutes, kept out of the decoder so no
  edge noise reaches its transcript, and restored with its own identity when
  the band comes back to it. Its transcript, colour and audio monitor follow
  the station instead of a new card appearing for it.

- The spectrum's frequency axis reads in RF when the receiver's frequency is
  known, instead of always in audio. On an audio card the axis carried the
  passband -- nought to twenty-four kilohertz -- while the decoder cards beside
  it already showed absolute RF, so one screen named the same signal two ways.
  Sideband is honoured, and the axis falls back to audio when there is no dial
  reading to map against, an honest passband being better than a blank ruler.

- Spots from a cluster were never drawn. The spectrum overlay was gated on the
  master switch of the web feed rather than on whether any spot source was
  live, so joining a node produced markers the display then refused to show.

- Direct SDR reception decoded nothing on any band but 20 m. The decoder is
  fed by a narrow slice taken from the wide capture, and the slice is refused
  outright unless it lies wholly inside the passband being acquired. Its
  centre defaulted to 14.050 MHz and moved only when the operator dragged a
  selection in the zoomed view, so a receiver started anywhere else had every
  block refused. The overview spectrum is produced before that refusal, which
  is why the fault showed as signals painting normally on the spectrum and
  waterfall with no stream ever identified or marked. The slice is now
  re-centred on the capture whenever it would fall outside it, both when
  reception starts and when the receiver is retuned, so the decoder follows
  the radio instead of being stranded at a frequency no longer being received.

- Starting SDR reception opened the display at whatever span the hardware
  delivered rather than the one selected. A device asked for 2 MHz commonly
  runs at the nearest rate it supports instead, and opening at 8 MHz put the
  whole CW segment inside a few pixels. The first view is now the configured
  width, centred on the capture, and falls back to the delivered span when
  that is narrower.

- Stations other receivers report hearing can be shown and used as
  corroboration. A receive-only feed places a marker on the separator between
  the spectrum and the waterfall where a station has been reported, with a
  square before the callsign for reverse-beacon evidence and a circle after it
  for cluster evidence, both appearing when the two independent sources agree.
  The same reports can raise a decoded callsign's standing among close
  candidates, spending the existing bounded provider budget rather than a new
  one, decaying with age, and matching only on exact equality. A spot can
  never supply a callsign, rewrite a character, or let a candidate with no
  acoustic support win, because reverse-beacon reports carry a measured error
  rate approaching two per cent per receiver. Nothing is ever sent to the
  feed but a request for spots, and nothing it returns can reach the transmit
  path. The whole feature is off by default.

- Documentation was reviewed against the source rather than against itself.
  Among the corrections: multiple-pass weak-signal decoding and alternative
  width selection were described as planned although both ship; a trained
  key-down likelihood model was described as existing although no such
  artifact or training code does; four operating modes and six scene-graph
  classes were described as present although none exist; and the transmit
  alphabet was still documented as unable to send a distress prosign after
  that decision was reversed and implemented.

- Moving the VFO could lose an identified stream and its audio monitor. The
  decoder carries every tracked signal with the dial so that identity
  survives a retune, but the request to do so was only made when the radio
  had been continuously readable: a momentary gap, which a polled radio
  produces while it is busy retuning, swallowed the move that followed it.
  The tracks then stayed at the old audio frequency while the signal moved
  away, so the stream was lost and re-acquired as a new one once it decoded
  again. What matters is that a previous frequency is known and the dial has
  moved, not that the radio was readable in between.

- Retuning a software-defined receiver erased the waterfall. A row is a
  history of frequency, so when the receiver moves that history is still
  true and simply belongs at different bins; it now slides by the number of
  bins the band moved, keeping what was drawn under the frequency it belongs
  to. Discarding it wiped the display at every click of the dial, which is
  not what the same action does on audio, whose axis does not move with
  tuning. History that cannot be slid, because the span itself changed and
  the bins no longer mean the same width, is still dropped.

- A debug capture reported that the Morse alphabet came from the copy inside
  the application whether it did or not. The flag compared symbol counts, and
  the shipped file and the built-in copy are generated from the same source,
  so they can never differ that way. The origin is recorded when the alphabet
  is imported instead.

- The weak-signal setting now applies to WAV replay as well as live audio. It
  reached only the live path, so replaying a recording decoded by different
  rules than hearing it live.

- Runs of fragments are removed from decoded text while the copy around them
  is kept. When keying evidence breaks up, what comes out is a run of the
  one- and two-element characters, and that happens inside an otherwise good
  track as readily as a bad one: a track reading `CQ POTA DE SN5WLF`
  correctly several times filled the spaces between with single-element runs.
  Suppressing whole tracks on this measure is therefore wrong, and measurably
  so, since tracks that recovered a correct callsign reach such runs of ten
  themselves. Six is the shortest run that is safe, real copy reaching four
  and five, and the run is replaced by a space rather than deleted so that
  unrelated text is not joined together.

- The receive pipeline no longer exceeds real time at the number of signals
  it is willing to track. Every tracked signal ran three oscillators and five
  three-stage complex filter cascades for every sample, whether or not
  anything read the keying evidence they produce, and that cost grew in
  proportion to tracked signals while building the spectrum cost the same for
  one as for twenty-four. A track that will not be decoded is no longer
  filtered. At twenty-four tracked signals the pipeline went from 1.09 times
  real time to 0.71, and mean character error across the synthetic surface
  did not regress.

- The spectrum trace drew its noise floor flat along the bottom of the plot,
  where neither its shape nor a signal's height above it could be read. One
  bound was serving two purposes: a palette that wants to start just under
  the noise so none of its range is spent colouring it, and a trace whose
  baseline is also the labelled axis and wants room beneath. The trace now
  keeps twelve decibels of headroom below the estimated floor while the
  waterfall keeps the tighter bottom it needs.

- Weak signals can be excluded from decoding, and are by default. A track
  below the threshold is still detected, tracked and drawn; it is simply not
  decoded, because below it what reaches the decoder is fragments rather than
  copy and each such track costs a decoder's worth of processor time. The
  gate reads the strongest level a track has reached rather than its level of
  the moment: gating on the latter suppressed signals while they were still
  being acquired and cost two of eight recovered callsigns on the capture
  corpus, one of whose settled level was thirty-five decibels. A track the
  operator selected is always decoded. Settings carries the switch and the
  threshold, defaulting to twelve decibels against a measured nineteen and a
  half for the weakest correctly recovered callsign.

- Reconfiguring the channel bank no longer discards settings that have their
  own setters. A caller changing one value passes a freshly constructed
  configuration, and replacing the whole structure silently reset the others;
  the station callsign survived only because the application re-applied it
  afterwards.

- The CW vocabulary gains the award and activity programmes an operator sends
  constantly -- POTA, SOTA, IOTA, WWFF and others -- along with ordinary
  ragchew words that were missing, among them UR, MNI, GUD, CU and RPRT.
  POTA and SOTA also join the small set distinctive enough to stand as
  evidence of real CW, since an activator sends one in the first exchange.
  Because the vocabulary is data this was an edit rather than a rebuild.

- The Windows build failed. A test added with the built-in alphabet used the
  POSIX `setenv` and `unsetenv`, which MSVC does not provide, so the core
  tests did not compile there while Linux and both macOS builds passed. The
  test uses a small portable helper now. The generated built-in alphabet also
  no longer records the full path of the machine that produced it.

- An upgrade can no longer be affected by what an earlier version left in the
  application data directory. The dictionaries carried inside the application
  are authoritative, and a copy in the data directory is used only when it is
  present and parses to something usable -- for the alphabet, only when it
  actually carries the letters and digits. A missing or unusable copy is
  replaced so an operator sees what is in force and can edit from it; a
  usable one is never overwritten. Nothing an operator has to do by hand is
  required when upgrading.

- Debug captures record what the detector was given. A report that nothing
  decodes could not be told apart from a configuration that excluded the
  signal, because the analyzer settings, whether any spectrum frame reached
  the detector at all, how many times the decoder had been reset, and whether
  the alphabet in force came from a file or from the built-in copy were all
  absent from the capture. They are recorded now.

- The decoder could be left with no Morse alphabet and would then track
  signals and decode nothing at all, showing a strong carrier, a measured
  signal-to-noise ratio, and no text or speed. Moving the alphabet into a
  data file made the decoder's core function depend on a file load that
  nothing verified, and a packaged build that failed to read it went
  silently deaf. The shipped alphabet is now also compiled in and used when
  every attempt to read a file fails; it is generated from that same file at
  build time, so it is the one alphabet rather than a second copy to keep in
  step. An empty copy in the operator's directory is repaired from the
  built-in one instead of being preferred forever, which is how a single
  failed read became permanent. The bundled files are addressed through one
  base path rather than a per-file alias, because such aliases resolve
  differently across build generators and a misplaced resource is what left
  an empty file behind in the first place.

- The live receive path can be measured per stage. `PERF-002` accepts an
  optimisation only after a before-and-after measurement, and there was no
  instrument to provide one. A profile now reports wall time for the
  spectrum and sample stages against the number of tracked signals. It shows
  the spectrum stage costing the same for one signal as for twenty-four
  while the sample stage grows in proportion to them, reaching real time at
  the twenty-four the decoder tracks: the cost is per tracked signal, not
  per unit of bandwidth.

- Prosigns can be transmitted. The encoder is keyed by character and could
  not represent a multi-character symbol, so an operator closing a contact
  had to send the letters, which is not what `<SK>` means on the air. Seven
  are now keyed as single symbols with no character gap between their
  letters: `<AR>`, `<AS>`, `<BK>`, `<CT>`, `<KN>`, `<SK>` and `<SOS>`. The
  table is closed and lives in the application rather than a data file,
  because what may be transmitted is a safety boundary, and a bracketed
  token that names none of them is refused during normalization rather than
  reaching a staged message. A distress call is included: sending one is
  legal and appropriate, and transmitting one unintentionally is already
  prevented by arming, exact callsign confirmation, message preview, an
  explicit send action, and a decoder that can never initiate a
  transmission.

- Contest cut numbers were incomplete. Only `T` for zero and `N` for nine
  were declared, so a serial sent as `ANU` read as letters rather than 123 --
  a wrong exchange logged as though it were right. The shipped contest files
  now carry the set an operator actually sends at speed, and because those
  files are data this was an edit rather than a rebuild.

- Three duplicated definitions are written once. The nine speed anchors every
  decoder starts from existed separately in the multi-speed and probabilistic
  decoders, so a speed added to one and not the other would have left the two
  disagreeing about which senders they can acquire with nothing to report it;
  the starting anchor is also named now rather than written as a bare index
  that silently selects a different speed if the set changes. The contest
  contraction of a signal report is declared once instead of beside each
  field that needs it.

- A failing decoder test crashed instead of reporting. The slow-track checks
  read the first element of containers the expectations immediately above had
  just failed on, so a decoder that produced no track turned a clear list of
  failures into a segmentation fault, and the crash was what got reported
  rather than the cause. Measured against a build with no dictionaries: the
  suite now exits with twenty-four named failures instead of a signal.

- Contest exchanges are data. The four supported contests were built in the
  application, so a sponsor changing an exchange, or an operator wanting a
  contest that is not among them, needed a rebuild. Each is now a file in
  `dictionaries/contests/`, carried inside the application, copied to the
  operator's data directory on first run and read from there afterwards. A
  file describes only the exchange: the conversation flow, its states and
  every transmit safety gate are still built by the application and cannot be
  named in one, so no file in that directory can arm a transmitter, change a
  key-down timeout, or relax callsign confirmation. A file is validated by
  the same gate the built-in profiles were written against and is refused
  whole if it fails, because an exchange that silently lost a field would be
  worse than a contest that does not appear; one bad file does not remove the
  others. The tests now read the shipped files rather than a private copy.

- Two more copies of the CW vocabulary are gone. Callsign attribution split a
  run-together prosign using its own list of four tokens while the context
  rescorer used another of six, so the two paths could disagree about the
  same text; both now read the one file. Track verification kept a third
  list, of the tokens distinctive enough that a single match is accepted as
  evidence of real CW, and that is now `cw-distinctive-tokens.txt`. It is
  deliberately not the whole vocabulary and is tested for the property
  rather than the contents: the gate works only while a match stays harder
  to counterfeit than the checks it replaces, so a single letter admitted
  there would quietly stop it meaning anything. The positional rules that
  weigh a callsign by what precedes and follows it are unchanged; naming a
  token in a grammar rule is not a duplicate vocabulary.

- The Morse alphabet existed twice, as two verbatim copies in separate
  translation units, which is exactly the arrangement that drifts without
  anyone noticing. There is now one copy and it is data: `morse-alphabet.txt`
  alongside the other dictionaries, carried inside the application and
  extensible in the operator's data directory, so an accented letter or an
  unnamed prosign no longer needs a rebuild. Unlike the exchange vocabulary
  an empty alphabet is not a graceful degradation but a decoder that reads
  nothing, so the shared instance recovers by reading the directory named by
  `CWA_DICTIONARY_DIR` rather than letting a caller that forgot to load it
  fail quietly. The transmit alphabet is unchanged and stays in the
  application: it is narrower on purpose, since `<SOS>` must be readable
  without being sendable, and a transmit constraint does not belong in a file
  an operator can edit.

- Word-boundary placement is measured. Neither existing instrument could see
  it: the synthetic surface is byte-identical with and without the context
  vocabulary because it never reaches the rescorer, and the capture corpus
  scores callsign recovery rather than spacing. A new benchmark generates
  contacts whose character and word gaps are deliberately pushed together --
  the measured shape of a real fist, and the one distinction a duration
  threshold cannot settle -- decodes each scene twice from identical samples
  with the vocabulary loaded and cleared, and scores only those decodes whose
  characters already match, so a lettering error is never counted as a
  spacing one. With gaps pushed to within a fifth of each other the
  vocabulary raises boundary recall from 0.429 to 0.582 and F1 from 0.574 to
  0.707, and at the milder setting from 0.958 to 1.000. It asserts no extra
  boundaries at either setting, which is the property that matters: the count
  of spurious boundaries is identical in both arms. The benchmark fails if
  the vocabulary ever stops recovering boundaries, starts inventing them, or
  changes a decoded character.

- The CW vocabulary the decoder recognises is operator-editable text rather
  than a list compiled into the application, and it has grown from thirteen
  tokens to eighty-two: the Q-codes an operator actually sends, signal
  reports and their contest contractions, the closing prosigns, and the
  ordinary vocabulary of a ragchew. The files are carried inside the binary,
  written to the application data directory on first run, and the operator's
  copies are preferred at startup thereafter, so an edit survives an upgrade.
  Nothing is left compiled in, including the one glued token that used to be
  named in code: a run-together reading is now decomposed against the
  vocabulary itself, so a token added to the file needs no code to support
  it. Matching is weighted by length, because a single letter falls out of
  almost any spacing by chance while a three-character Q-code does not, and
  without that weighting a vocabulary this size would let incidental
  one-letter matches outvote a genuine Q-code. These lists only choose
  between readings carrying identical characters, so a token can move a word
  boundary and can never change a decoded letter.

- Zoom on the wide SDR display was discarded by the next spectrum frame. The
  view was rebuilt from the frame bounds unless the zoom was being carried
  across a retune, but carrying it is by definition a response to the source
  bounds moving, so the condition was false for every ordinary frame and the
  view snapped back to full span as soon as one arrived. Only a source change
  may move the view now, and a regression test zooms, feeds further frames on
  unchanged bounds, and requires the span to survive them.

- The wide SDR spectrum and waterfall showed noise rather than signals. The
  overview produces 16384 frequency bins and the display is around 1500 pixels
  wide, and every bin was being drawn: the trace connected all of them, so each
  pixel column was rasterised as a bar spanning the entire spread of its
  eleven-or-so bins, and the waterfall built a texture one pixel wide per *bin*
  before letting bilinear minification pick an arbitrary two of every eleven.
  Both showed the raw scatter of the measurement instead of what was received.
  Each column is now reduced to the strongest bin it covers -- the peak rather
  than an average, because a CW carrier occupies one or two bins of the eleven
  and averaging would bury it by around ten decibels, while the noise between
  carriers is already smoothed over time. The waterfall texture rebuilt every
  frame falls from roughly forty megabytes to about three.

- The waterfall palette was spending most of its range on noise. Its floor was
  derived from the twentieth percentile of the bins and then lowered a further
  eight decibels; for the exponential distribution that bin powers follow, that
  percentile already sits six and a half decibels below the mean, so the palette
  bottom ended up about fourteen decibels beneath the actual noise floor and the
  noise itself straddled the blue-to-green transition. The floor now comes from
  the median with a two-decibel margin, which moves mean noise from roughly a
  quarter of the way up the palette to near its bottom and hands the range back
  to signals.

- Noise suppression stopped suppressing at wide bin spacings. Its local
  reference window was sized in hertz, so at a megasample per second it
  collapsed to a median of six neighbouring bins, each carrying the same
  scatter -- comparing noise against noise, which adds speckle rather than
  removing it. The window now has a floor in bins as well, so the reference
  always has enough samples to be a reference.

- Zooming into the SDR spectrum and then moving the VFO threw the zoom away and
  returned to full span. SDR frames are described in absolute radio frequency,
  so retuning genuinely moves the axis, and any axis change was treated as a new
  source. An operator's zoom is now preserved and re-centred on the new
  frequency. Audio never showed this because its axis is fixed regardless of
  tuning. The waterfall history is also cleared on a frequency change: it was
  only being cleared when the number of bins changed, which a retune does not
  do, so old rows were redrawn against the new axis under the wrong labels.

- The centre bin of a direct-SDR spectrum is no longer allowed to distort the
  display. Local-oscillator leakage sits exactly at the centre of the span and
  was feeding the automatic range as though it were a signal. It is interpolated
  across for display only; detection is unaffected.

### Changed

- The spectrum gestures are regrouped so that each does one thing and related
  intentions share a modifier. Defining the decode region moves from
  Shift+left-drag to **Ctrl+right**, joining plain right-click, which points
  it: both belong to deciding where to listen. Opening a manual decode moves
  to **Alt+left-click**, beside plain left-click, which opens a stream
  detection already found: both belong to deciding what to read. Ctrl+left
  still requests the transmit frequency, and clicking the **wheel button** now
  retunes a direct IQ receiver so that frequency becomes the centre of the
  acquired spectrum. The region drag no longer also opens a decode at the
  centre of the selection -- choosing where to listen was silently creating a
  stream nobody asked for, at a frequency that is merely the middle of a drag.

- Spots on the separator now sit in a bar of their own and carry their source
  as colour rather than as a symbol. The callsigns are a fifth larger, and a
  square before the call for reverse-beacon evidence and a circle after it for
  cluster evidence are gone: they asked an operator to remember which shape
  meant which, cost width beside every callsign, and read as punctuation rather
  than information. A two-pixel stripe under the call carries it instead --
  blue for a receiver's report, tan for a person's -- and when the two sources
  agree the stripe is half of each, so agreement is legible without a second
  mark and takes no extra room. Both hues stay in the chrome family, clear of
  the decoded-stream identities, the decode window and the transmit slice, so a
  report cannot be mistaken for something this receiver copied. The bar is
  drawn only when there is something to put in it.

- The two spot subsystems are one. Spots arrived either from a polling web
  feed or from a telnet cluster, each with its own settings, and the operator
  had to understand a transport distinction that was never theirs to make. The
  web feed has been removed outright: it accepted a document shape no public
  service emits, so it never had an address that would work, and the two
  checkboxes that chose between report kinds were meaningless once a node is
  joined -- what kind of report a spot is arrives with the spot. What remains
  is one node, one switch, and the retention, tolerance and label settings that
  are about spots rather than about transport. It now lives under its own
  Cluster tab instead of inside Decoder, and a status chip beside the transmit
  state says whether the link is up.

- The threshold below which a signal is not decoded is 4 dB, not 12 dB. The
  higher figure came from a corpus of twenty-two recordings made through a
  single receiver; that corpus was never the whole population, and on air the
  gate proved aggressive enough to suppress workable signals. Recovery across
  the corpus is unchanged at the lower setting. The saving in processing that
  the gate also produced is largely given back, because skipping the filtering
  is the same act as declining to decode: at twenty-four simultaneous signals
  the cost returns from 0.71 to 1.06 times real time.

### Added

- The diagnostics stream authenticates each reader and can refuse peers
  outright. **Allowed peers** takes addresses or subnets -- `192.168.1.50`,
  `192.168.1.0/24`, `2001:db8::/32` -- or `any`; a peer's address is known from
  the socket before a byte is exchanged, so one that is not permitted is closed
  without a greeting and never learns what is behind the port. Left empty it
  permits loopback only, and a rule that cannot be parsed permits nothing, so a
  typo can never widen access. The access token is then read as one bounded
  line and compared in constant time; a client presenting the wrong token, or
  none, receives no record at all. IPv6 is carried throughout -- discovery lists
  both families, either can be bound, scope identifiers and bracketed literals
  are accepted, and an IPv4 client arriving on a dual-stack socket still matches
  an IPv4 rule.

- Diagnostics report how late the thread that draws is running. A hundred-
  millisecond heartbeat measures its own lateness, so a blocked interface
  becomes a number rather than an impression: `latenessMs`, `peakLatenessMs`,
  `stallCount`, and `sinceLastHeartbeatMs`, which grows while that thread
  cannot answer at all and so makes a stall readable while it is happening
  rather than only afterwards.

- A station can be watched while it runs. Settings gains a **Network** tab
  offering the addresses this machine can bind, one checkbox each, with the
  interface named and loopback marked; the chosen addresses carry a
  line-delimited JSON stream of the same records the debug capture writes,
  once a second, for as long as the service is enabled. An indicator sits
  beside the transmit state for as long as it is listening, because a service
  an operator has forgotten is running is the one that will surprise them.

  **The stream only emits, and that is the whole safety argument.** This
  process holds transmit, so a diagnostics channel that accepted input would
  be a second and weaker way to reach a radio. Bytes arriving from a client
  are discarded and never parsed; a peer that keeps sending is disconnected,
  because it has mistaken the port for something that answers.

  That decision has an honest cost, stated in the settings page rather than
  buried: per-client authentication would require reading what a client sends,
  so there is none. The access token gates which addresses may be bound -- a
  routable address cannot be offered without one -- but anything that can reach
  a bound address receives the stream. Real authentication belongs with the
  planned remote-operation work, where TLS and per-client certificates are
  designed for rather than bolted on. The service is off by default, bound to
  loopback until told otherwise, and holds at most four observers, dropping one
  that cannot keep up rather than growing this process until the station stops.

- The waterfall can be switched off to save the resources a station running as
  a diagnostics server does not need for it. Turning it off detaches the
  display from the frame source rather than merely hiding it, so no row is
  conditioned, appended or retained and the history already held is released.

- Diagnostics record whether the application is keeping up, not only what the
  decoder found. Model publications per second, blocks drained per second,
  drains that hit their own block cap -- the honest sign that samples are
  arriving faster than they are consumed -- and average and peak drain
  durations. These exist because an operator reporting that the application had
  stopped responding left no number anywhere to look at, and the cause was
  invisible in diagnostics that described only the decoding.

- Diagnostic output can be written to a file for a whole session with
  `--log-file <path>` or by setting `CWA_LOG_FILE`. Every line is flushed as it
  is written, because a log that loses its last buffer is silent about the one
  moment worth reading. Off unless asked for.

- The cluster login can carry a connection SSID, 0 to 99. An operator running
  more than one connection from one station is told apart on a node by it, so
  CW Buddy can join as `CALL-1` beside a logging program already logged in as
  `CALL`. It is a number rather than a second callsign field: the login is
  always composed from the station callsign, and the line beneath the switch
  shows it exactly as it will be sent. The shared callsign policy is
  unchanged -- it is what the decoder uses to judge a callsign on the air, and
  loosening it to admit a hyphen for the sake of a login would have changed
  what the decoder accepts.

- The spectrum separator carries an audio-offset scale, completing the ruler
  work begun when the main axis started reading RF. Once the axis is in RF an
  operator loses the sense of how wide the visible span is in audio, which is
  what the decoder hears and what the receiver's filter sets. It appears only
  where the two scales genuinely differ -- on direct IQ the frames already
  arrive in absolute RF, so a second scale there would print the same numbers
  twice -- and sits below the spot markers in the one band of the gutter that
  no other mark occupies.

- Spots can now be received from the DX cluster and reverse-beacon networks
  themselves, over telnet, rather than only from a web feed. This is what
  those networks actually speak: the public HTTPS feeds are aggregators, and
  the Reverse Beacon Network -- the one source here that is a receiver rather
  than a person, reporting every call it decodes with a measured
  signal-to-noise ratio and speed -- publishes no web interface at all.

  A server is chosen from a list in `dictionaries/dx-cluster-servers.txt`,
  which is data and not compiled in, so a host that moves or a node that
  closes can be corrected without a new build; a custom host and port can be
  given instead. Every server shipped in that list was connected to and
  verified before it was listed.

  Spots are filtered twice, and neither filter replaces the other. On the
  server, the filter commands listed for it are sent at login and any naming a
  band is sent again when the radio changes band, so what the server sends
  follows the receiver. Every such command was watched being accepted by the
  server it is listed against rather than taken from documentation, because a
  guessed one earns an error reply the operator never sees and leaves a filter
  that quietly is not there. DXSpider takes `accept/spots 0 on 20m/cw`;
  AR-Cluster refuses that band name outright and takes
  `set/dx filter band=20 and mode=cw`, so the band is offered to the server
  list under two names rather than one form being bent to fit both. On arrival, always, because the reverse beacon network
  accepts no filter commands at all and its feed runs at roughly six spots a
  second worldwide; anything outside the band being received is discarded
  before it is stored. When no receive frequency is known nothing is
  discarded and no band filter is sent, because a filter that threw everything
  away for not knowing where the radio was pointed would be worse than none.

  The client joins with the station's configured callsign, which is what the
  cluster network is for, and sends nothing else: no spots, no announcements,
  and no reply to anything the server or its users send. Cluster logins are
  unencrypted, as that protocol has always been. One connection is held at a
  time and a failure is retried after a wait that lengthens each time rather
  than in a loop, because these are volunteer machines shared by thousands of
  operators. The whole feature is off by default, and the existing prohibition
  is unchanged: a spot corroborates a callsign this receiver decoded and can
  never supply one, rewrite a character, or reach the transmit path.

- Direct SDR reception can be recorded as interoperable IQ. Recording previously
  refused to run at all on an SDR source, and the writer behind it kept only the
  real half of each complex sample -- which discards the distinction between the
  two sidebands and cannot be used for analysis afterwards. Captures are now
  written as SigMF: interleaved complex samples in a `.sigmf-data` file with a
  `.sigmf-meta` sidecar carrying sample rate, centre frequency, datatype and
  start time, so recordings open in other software. The default datatype is
  `ci16_le`, which halves the file against 32-bit floats and loses nothing on
  hardware whose converters are fourteen bits or fewer; a bit-exact float option
  remains. Recording is bounded by a byte budget as well as a duration, because
  a limit expressed in samples was written for audio rates and would be reached
  in about twenty seconds at eight megasamples per second.

- Each IQ recording carries the receiver's gain state and its own level
  measurements -- peak magnitude, how many samples approached full scale, and
  the residual direct-current offset -- per block and cumulatively. A recording
  that documents a symptom without recording the gain that produced it cannot
  settle whether the receiver was over- or under-driven, and the application has
  no other overload indication.

- The waterfall frequency scale is legible: larger, brighter, and drawn on a
  plate so it is not competing with the waterfall behind it.

- The decode window stays findable when it is off screen. Zoom and the decode
  window are deliberately independent, so an operator can be looking somewhere
  the decoder is not; an edge indicator now points to the decode window and
  names its centre frequency when it is outside the visible span.

- The spectrum snapshot can now describe a noise floor independent of
  transform size. Bins stay calibrated to the window's coherent gain, which is
  what a tone's dBFS reading needs and keeps a full-scale carrier at 0 dBFS at
  any transform size, but that same calibration makes bin noise power grow
  with bin bandwidth, so an unchanged floor reads about nine decibels lower
  when the transform grows from 2,048 to 16,384 bins, and no single factor can
  hold a tone and a noise floor constant at once. The snapshot now also
  publishes the equivalent noise bandwidth of a bin, in hertz, so a consumer
  judging a floor rather than a tone can subtract it out and compare noise
  density in dBFS per hertz across transform sizes and sample rates.

- The selectable keying model can be measured. Every benchmark constructed a
  decoder and left the model at its default, so the alternative available in
  Settings had no coverage and a regression in it would have been invisible.
  The accuracy surface now accepts `--semi-markov`, and the two models measure
  as follows on the full surface: mean character error 0.2579 against 0.2591 --
  level, against a spread across seed sets of about 0.08 -- while callsign
  recovery differs materially, 150 correct against 136, for 33 and 31 wrong
  respectively. The per-frame threshold remains the default on that evidence.

### Changed

- The SDR faceplate follows the same visual language as the radio faceplate
  beside it. Sync is a square tile that turns green when it is engaged, rather
  than a wide pill whose only active-state feedback was the colour of its text.
  No control clips its own label any more; the operating mode, antenna,
  bandwidth and tuning step showed truncated text because each was given a fixed
  width narrower than the space its own contents needed. Stream markers show
  frequencies in the same grouped form as the tuning readout instead of a bare
  digit string.

- The decimation badge is gone. It displayed a fixed caption with no value
  behind it and no means of ever acquiring one; the effective sample rate shown
  beside it already determines decimation, and the explanation it carried has
  moved onto that control.

### Changed

- Direct SDR operation now has its own compact **SDR Radio Control** above the
  independent CAT **Radio Control**. It provides an editable SDR RX readout,
  one-step left/right tuning, effective IQ-rate/driver-decimation control,
  hardware RF-bandwidth and antenna/input selection, plus provider-neutral
  **Radio Sync**. The waterfall edge arrows also tune direct SDR reception.

- SDR discovery now groups driver configurations by physical receiver and
  serial number. An RSPduo therefore appears once, with Single Tuner, Dual
  Tuner, and 6/8 MHz Master configurations in a separately described
  operating-mode selector;
  independent receivers such as an RTL-SDR remain separate entries.

- Bidirectional Radio Sync treats CAT readback as observation and an SDR
  faceplate edit as one explicit provider-neutral RX-frequency command. The
  configured signed LO offset is applied in opposite directions, stale echoes
  cannot create a write loop, and decoder-window-only gestures never tune the
  radio.

- Radio Settings and the station wizard now expose only configuration consumed
  by the selected provider. OmniRig owns its COM/framing values, rigctld owns
  its physical-radio connection, and CAT4OM owns its server-side connection;
  CW Buddy no longer shows misleading local baud, parity, stop-bit, polling,
  or timeout controls for those integrations. The retained direct-serial model
  validates baud rates against conventional 1200–115200 choices and explicitly
  bounds data bits, parity, and stop bits for a future direct-CAT provider.

- In direct-SDR views, Shift+left-drag now selects the decoder center and
  bandwidth directly on the spectrum/waterfall. A short Shift+click recenters
  the existing window, the drag overlay previews the selection, and the
  established wheel-zoom, middle-pan, right-probe, and Ctrl+click TX gestures
  remain unchanged. The compact pointer legend now stays visible for at most
  ten seconds and cannot reappear for five minutes; a profile-persisted Display
  option can disable that spectrum overlay without removing button tooltips.

- The public design now defines distinct Standard, PileUp Chaser, PileUp
  Slicer, and Runner workspaces. Chaser includes an observation-only learning
  phase, configurable half-duplex relearning, continuous full-duplex learning,
  asymmetric runner/pileup decoding, evidence-gated VFO B following, and a
  strict boundary that prevents an operating mode or decoder output from
  acquiring TX authority.

- Decoder transcripts now scroll inside a fixed clipped frame with explicit
  style-independent padding. The guarded TX action is left-aligned in the
  lower row, while a matching full-size **Monitor** button replaces the small
  standalone speaker glyph at the right.

- The former fixed 700 Hz visual boundaries now show the authoritative TX-VFO
  slice. They follow radio readback in both absolute-RF SDR and sideband-aware
  audio views. Capability-gated Ctrl+click requests the pointed RF on the TX
  VFO through the configured provider and enables split when required; ordinary
  left-click and right-click retain decoder-open and manual-probe behavior.

- SDR center and decoder-center editors now use VFO-style kHz input, such as
  `7021.43` for 7.02143 MHz. Radio Control remains visible during direct-SDR
  reception, allowing an SDR RX endpoint and a separately configured CAT/keyed
  radio TX endpoint to operate as a full-duplex station topology.

- Direct-SDR reception now separates the complete acquired RF overview from a
  bounded CW decoder window. The wide spectrum and waterfall remain visible,
  while one shared tuned, anti-aliased and decimated IQ branch limits the
  sample rate presented to carrier detection and per-stream decoding.

- Settings > SDR now derives sample-rate, hardware RF-bandwidth, antenna/input,
  gain-mode, and gain limits from the selected receiver when its driver exposes
  them. Effective low SDRplay sample rates use the driver's supported
  decimation; RSPduo selector entries are identified as operating modes rather
  than separate physical receivers.

- The direct-SDR spectrum and waterfall support pointer-centred wheel zoom,
  middle-button panning, explicit full-span reset, and a visible overlay for
  the bounded decoder window. Right-clicking an SDR frequency moves that window
  before starting the normal manual CW probe.

- A profile can keep the SDR and decoder window aligned with authoritative RX
  VFO readback from any radio-control provider. An optional signed SDR LO
  offset accommodates independently tuned receiver hardware; the offset is
  bounded so the decoder window remains inside the acquired passband.

- Decoder-card TX and speaker actions now occupy a stable lower action row.
  They remain clickable while live text refreshes, and the speaker remains the
  sole per-stream monitor control; opening a card still never starts audio.

- Live transcripts use a slightly smaller reading font, reserve a scrollbar
  only when needed, append provisional characters immediately, and start a
  clean new line after a sustained transmission pause. Later acoustic
  refinement can still correct the provisional tail without rebuilding the
  card on every character.

- Opening Settings > SDR now performs one receive-only device discovery after
  the page renders. Application and profile startup still never probe hardware,
  and **Refresh devices** remains available for reconnect and hot-plug scans.
  The module list canonicalizes and deduplicates paths reported more than once
  by the runtime.

- The receiver toolbar now exposes only **OFF / RX** for whole-window audio.
  The redundant **STREAM** choice is removed; each decoder-card speaker is the
  sole control for enabling or disabling one or more filtered CW streams.

### Fixed

- Changing only a live SDR center frequency no longer stops and reopens the
  receiver or resets the spectrum. Rapid VFO changes are coalesced and applied
  through the active receive backend with authoritative frequency readback;
  changes that alter the stream format or hardware route still restart safely.

- The Windows COM implementation now keeps the Windows SDK umbrella header
  ahead of `OleAuto.h`, preventing MSVC syntax failures after include sorting.

- Recentring the live-SDR decoder window no longer drops the immediately
  following manual-probe request while the channelizer is resetting. The
  request is retained until the first spectrum from the new IQ slice establishes
  valid frequency bounds, then its decoder card is opened normally.

- Spectrum pointer gestures are now mutually exclusive: plain left opens a
  stream, Ctrl+left sets TX, Shift+left selects the SDR decoder span, right
  probes, and middle-drag pans. Unsupported modifier combinations do nothing,
  while full-span reset is an explicit button action instead of a double-click
  that also fired ordinary left-click behavior. Pointed TX selection has its
  own capability gate (it no longer incorrectly requires RX-frequency
  readback), and OmniRig writes the last authoritatively identified TX VFO
  instead of a transient simplex VFO observed while split is being enabled.

- Selected-stream monitor audio now applies bounded post-filter level
  normalization with fast attack and slow release. This compensates for large
  SDR device/gain-level differences without changing decoder evidence or
  pumping the noise floor during ordinary Morse gaps.

- Material-style padding and the scrolling transcript background no longer let
  the first decoded line touch or cross the decoder-card text-box border.

- Selecting direct SDR reception no longer hides the configured Radio Control
  panel.

- High-rate SDR input no longer drives the overview FFT or every CW channel at
  the full hardware sample rate. Overview frame production is rate-limited and
  the decoder consumes only the configured down-converted window, preventing
  multi-megasample receivers from flooding the UI and building decode latency.

- Decoder-card commands no longer depend on a transient delegate instance
  surviving from mouse press through release. This removes the apparent
  status-dependent speaker control and prevents text updates from swallowing a
  monitor request.

- A newly keyed transmission no longer appears briefly attached to the end of
  the preceding transmission, and unlocked provisional text is no longer
  duplicated when the timing hypothesis settles.

- Packaged SDR module discovery now resolves application-relative directories
  before retaining operator-provided SoapySDR paths. Moving a portable install
  or launching the macOS bundle from Finder therefore does not hide its bundled
  RTL-SDR module or a separately installed vendor module.

- Application and profile startup no longer block while SoapySDR probes USB
  hardware. Direct receiver discovery starts only from the explicit **Refresh
  devices** action in Settings > SDR; live audio remains immediately usable.

- The Windows build-tree UI smoke test now loads the pinned SDR DLL closure
  from its prepared runtime directory. This prevents a missing-DLL loader dialog
  from blocking CTest before the independently verified package-staging step.

- Portable Linux SDR packaging now follows Ubuntu 24.04's `librtlsdr.so.2`
  package ABI and carries libudev's libcap dependency. Windows pinned-runtime
  builds explicitly enable the supported legacy policy floor required by the
  hosted CMake version; diagnostics no longer fail while parsing a native-command
  error message, and both package steps retain bounded logs in SSH status tags.
  Configure-stage failures retain bounded CMake diagnostics through the same
  status channel.
  Windows now resolves and verifies the pinned SoapySDR CMake package at its
  upstream platform-specific `<runtime>/cmake` location rather than relying on
  a transient registry.
  Linux dependency installation now uses bounded APT download retries and
  retains its install log when a repository or mirror fails. Hosted SDR package
  setup reads only the runner's signed Ubuntu source definition, so an unrelated
  preinstalled third-party repository cannot block the release matrix.
  The isolated source-parts directory is explicit and install logging begins
  before source preflight, avoiding assumptions about mirror URI text.

- Hovering a detected-stream label now applies a smaller, less intrusive text
  enlargement, preserving frequency context and reducing overlap with nearby
  stream markers.

- Switching from audio to direct SDR now clears an incompatible full-passband
  loudspeaker monitor selection. Selected-stream monitoring remains available,
  and its downsampler cannot carry a partial sample across source restarts.

- Receiver-annotation reports now score the callsigns actually published at
  each event instead of re-extracting calls from transcript text. Reviewed
  coverage is explicit, withdrawals and transient labels are timestamped,
  uncertain events protect matching tracks without entering accuracy scores,
  and false-publication episodes exclude unreviewed audio.

- Cross-platform CI now retries each platform's diagnostic status-tag push
  with bounded backoff, so a transient Git transport failure after successful
  build, tests, packaging, and artifact upload does not immediately discard the
  otherwise valid platform result.

- Brief spectrum-association dropouts no longer force the event lattice to
  finalize its newest ambiguous Morse symbols. A bounded one-second/six-run
  look-ahead lets later spacing resolve that suffix append-only; sustained
  silence and explicit end-of-input still close the transmission. Callsign
  labels now reconcile the literal and refined transcripts, preventing a
  one-off callsign created only by a reconstructed prosign boundary from
  displacing independently shared evidence.

- **A=B** now writes the exact VFO A/RX whole-hertz value to VFO B/TX. The
  previous path incorrectly passed a whole-hertz integer through the public
  kHz/MHz text-entry API and therefore rejected every synchronization request.

- Continuous-release replacement now retries GitHub asset uploads with bounded
  backoff. This covers the short deletion-propagation window exposed when the
  version manifest was replaced immediately after the platform payloads,
  without advancing `latest.json` or the verified tag until publication is
  complete.

- The transmit-mode faceplate no longer renders an unobserved standby VFO as
  `?`. It now keeps the operator's persisted CW/CW-R target separate from
  provider readback and marks it **CONFIRMED** only after matching hardware
  state is observed. Backends that cannot write or independently read TX mode
  leave the target visibly unconfirmed instead of copying RX state or claiming
  a hardware change.

- Direct-keying worker notifications now carry monotonic revisions, preventing
  a delayed queued snapshot from replacing newer synchronous KEY/PTT state and
  prematurely completing or faulting a transmission.

### Added

- Official Windows packages now carry the MIT-licensed SoapySDRPlay3 bridge
  for direct SDRplay RSP discovery. SDRplay Hardware API 3.15 and its service
  remain operator-installed proprietary prerequisites and are never included
  in CW Buddy. The Windows loader locates that registered 64-bit API runtime,
  while discovery reports a module dependency failure instead of silently
  hiding it.

- Official Windows, macOS, and Linux builds now enable the receive-only
  SoapySDR backend. Windows and macOS packages bundle SoapySDR, SoapyRTLSDR,
  librtlsdr, libusb, required runtime libraries, licenses, and exact build
  provenance. The portable Linux archive bundles the corresponding ELF closure;
  the Debian/Ubuntu package declares its RTL-SDR module dependency. CI loads the
  packaged module and enumerates its factory without requiring attached
  hardware, rejects unresolved build-machine paths, and verifies installer
  contents. SDRplay's proprietary API remains an operator-installed vendor
  prerequisite and is never included in or redistributed with CW Buddy.

- An optional receive-only SoapySDR adapter now discovers installed modules
  and receivers, accepts direct complex-float IQ, reports device overflow,
  pipeline overrun, timeout, invalid-block, and read-error conditions, and
  feeds the existing wide-spectrum detector and per-stream decoder. The SDR
  settings page exposes center frequency, requested sample rate, AGC/manual
  gain, actual availability, and actionable missing-module diagnostics. Raw
  wideband IQ never reaches the loudspeaker; selected-stream monitoring uses
  the existing carrier-following filter and bounded audio conversion.

- The dependency-free receiver core now validates timestamped IQ descriptors,
  sequence continuity, finite samples, and bounded magnitude, supports a
  16,384-point wideband FFT, preserves absolute RF coordinates, and provides a
  selected-channel IQ-to-audio bridge with deterministic tests. The adapter is
  strictly RX-only and exposes no SDR transmit, PTT, or KEY operation.

- Completed transmissions now receive a bounded second timing pass over the
  retained physical mark/gap lattice. Up to nine existing WPM hypotheses are
  compared using aligned acoustic cost and confidence; contradictory,
  immaterial, or committed-boundary-crossing alternatives abstain, while
  provisional live text remains unchanged.

- Completed turns can now carry an exact-run timing fingerprint with dit/dah
  and gap counts/medians, keying weight, normalized mark residual, evidence
  interval, and duration-weighted confidence. Replay and debug-capture output
  expose it for co-channel research, but it contains no callsign, carrier, or
  sender inference and cannot affect decoding or transmission.

- A checksum-bound, sample-indexed receiver-annotation sidecar and replay mode
  now report CER/WER, exact callsign precision/recall, decode latency,
  non-append revisions, and unmatched publications without committing private
  recordings. A separate generated receiver-path holdout gates fading, drift,
  manual weighting, timing jitter, latency, callsign precision, and a no-CW
  hard negative; its current CER 0.458, WER 0.786, and callsign recall 0.333
  explicitly record the decoder's remaining immaturity.

- A generated production-path decoder quality gate now enforces normalized
  character and word error, exact callsign precision/recall, provisional and
  stable decode latency, verified-stream acquisition latency, and zero false
  stream or callsign publications during a deterministic no-CW minute.

- The independent cadence estimator now pairs each mark with its immediately
  following gap. Their total is insensitive to ordinary hand-key weighting,
  reducing mean WPM error from 1.366 to 0.418 WPM across the weighted-keying
  fixture while leaving its character error unchanged. This improved estimate
  is limited to reported/per-sender cadence; established filter, recovery,
  lattice, verification, and callsign decisions retain the separately measured
  mark/gap control estimate.

- Per-hypothesis decoder updates now retain stable dynamic transcript and
  character storage between Morse boundaries instead of deep-copying it at the
  500 Hz evidence cadence. This removes nine steady-state deep copies per track;
  an isolated two-million-frame measurement reduced that inner operation by
  70.7%, while the outer presentation snapshot remains unchanged.

- A provider-neutral Hamlib rigctld adapter now reads and capability-gates
  independent RX/TX frequency, mode, VFO, and split state, and routes the same
  operating-panel commands as OmniRig and CAT4OM. It requires rigctld VFO mode,
  treats only complete poll readback as authoritative, accepts loopback
  endpoints only because the raw protocol has no authentication or TLS, and
  deliberately exposes no PTT or KEY command.

- Direct-keying validation now performs a real bounded electrical loopback on
  the exact selected serial port. After explicit radio-disconnected
  confirmation it verifies inactive lines, RTS→CTS and DTR→DSR independently,
  releases KEY before PTT, and stores only a platform/configuration-bound
  SHA-256 fingerprint and timestamp. The former writable acknowledgement
  checkbox can no longer enable hardware.

- Active message and TUNE operations now show authoritative elapsed time,
  remaining time, and progress from the worker schedule. Report and exchange
  fields plus four profile-configurable quick macros all prepare the existing
  exact-confirmation preview and never send automatically. While TUNE is
  active, its orange faceplate tile shows the rounded-up watchdog countdown;
  pressing it again still releases KEY and PTT immediately.

- Guarded direct CW transmission now connects the operator-confirmed QSO flow
  to the dedicated RTS/DTR adapter. Arming requires a measured physical
  loopback plus provider-confirmed TX frequency, CW/CW-R mode,
  and split state; changing any captured station or keying condition disarms or
  emergency-releases. Confirmed own-call, editable-report, and free-text plans
  run at fixed or selected-stream RX-adaptive WPM, remain synchronously
  cancellable, and preserve KEY-before-PTT release ordering. Operator TUNE uses
  the same boundary with an independent, non-extendable 15-second deadline.
  Hardware remains opt-in and first acceptance is documented for a dummy load.

- The radio faceplate aligns both frequency displays against equal-sized,
  fixed right-hand RX/TX mode controls. The orange TUNE tile sits directly
  beneath the borderless ON AIR status lamp, while SIMPLEX/SPLIT and A=B are
  centered in the remaining lower-row space. ON AIR remains driven only by
  authoritative KEY state.

- A provider-neutral **A=B** control copies the checked VFO A/RX actual-RF
  frequency to VFO B/TX through advertised TX-frequency and split
  capabilities. Its core availability check fails closed on invalid or unknown
  provider state. It deliberately does not copy RX mode into the CW/CW-R TX
  target.

- The radio faceplate now uses equal 52-pixel control tiles, a substantially
  smaller ON AIR symbol, compact two-line TX labelling, and a protected
  frequency readout that scales its digits instead of replacing them with an
  ellipsis at the minimum decoder-pane width. Both readouts accommodate
  grouped frequencies through 99 GHz. The right column now presents Radio
  Control and CW Decoder as separate sections, with Diagnostics and Debug
  capture owned by the decoder header. RX and TX mode occupy the same rightmost
  column in their respective VFO rows. The guarded TUNE control is also
  available as an equal-sized Radio Control tile and retains its explicit
  arming requirement and hard 15-second watchdog.

- The secure remote-operation specification now defines mutually authenticated
  TLS 1.3, local pairing, unique client credentials, station pinning, strict
  per-message authorization, encrypted bounded media, replay protection,
  fail-safe remote TX and a station-wide exclusive control lease. Multiple
  authenticated receive-only clients may operate concurrently, while exactly
  one lease holder can change any coupled station resource.

- Receiver monitoring now distinguishes **All RX** from **Stream** without
  coupling listening to decoder-card selection. Opening a detected stream only
  opens its card. Each card has an independent speaker control, and several
  selected streams can be mixed after their carrier-following narrow filters
  reject adjacent audio and re-pitch each carrier to the configured CW tone.
  Stream mode with no selected speakers stays silent; disabling the final
  speaker returns monitoring to Off. The Audio settings select the PC output
  device and the receiver toolbar sets level without allowing queued audio to
  grow without bound.

- The application header now presents the `CW BUDDY` wordmark with
  `by IU0LFQ`, and exposes the compiled application version in both the header
  and native window title.

- The first guarded transmit-workflow slice adds an operator-facing TX/QSO
  drawer, exact callsign and normalized-message confirmation, free-text/own-call
  and report preparation, standard 5–80 WPM Morse timing plans, inert Auto-QSO
  suggestions, emergency release, and a distinct TUNE safety state with a hard
  15-second continuous-KEY limit. A worker-thread transmit scheduler and direct
  RTS/DTR KEY/PTT adapter now have deterministic fake-backend tests, safe
  inactive opening, ordered release, watchdog, and fault handling. Decoder
  output can only propose text. The application controller does not yet connect
  this adapter to operator actions, so no message or TUNE action can key
  hardware in this build.

- A selected station with checked absolute-RF context can now be explicitly
  anchored to the configured CW reference tone from the QSO drawer. The action
  uses the existing provider-neutral, capability-gated RX-frequency route and
  leaves TX frequency, split, and mode unchanged, placing an ordinary UP pileup
  to the right of a runner centered at the default 700 Hz.

- A verified simplex stream now recognizes the high-confidence
  `CALL1 DE CALL2` handover as one two-party QSO on one carrier. The decoder
  card retains both participants and labels the session `QSO CALL1 ↔ CALL2`
  instead of incorrectly treating one frequency track as one station. The
  rule requires two complete, distinct, structurally plausible callsigns and
  does not guess which participant is currently sending.

- Decoder tracks now retain bounded transmission turns after sustained silence
  or an explicit end-of-input boundary. Explicit `CALL1 DE CALL2` and
  `CQ ... DE CALL` evidence can identify the current sender, while bare calls,
  incomplete `DE CALL` fragments, and conflicting final paths deliberately
  leave it unknown. Up to eight identified senders retain independent cadence
  summaries; a well-supported earlier cadence may only nudge a later timing
  candidate from the same sender when the live acoustic estimate already
  agrees with it.

- Completed turns receive a separate contextual presentation that can choose
  only among acoustically competitive alternatives with the same decoded
  characters. It repairs a bounded set of missing word gaps around common CW
  exchange forms and separates turns with `|`; raw and phase-consensus text,
  character evidence, callsign confirmation, and verification remain
  unchanged. Debug capture now includes the retained turns and explicit sender
  evidence.

- A recognised calling or contest token is now evidence that a channel carries
  real Morse. A station whose decoded text plainly reads `TEST` was being
  discarded because one timing measure sat under its threshold for the track's
  whole life -- nothing else about it was in doubt, it had a carrier, keyed
  edges, cadence and coherence. Such a token may now satisfy the three gates
  that judge how good the decoded characters are. It deliberately cannot satisfy
  the requirement to have decoded enough of them, and cannot be reached at all
  before the acoustic gates have passed, so it can never verify a silent
  channel. The list is short and skewed to tokens noise is unlikely to spell by
  chance -- `CQ`, `TEST`, `599`, `5NN`, `QRZ`, `TU`, `UP` -- matched as whole
  words only; `K`, `DE` and single letters are excluded however useful they are
  to a human reader, because the point is evidence rather than readability.

  Measured across all twenty-two captures: callsign recovery unchanged at eight
  of nine, no false callsign on the four recordings containing none, and
  published tracks identical on every capture but one, where a garbage fragment
  merged into a real station's identity instead of standing as its own track.
  The original failure no longer reproduces on this corpus, so the path stands
  as a safety net rather than a fix, and the verification diagnostics now count
  how often it is actually needed.

- The operator's role is configurable, and it decides whose callsign a stream is
  expected to carry. Exchange context alone cannot always tell: `TU` precedes a
  runner identifying itself and equally the station it has just worked, and both
  score the same, so a run could be labelled with the station that was worked
  rather than the one being listened to. Settings -> Station now offers
  Monitoring (the default, which assumes nothing), Search and pounce, and
  Running. Hunting, the stream is a runner, so an unambiguous runner context
  outranks `TU`; running, the stream is somebody answering, so a repeated bare
  call outranks one introduced by a `CQ` belonging to another transmission.
  Measured on exactly the ambiguous case: `5NN TU DL1NKB OK5OO UP K` labels
  DL1NKB, the station just worked, with no role, and OK5OO, the split runner,
  when hunting.

  In every role the operator's own callsign is now removed from candidate
  scoring rather than only blanked afterwards, so a transmission that mentions
  it still gets labelled with the station actually being heard instead of losing
  its label entirely.

### Changed

- Experimental CW likelihood-model training utilities are no longer part of
  the distributed source tree or release CI. The optional, sandboxed runtime
  boundary for an operator-supplied local model remains available, and the
  deterministic decoder remains the default.

- Small bounded presentation-frequency samples now use an explicit
  bounds-safe prefix sort, and aggregate test fixtures initialize owned
  containers explicitly. This keeps the dependency-free core and tests clean
  under the stricter GCC 16 warning analysis without changing decoder results.

- Right-click manual decoding no longer moves the independent red CW guide.
  It creates a temporary decoder region at the pointed frequency; measured
  carrier associations move its DSP and displayed centers through the same
  bounded tracking logic as automatic streams. An unverified region expires
  after the configured decoded-stream timeout, while a verified region is
  promoted in place and follows normal stream retention.

- The decoder pane now uses a compact radio-style VFO faceplate with grouped
  whole-hertz RX/TX digits, ON AIR state, and explicit VFO, SIMPLEX/SPLIT, and
  provider-reported RX/TX modes. A dependency-free radio-control contract keeps
  unavailable state explicit and capability-gates independent RX/TX frequency,
  mode, and split writes. Windows OmniRig and CAT4OM map their authoritative
  state and supported writes into that shared contract; unsupported providers
  remain visibly read-only. The main
  window opens maximized, the decoder pane has more room, spectrum hover shows
  the left/right/Ctrl pointer actions, and every push button in the main,
  settings, setup, and profile views has contextual hover help.

- The radio faceplate now retains the provider's independent standby/transmit
  VFO frequency in simplex instead of replacing it with the RX frequency.
  CAT4OM always presents its explicit TX VFO state. Because OmniRig exposes
  only one mode property, CW Buddy keeps separate last-observed A/B modes and
  leaves a never-observed inactive VFO unknown instead of copying the active
  VFO mode into it. Enabling split therefore preserves real per-VFO state and
  cannot manufacture a mode.

- Live decode workers no longer rebuild the complete, deeply nested decoder
  snapshot for each character-model window. A bounded lightweight refinement
  view carries only the lane evidence the asynchronous model frontend needs,
  while the full operator snapshot is still rebuilt for presentation.

- The TX activity indicator uses a compact transparent ON AIR mark and remains
  dim unless the guarded hardware state reports that KEY is actually asserted.

- The CW Buddy application and README icon now use true transparency outside
  the rounded-square edge, so launchers and light backgrounds no longer print
  or display opaque black corners. The PNG, Windows ICO, and macOS ICNS assets
  share the same alpha-aware master.

- The application is now **CW Buddy** throughout its public identity, including
  the desktop UI, executable and package names, installer text, update assets,
  documentation, and repository links. The new project artwork is used by the
  application and shown on the repository home page. Existing profiles and the
  managed Super Check Partial cache are imported on first launch; Windows and
  macOS retain their established upgrade identities, and the Debian package
  replaces the former package instead of installing a duplicate.

- Keying-level estimation now combines the responsive per-frame tracker with a
  bounded 512 ms amplitude history. A robust two-class split may anchor the
  space and mark levels only when both populations contain enough samples, are
  separated by at least 6 dB of power, and explain at least 70% of the observed
  variation. Ambiguous edge samples therefore no longer make the two learned
  levels drift together, while fading and manually weighted sending still use
  the responsive tracker.

  On identical, cross-platform reproducible generated audio, full-surface mean
  character error falls from 0.3070 without the robust history to 0.2579 with
  it; all three seed sets improve and the aggregate paired gain is 0.0491.
  Wrong callsign assertions on that surface fall from 40 to 36. The receiver
  corpus remains at eight of nine corroborated
  callsigns and zero callsign assertions on all four recordings containing no
  CW. Debug captures now include the level separation, explained variation,
  and whether the robust anchor was accepted, so future changes can be judged
  from evidence rather than transcript appearance.

  The benchmark noise generator now derives its Gaussian-like samples directly
  from the standardized `mt19937` integer sequence. `normal_distribution` does
  not define the same mapping across standard libraries and had silently given
  MSVC, libc++, and libstdc++ different acceptance waveforms for the same seed.

- Debug capture is reachable from Settings -> Decoder, not only from the decoder
  panel header, with a button that opens the capture folder in the file manager.
  A capture is only useful once it has been found and reviewed, and a path shown
  as text still had to be copied out by hand. Its auto-stop is now a persisted
  setting between 30 and 1800 seconds instead of a fixed five minutes: a signal
  that misbehaves only occasionally cannot be caught inside five minutes, while
  a quick reproduction should not leave a needlessly large recording to review
  before sharing. The decoder-panel status line reports the configured limit
  rather than advertising 300 seconds.

- The decoder's keying technique is selectable, and a second one is offered
  beside the shipped default. Settings -> Decoder now names the two: **Adaptive
  threshold**, which decides key-up and key-down from the envelope moment by
  moment and classifies elements afterwards by duration ratio, and **Semi-Markov
  (HSMM)**, which weighs each mark and gap against the lengths Morse expects,
  scoring whole runs rather than single frames. The default is unchanged, and a
  decoder left on it behaves exactly as before -- measured, not assumed: the
  synthetic accuracy surface reproduces 0.2884 mean character error to the
  digit, capture recovery stays at eight of nine callsigns, and no false
  callsign appears on any of the four recordings that contain no CW.

  Neither technique is better everywhere, which is why this is a choice rather
  than a replacement. Against the keying-style bench the duration model is
  better when the sender is systematically off the textbook ratios -- 0.058
  against 0.133 on heavy weighting, 0.075 against 0.100 on Farnsworth spacing --
  and worse when their timing wanders, 0.283 against 0.075 at ten per cent
  jitter and 0.458 against 0.267 at twenty. Hand and bug sending produce exactly
  that wander, so the threshold remains the default; a machine-sent or heavily
  weighted signal is where the other is worth reaching for. Switching models
  restarts the decoders but keeps every track, so two techniques can be compared
  on the same station while it is still sending.

  The choice is stored by name rather than by position, so it survives another
  technique being added or the list being reordered, and an unrecognised stored
  value falls back to the default rather than to whatever happens to be first.

- Morse timing is self-similar at a factor of three, and a decoder that models
  durations explicitly has to be told so. Read three times too fast, every dash
  becomes a dot and every character gap becomes a gap between elements: the
  result is legal Morse, composed entirely of known symbols, fitting its own
  duration model perfectly. A hypothesis at 44 WPM produced "E ?TMT MMTM MTMT"
  from a clean CQ at 16 WPM and was preferred to the truth. What separates them
  is that a real dash is longer than the longest mark such a hypothesis can
  describe, so it must break solid marks apart to make the interval tile at all
  and pays for every invented gap. Speed hypotheses are scored on that fit,
  which took the affected cells from 0.283 to 0.092. The threshold decoder is
  unaffected -- at the wrong speed it emits unknown symbols, which already lose
  -- so the term applies only to a technique that needs it.

- The verification timing-quality threshold is a plain constant again. It had
  been left parameterised by a build-time define after an experiment, so a build
  that happened to set that name would have silently changed which signals the
  application accepts as CW. The shipped value is unchanged.

- The optional local model no longer reports an error for a model that was
  never configured. Enabling it without selecting the files still attempted a
  load, and an empty path failed the metadata check as though the file were the
  wrong kind or too large, so the card read "metadata must be a regular JSON
  file no larger than 64 KiB" for a model the operator had not chosen. An
  enabled but unfinished setup now says so and loads nothing, and its panel is
  hidden entirely while the feature is not in use rather than occupying every
  card to report a state the operator has no interest in. The four genuine
  metadata failures -- a missing file, a path that is not a file, an empty file
  and one past the size limit -- each state their own cause.

- Farnsworth spacing no longer splits every character apart. The decoder scored
  the gaps between symbols against fixed centres of one, three and seven
  element lengths, but Farnsworth sending keeps element timing at the operator's
  speed while stretching the character and word gaps by a common factor. A
  stretched character gap therefore sat closer to the word-gap centre than the
  character-gap one and was read as a word gap, so the consensus transcript --
  which the decoder card prefers -- broke a callsign into single letters while
  the literal transcript beside it read the message correctly. On a controlled
  fixture its character error fell from 0.56 to 0.03, from 0.55 to 0.06 and
  from 0.52 to 0.09 as the spacing was stretched further, and on a receiver
  capture a callsign that had been shattered into "R 7 K B ?" reads "R7KBB".
  The factor is recovered from the gaps themselves: ordinary text contains far
  more character gaps than word gaps, so the median gap clearly longer than an
  element gap is a character gap. Nothing adapts without at least six confident
  gaps, and only within the range real sending occupies; outside that the
  standard centres stand.
- Decoded session cards are reordered by dragging them. The up and down arrows
  are replaced by a handle: the card lifts while it is moved and takes its new
  position on release. The list itself still owns placement, so nothing is
  reparented. The same handle keeps arrow-key reordering when focused, so the
  order remains reachable without a pointer.

- A stream shows no speed until enough symbols support one. Below a few decoded
  symbols the timing bank has nothing to choose between its hypotheses: the
  value is still the seeded default, and whichever anchor briefly leads can be
  far from the truth. On a receiver capture a 27 WPM station read 20 WPM before
  anything had been decoded and reached 40 WPM on its fourth key transition,
  which is what made the displayed speed appear to wander. Across that
  recording six of fourteen sampled readings were unsupported and are now
  withheld, and the range shown narrows from 20 to 28 WPM down to 25 to 28. The
  decoder itself continues to adapt exactly as before; only the presentation
  waits for evidence.

- The signal level shown for a stream is the mark level, not an instantaneous
  reading. A keyed carrier is present only half the time, so the instantaneous
  figure swings between roughly +28 dB inside a mark and below zero inside a
  gap, and whichever value the display happened to sample told the operator
  nothing. On a receiver capture a perfectly readable station reported -6.3 dB
  because the sample landed in a gap; it now reports +31 dB.
- The decoder metrics line is legible while operating. It was ten-pixel
  low-contrast grey on a single elided row, so its values were cut off, and it
  reported instantaneous confidence, which falls to zero between characters and
  therefore read zero per cent while text was arriving. It now uses the
  character-averaged confidence, wraps instead of eliding, and drops the
  instantaneous key percentage, which only ever flickered and is already shown
  by the keyed marker.
- A prosign glued to the callsign after it no longer becomes the station label.
  A missing word gap merges the two, and the merged token then collects the
  context credit the callsign earned: a station sending CQ CQ CQ DE SV7BIO
  decoded as "CQ CQ DESV7BIO SV7BIO" was labelled DESV7BIO even though the real
  callsign stood alone twice in the same text. The pair is split only where
  what follows the prosign is itself a plausible callsign, so a genuine
  DE-prefixed German call is untouched: removing DE from DE1ABC leaves 1ABC,
  which is not a callsign, and the token stands.

- Local callsign suggestions can maintain an optional cached `MASTER.SCP`
  directly from the Super Check Partial Database. Provider discovery and
  conditional HTTPS checks run no more than daily unless requested by the
  operator, changed data is validated before atomic replacement, and every
  failure preserves the last valid offline copy. Operator-selected files remain
  supported, while suggestions retain separate advisory-only `≈`/`DB`
  provenance.
- Settings can load an operator-supplied offline `master.scp` or Call History
  text file. A database call is shown only as an explicitly marked advisory
  suggestion when at least two current bounded acoustic alternatives contain
  that call and it fits a completed uncertain call-shaped span. It
  never replaces transcript text, confirms a stream, or performs a network
  query.

### Fixed

- Missing word spacing no longer lets common leading prosigns such as `DE` or
  `CQ` become part of a stream's callsign label. The split is accepted only
  when the remaining token is itself a plausible callsign, preserving genuine
  `DE`- and `TU`-prefixed calls. The near-miss correction control is now beside
  the callsign-list state and explains when no ready database is available.
- The startup application-update notice now reports download and checksum
  verification status. After verification, **Open Installer** and the
  platform-specific reveal action replace **Download update** in place, just
  as they do in Settings → About.
- Corrected the keying decision that limited character accuracy at every speed
  except a narrow band around 20 WPM. The decision was taken on a decibel-domain
  envelope at a fixed fraction of a floor-to-peak span, which sits far below
  half amplitude in linear terms, so every mark was measured long and every gap
  short — and the error grew as the signal got stronger, because a stronger
  carrier widens the span. Keying is now decided in the linear power domain at
  the half-amplitude point between an estimated space level and mark level.
- Evidence smoothing now scales with the element length each timing hypothesis
  is tracking instead of using one fixed 12 ms constant across the whole 8-60
  WPM range, where it was 8% of a dit at the slow end and 60% at the fast end.
- Impulsive noise is now rejected by duration rather than by a wide decision
  hysteresis. Separating the two lets the amplitude decision sit on the real
  edge, and it is what allows irregularly keyed noise to be told apart from CW.
- Character and word boundaries are no longer detected late. A still-open gap
  was timed against the wall clock while its start edge had been observed one
  smoothing constant after the signal actually changed.
- Raised the verification timing-quality floor from 0.45 to 0.55. It had been
  calibrated against the earlier biased measurement and sat directly on top of
  the irregular-impulse hard negative; corrected timing puts real CW at
  0.85-0.98 and that negative at about 0.44, so the threshold now sits in the
  gap between them.
- Cadence and lattice evidence now follow the current presentation leader.
  Hypotheses previously shared one key decision, so the first seeded (slowest)
  one could be sampled; they now smooth against their own element lengths.

  Measured against a character-error-rate surface driven from synthesised audio
  through the production path (seven speeds from 12 to 50 WPM by four
  signal-to-noise ratios), mean character error falls from 0.713 to 0.653, and
  at 30 dB from 0.363 to 0.053. Acquired-speed error falls from as much as 23%
  to at most 4.2% across the 8-55 WPM benchmark. The deterministic timing
  corpus keeps zero character edits, zero refined edits and no speed failures,
  and the hard-negative corpus keeps three of three acquisitions with zero
  false publications. On the available receiver captures a previously confirmed
  but incorrect callsign is no longer presented.

- Narrowband analysis width is now chosen from the keying bandwidth a signal
  actually needs — about 3.5 element rates — instead of from comparative filter
  power. The narrowest 60 Hz path is additionally restricted to genuinely slow
  signals: its three cascaded sections delay the envelope by 15.9 ms, a sixth
  of a 12 WPM element but a quarter of a 20 WPM one, so selecting it for
  ordinary speeds rounded real elements together. On receiver captures that
  cost one recording its callsign entirely and made another report a wrong one.
- The keying decision band now adapts to measured keying contrast. A clean
  signal is sliced tightly for accurate edge timing, while a weak one widens
  the band, trading some edge precision for noise immunity rather than
  chattering on a noisy envelope.

  Across the audio-driven speed and noise surface, mean character error falls
  from 0.653 to 0.621. On receiver captures a repeated callsign that was
  previously garbled on its second appearance now decodes correctly, and a
  second capture's repeated exchange is materially cleaner. The deterministic
  timing corpus keeps zero character and refined edits with no speed failures,
  and the hard-negative corpus keeps three of three acquisitions with zero
  false publications.

- The element-length estimate is no longer biased by the operator's keying
  weight. It was derived from mark durations alone, but at keying weight w a
  dit lasts w elements while the gap after it lasts (2-w), so lightly weighted
  and bug-style sending pulled the estimate short and every gap then read long.
  The estimate now uses the mark together with its following element gap, whose
  sum is independent of weight. Measured at 20 WPM and 20 dB, bug-style sending
  (0.8 weight with 15% jitter) falls from complete failure to 0.61 character
  error and becomes partially readable, Farnsworth spacing falls from 0.56 to
  0.31, 10% timing jitter from 0.33 to 0.17, and lightly weighted sending from
  0.59 to 0.48. The paired estimate adapts at 70% of the former rate, because
  summing a mark and its gap also sums their measurement noise; without that
  damping the light-weighting case came out worse than the biased estimate it
  replaced.
- An adaptive word-gap classifier, learning the character/word boundary from
  the observed gap population rather than a fixed multiple of the element, was
  prototyped and rejected. It improved Farnsworth spacing on synthetic timing
  but degraded the receiver captures and asserted a callsign on a capture
  independently established to contain no CW. Farnsworth spacing remains an
  open limitation.

- The offline callsign list can now correct a decoded callsign, not merely
  confirm one. Its fuzzy lookup existed and was tested but nothing in the
  application ever called it: the only use was an exact-membership check that
  labelled a suggestion's source. Where a verified stream's acoustic winner is
  within two characters of a single listed entry, that entry can now be
  suggested instead, which is what recovers a callsign whose opening characters
  were lost -- the acoustic path routinely loses them, because element
  boundaries must be committed before any speed estimate exists. Correction is
  off until enabled under Settings, Decoder, Callsign directory: two listed
  stations can differ by one character, so a correction can name a station that
  was never sent. It stays advisory in either case and cannot rewrite the
  transcript, become the confirmed callsign on its own, or affect verification,
  and it never substitutes where more than one entry is equally close.
- Every diagram in the documentation is now rendered rather than drawn in ASCII (DOC-002).
  The layer stack, realtime data flow and transmit-safety states in the
  architecture document, the decoder pipeline in the decoder strategy, and the
  renderer node tree in the first design decision are Mermaid: the source stays
  in the document where it can be diffed and edited, it renders as vectors in
  GitHub and the common documentation viewers, and it needs no build step or
  checked-in binary. Each keeps a prose description of the same content beside
  it so a terminal or screen reader loses nothing.
- A station calling the operator is now visible on the spectrum. Where a
  stream's text contains the configured own callsign, its marker blinks and
  reads CALLING YOU. The application already flashed an opened decoder card for
  this, which cannot draw attention to a call the operator has not found yet.
- The operator's own callsign is no longer used as a stream label. It reaches
  decoded text whenever somebody calls the operator -- a caller sends it before
  its own -- so a pileup answering the operator could label every stream with
  the operator's own call. A stream now stays unlabelled until the calling
  station identifies, which is the honest answer.
- The decoder benchmark also reports how often a callsign is asserted wrongly.
  Naming the wrong station is worse than naming none, because an operator logs
  what the application asserts, and nothing measured this: the quality checks
  counted callsigns recovered and false callsigns on silent recordings, so a
  wrong station named on a live signal was invisible. It is not rare -- six
  assertions in thirty-three across the surface name a station that was never
  sent -- and it cannot be scored away, because a mis-decoded token sitting
  where a callsign belongs scores exactly as a correct one does. The offline
  list is what distinguishes them, which is what the LISTED and DECODED badges
  report.
- A benchmark reports character error over the speed and noise surface together
  with its own repeatability. The figure moves by a few hundredths with the
  noise draw while being deterministic per seed, so it prints the spread across
  independent seed sets and states that a smaller difference is not a result.
  Two builds must be compared on the same seeds and judged on the paired
  difference.
- A confirmed callsign now shows whether the offline list corroborates it. The
  decoder card carries a green LISTED badge with a tick where the callsign is
  in the list and a plain DECODED badge where it is not, and the spectrum draws
  a corroborated callsign as a solid chip with a tick. Neither is shown when no
  list is loaded, and DECODED is not a warning: an unlisted station is ordinary,
  so the badge reports corroboration rather than correctness.
- Pending updates are reported at startup. A newer application build or a newer
  callsign list is listed once per launch with a button for each; nothing is
  downloaded until the operator presses one, and the notice waits for any
  first-run profile or setup step to finish.

- Gap classification no longer splits characters that were never spaced apart.
  The threshold separating an element gap from a character gap sat close to the
  midpoint between their nominal lengths, which is where it belongs only if
  gaps are measured cleanly. They are not: a noise excursion inside a gap
  registers as a mark and eats into it from both ends, so measured gaps run
  short and a midpoint threshold breaks characters apart. Placing it nearer the
  nominal character gap costs nothing on unambiguous spacing and recovers
  those characters, lowering mean character error by 0.024 across the speed and
  noise surface.

- The decoded transcript no longer shudders while text arrives. Each update
  replaced the whole text document, which discarded its layout and reset the
  viewport, so the card showed a stale scroll offset for one frame on every
  decoded character and only jumped back to the bottom afterwards. New
  characters are now inserted as a suffix, leaving the existing layout and
  scroll position untouched, and a transcript that is following the tail is
  pinned to the bottom in the same frame its content grows rather than a frame
  later. The local model transcript in the same card had the same defect and
  is fixed with it.

- The keying decision is now a soft one. The detector previously shaped its
  amplitude reading into an evidence ramp with a hand-set slope, and the timing
  decoder squashed that ramp a second time to get a probability, so the number
  it worked from was not a measure of anything. The detector now states a
  log-likelihood ratio between its mark and space hypotheses, each level
  carrying its own measured scatter, and encodes it so the decoder recovers a
  calibrated posterior directly. The slope is therefore measured rather than
  chosen: it steepens when the two levels separate cleanly and falls to zero --
  meaning no information -- when they do not, which is the honest reading for a
  channel carrying no CW.

  Because a mark carries signal plus noise while a space carries noise alone,
  the two levels scatter differently, and the decision consequently sits nearer
  the mark than half amplitude. That is where the weak-signal gain comes from:
  noise excursions stop producing marks. The one thing that must never happen
  is the shift running the other way, so the space's scatter is held to no more
  than the mark's. A keying edge sweeps through both levels and can briefly
  inflate the space estimate past the mark's, and unconstrained that mistimes
  every element badly enough to lose the message entirely.

  Mean character error over the speed and noise surface falls from 0.507 to
  0.307. The gain is concentrated where copy was worst: averaged across speeds,
  error at 20 dB falls from 0.389 to 0.136, at 15 dB from 0.619 to 0.338, and
  at 12 dB from 0.915 to 0.719. Keying style improves throughout, with mean
  error across the styles falling from 0.296 to 0.108 -- a light-weighted fist
  from 0.442 to 0.117 and a bug-like fist from 0.592 to 0.117 -- and six of the
  seven styles now read the callsign correctly on both repetitions. Receiver
  captures hold at eight of nine corroborated callsigns with none asserted on
  any of the four captures containing no CW, and output remains identical
  across a 52 dB span of absolute input level.

- Detection no longer shares state with the spectrum display. It reads the
  unaveraged bins and applies its own smoothing over a fixed time constant, it
  runs on its own cadence rather than once per displayed frame, spectral
  persistence accrues from elapsed time rather than per frame, and the receive
  workers reset the decoder only when the audio reaching the detector actually
  changes. Previously the **Avg** control and the display line rate both
  changed decoded output — on one receiver capture the line rate changed the
  callsign shown — and every display adjustment discarded all tracks,
  transcripts and confirmed callsigns while the workspace kept showing the
  previous channels.

  This was first attempted earlier and withdrawn because it destabilised the
  hosted live-audio acceptance test: element timing was then sensitive enough
  that a few milliseconds of change in candidate admission selected a different
  speed hypothesis for a whole segment. With the timing corrections since, that
  sensitivity is gone and the change now improves every measure. Mean character
  error over the audio-driven speed and noise surface falls from 0.545 to
  0.507. Across the receiver captures, eight of nine externally corroborated
  callsigns are now recovered against six of eight before, and none of the four
  captures containing no CW asserts a callsign. Two recordings previously
  believed to contain nothing in fact carry traffic that was being lost
  entirely; one of them repeats a plain CQ call whose station identifier the
  application's own capture-time diagnostics independently confirm.

- Offline callsign suggestions now tolerate at most two acoustically supported
  substitutions, insertions, or deletions between a completed uncertain span
  and the current N-best callsign winner. Two current paths must still agree,
  ambiguity causes abstention, and a stronger acoustic winner absent from the
  database is labeled `AUDIO` instead of being displaced by a weaker database
  entry. Decoded text remains unchanged.
- Replacement trackers retain bounded transcript and color continuity but no
  longer inherit the predecessor's confirmed callsign. The new acoustic source
  must establish its own station identity.
- Debug captures now state whether decoder tracks existed before recording
  began and include local-model/database state plus each callsign suggestion or
  its rejection reason, without publishing that diagnostic traffic while
  capture is inactive.
- Hosted build-failure markers now preserve bounded compiler/linker error lines
  separately from the trailing parallel-build output, so a later successful
  target cannot hide the actual failing target from Git-only diagnostics.
- The callsign-evidence regression test now includes its standard algorithm
  dependency directly, restoring MSVC compilation without relying on
  toolchain-specific transitive headers.
- The managed-database startup contract test now normalizes Windows CRLF
  checkouts before matching its network-suppression guard.
- Decoder cards now show the append-only phase/timing consensus as soon as it
  is available, retain the literal decoder only as an acquisition fallback,
  wrap at decoded word boundaries, and derive their clipped height from their
  real content so transcripts and local-model status cannot escape the card.
- Completed transmissions now finalize the timing lattice's best bounded path
  instead of dropping an unresolved competitive suffix. A modestly lower
  evidence floor is permitted only at that explicit boundary; continuous
  noise keeps the stricter gate. On the reported receiver capture this changes
  the deterministic correction from `T90 ` to a spaced transcript containing
  `T90ZMV PSE K`, `DF6`, and both `5NN` reports.
- Overlapping local character-model windows may now confirm a moderately
  confident character after three independently aligned windows. This recovers
  repeated model evidence such as `EM90ZMV` without duplicating the overlap as
  `EM9090ZMV`, while two moderate windows remain provisional.
- Ordinary-QSO callsign labeling now recognizes a complete acoustically
  decoded call before `PSE K`, `K`, `KN`, `AR`, or `SK` as supporting context,
  in addition to CQ/DE/TU/UP and exact repetition. Context ranks existing text;
  it never invents or replaces decoded characters.
- Fixed the RX-frequency editor reopening or remaining active after Enter or
  Escape. Losing focus now cancels editing, and the display/edit states use a
  dark, high-contrast monospaced LCD-style panel.

### Added

- Added operator-controlled RX tuning for linked writable radios. Click the
  large RX readout to enter an exact frequency, or use stable `<` / `>`
  controls at the waterfall edges to move by a profile-persisted 1–100 kHz
  step (1 kHz default). Actual RF entries are converted back through the
  configured RX transverter offset with checked integer arithmetic. CAT4OM
  explicitly targets its active RX VFO; Windows OmniRig selects its writable
  active `Freq`, `FreqA`, or `FreqB` property only while the rig is online and
  receiving. Split TX frequency, mode, PTT, and KEY are never changed by these
  controls, and read-only or unavailable providers expose no active tuning
  target. OmniRig frequency readback stays responsive while its additional
  capability/VFO discovery is rate-limited; every write still rechecks the
  complete live safety state immediately.

- Added an optional, CPU-only local character-refinement decoder for
  operator-supplied ONNX models and metadata. Up to four verified,
  Morse-likely, or manually selected tracks receive independent 30–50 Hz
  lanes; eight-second overlapping feature windows run on a bounded,
  coalescing inference worker and timestamp consensus produces a separate
  append-only transcript. An overlap-confirmed structurally valid callsign may
  complete verification of a carrier that already passed spectral, keying,
  cadence, coherence, and sustained-entry qualification; it cannot create a
  carrier from model text, replace raw decoder text, or initiate transmission.
  The separately marked model call may appear on the verified decoder card.
  Bounded silence grace preserves slow manual word gaps, while
  reset/reconfiguration cancels stale inference and clears retained card text.
- Automatic peak tracking now reserves at most one decoder identity in each
  configured separation cell across spectrum frames. Keying sidelobes reuse or
  defer to that identity instead of filling the bounded bank with clones, and
  verified identities take reservation priority. Local character lanes follow
  the robust presentation center rather than instantaneous DSP-center walks.
- Added strict optional ONNX Runtime 1.28.2 packaging for native Windows,
  Linux, and macOS builds, including platform runtime layout, license/notices,
  model/metadata/tensor validation, malformed-output bounds, and deterministic
  enabled/disabled integration tests. Runtime telemetry is explicitly disabled,
  source-built packages retain the upstream privacy notice and required
  major-version runtime aliases, and no character model is bundled or
  downloaded by the application.

- Added an independently implemented experimental CW likelihood toolchain:
  deterministic synthetic receiver audio with exact key-run labels and
  checksummed leakage-safe splits, a compact stateful causal GRU that predicts
  key-down and target-channel-CW probabilities (not characters), temporal and
  hard-negative evaluation, ONNX export equivalence checks, and deterministic
  anti-aliased WAV inference. No model artifact or inference runtime is bundled
  or enabled in the application until held-out receiver accuracy and packaging
  gates pass.
- Added a dependency-free streaming probability-to-Morse boundary. It debounces
  isolated probability spikes, converts confirmed transitions into immutable
  evidence runs, preserves observation provenance, and exposes bounded N-best,
  provisional, and append-only stable output from the existing acoustic timing
  lattice without callsign, language, or conversation input.

- Right-clicking an unmarked spectrum/waterfall frequency now opens a neutral manual
  decoder slice at that exact audio center. The probe can accumulate measured
  weak-signal evidence below automatic acquisition, keeps nearby manually
  selected lanes distinct, and promotes into the normal colored stream only
  after the unchanged acoustic verification gates pass; otherwise it expires
  after 30 seconds and exposes no text or callsign. Left-click remains
  dedicated to opening an already detected stream.

- The live timing decoder now feeds immutable mark/gap runs into its bounded
  event lattice. At completed-gap checkpoints it publishes up to four acoustic
  timing alternatives and a separate append-only correction containing only
  characters and spaces on which competitive paths agree. Decoder cards keep
  the continuously updating literal transcript; consensus remains separate
  callsign/debug evidence so a supported complete callsign can be identified
  without silently rewriting or visually displacing received text.

- Added dependency-free decoder foundations for the next accuracy pass: a
  bounded acoustic event lattice retains timestamped mark/gap evidence and
  produces N-best Morse segmentations without callsign or language influence;
  a separate callsign-evidence ranker preserves the raw span while scoring only
  acoustically compatible suggestions with capped provider evidence; and
  validated conversation profiles distinguish neutral monitoring, open-ended
  ordinary QSOs, and rule-specific contest exchanges. The lattice now feeds a
  separately labeled acoustic correction while the literal transcript remains
  intact; none of these APIs provides a path from decoder events to
  transmission.

- Open decoder cards now follow newly appended text automatically unless the
  operator is selecting text. Confirmed remote callsigns are bold and
  color-emphasized; an exact match for the station profile's own callsign is
  highlighted, displays **YOUR CALL HEARD**, and flashes the card five times.
  This is a receive-only visual notification and cannot initiate transmission.

- The spectrum panel now offers two profile-persisted live views: **Audio
  spectrum** keeps the smoothed FFT waterfall, while **CW symbols** renders
  only verified channels' keyed/unkeyed acoustic envelopes on a neutral
  background with nearest-neighbor sampling, so dit, dah, and gap edges remain
  visible without mixing full-passband receiver texture into the raster. Both views use
  the same live/replay stream and can be switched without resetting decoder
  state; the symbols view is raw keying evidence, not reconstructed text.
- Added the native `cwa_capture_replay` audit executable. It replays one or
  more operator-provided `audio.wav` captures through the production spectrum,
  tracking, and decoding path and reports every publication plus bounded-bank
  summary metrics, including the assigned color lease index; private captures
  remain local and are not CI fixtures.
- Added an independent bounded cadence estimator that fits recent key-down
  durations to 1/3 units and key-up durations to 1/3/7 units. Production
  capture audits now report its acoustic WPM and fit confidence alongside the
  selected decoder WPM, without using decoded words as timing evidence.
- `CwChannelBank::shiftTrackedFrequencies()`: retuning the linked radio's RX
  VFO while live audio is running now re-centers every currently tracked
  signal by the exact audio-domain shift the retune implies (accounting for
  CW-U/CW-L sideband direction), resynchronizing each track's narrowband
  mixer/filter at its new position without discarding decoded text or
  verification state — a deliberate retune no longer loses an
  already-identified signal's identity the way an unexplained jump would.
  Covered by a dedicated core test asserting the shift preserves state and
  text, then continues decoding correctly at the new frequency.
- The VFO readout now shows decimal/centesimal precision matching a real
  rig's display (e.g. `7016.45 kHz` on 40 m) via a dedicated formatter,
  instead of the coarser rounding used for spectrum axis labels.
- Debug capture (`OBS-003`) diagnostics snapshots now include a `radio`
  object (availability, RX/TX frequency, split state) on every line, so a
  capture can show whether/when the operator's VFO moved during the
  recording — a common, easily overlooked explanation for a signal that
  stops decoding partway through a capture. Covered by an extension to the
  existing `cwa_live_audio_pipeline_test`.

- Application update checking (initial `PKG-004` slice): a background check
  runs a few seconds after startup (disableable in Settings), and Settings →
  About gains a manual **Check for updates** button, comparing this build's
  version against the `version` field of the published continuous-release
  manifest. When an update is available, **Download update** fetches this
  platform's artifact, verifies its SHA-256 against the published
  `SHA256SUMS` before saving it, and discards anything that fails
  verification. Nothing is installed automatically: **Open installer** hands
  the verified download to the OS's own installer/package handler, and
  **Show in folder** reveals it as a fallback. New `Qt6::Network`-based
  `UpdateChecker` class; no silent self-install yet (tracked as the
  remaining scope of `PKG-004`).

### Changed

- Debug-capture track records now include the callsign actually presented to
  the operator and whether it came from phase consensus, the literal decoder,
  or retained same-stream identity, making label provenance auditable.
- Established operator-selected, Morse-likely, and verified streams now reserve
  their nearest raw spectral ridge before global peak-separation ranking. A
  stable presentation-center tie-break lets a returning true carrier reclaim
  its existing identity instead of being suppressed by a nearby skirt and
  published as an adjacent duplicate.
- Learned-likelihood experiments now preserve physically scaled input features,
  use a causal 30 ms contrast integrator, and score transition excess and
  implausibly short runs in addition to aggregate frame accuracy. This reduced
  field-replay probability fragmentation substantially, but the candidate
  remains experimental because exact character accuracy has not met the
  shipment gate.
- Decoder-card transcript content now fills the viewport when short and grows
  for scrolling when long. A complete callsign in append-only acoustic
  consensus remains eligible for the existing context/repetition label policy
  even when the separate legacy timing score later declines, and explicit
  segment flush preserves the final word boundary needed to complete a call.

- Stream labels and RF mapping now use the stabilized presentation frequency;
  the adaptive DSP center remains available for diagnostics but small carrier
  estimates no longer make the operator-facing frequency flicker.

- A track now stops feeding its timing decoder after 750 ms without a matched
  spectral candidate. The boundary forces key-up and flushes pending acoustic
  output once, then freezes verified or private decoder state until that
  carrier is genuinely matched again. Normal same-frequency word gaps remain
  continuous, while residual energy or a stronger nearby station can no longer
  append text indefinitely to an absent track.

- Retained decoder sessions now reconcile replacements with explicit source
  provenance: two simultaneously published nearby tracks cannot overwrite one
  observation or share a display color, while a genuine later reacquisition
  inherits the previous bounded transcript exactly once. Only the replacement
  decoder's raw text is considered for new callsign evidence; the composed
  presentation transcript is never rescored as repeated acoustic output.

- Live decoder transcripts now use a stable plain-text viewport instead of
  rebuilding rich text and moving the text cursor on every update. A card
  follows appended output only while already following the bottom; selecting
  text or scrolling upward leaves the operator's viewport undisturbed. The
  fixed wrapping width and reserved vertical scrollbar gutter also prevent
  line reflow as the transcript grows. Confirmed calls remain emphasized in
  the card header and own-call alerts remain unchanged.
  Its source-level regression check is line-ending independent on Windows,
  Linux, and macOS checkouts.

- The hosted Windows MSI contract check now accesses the legacy Windows
  Installer automation API through explicit reflected method and indexed-field
  calls, avoiding null results from late-bound COM dispatch under PowerShell 7
  while retaining readable normalized queries and strict table assertions.

- Windows MSI upgrades no longer invoke a nonstandard UI process detector
  bound to the incompatible x64 WiX custom-action binary, which caused setup to
  fail immediately after the welcome page. Process closure now remains in
  WiX's standard execute sequence, the finish-page launcher uses the canonical
  WiX binary, and CI rejects the unsafe detector or a wrong action binding.
  **Launch CW Buddy** remains unchecked on clean installs and is selected
  by default after interactive upgrades.

- Verified stream markers now correct a biased initial acquisition from robust
  recent carrier evidence, then follow only sustained coherent motion through
  deadband, dispersion/drift, slew-rate, and immutable-origin bounds. Identity,
  color retention, and decoder association remain anchored independently, so
  noise and adjacent signals cannot walk the marker. Debug captures now expose
  identity, DSP, and presentation frequencies plus match age, activity, color,
  and key state for direct field diagnosis.
- After an update artifact passes checksum verification, **Open Installer**
  and the platform-native reveal action replace **Download update** in the same
  row. The Windows MSI finish page now offers **Launch CW Buddy**, selecting
  it automatically only when an interactive upgrade found and closed a running
  instance; its process detection, bounded shutdown, condition, and installed
  launch target are verified from the generated MSI tables in CI, with verifier
  failures retained in the queryable job result for diagnosis.
- A verified marker without a confirmed callsign now labels only its frequency,
  avoiding repetitive vertical **CW stream** text. The confirmed callsign
  replaces the frequency as soon as sufficient decode/context evidence exists.

- The live spectrum controls now auto-collapse to their header when the
  pointer leaves the panel, reclaiming vertical space for the spectrum and
  waterfall. Hovering expands them immediately, and **Pin** keeps them open
  while making several adjustments. Opening the Audio spectrum/CW symbols
  selector also keeps the panel expanded until the selector closes.
- CW verification and WPM selection now use bounded recent character/cadence
  evidence instead of lifetime averages. All nine fixed 8–60 WPM hypotheses
  continue processing after the initial presentation choice, and the best
  complete path is selected again at a safe silence/flush boundary, so an
  early speed decision no longer permanently disables every alternative.
- Candidate verification now requires a sustained passing interval and
  verified tracks use a separate failure hold before demotion. Narrowband
  coherence is a bounded 0–1 concentration measure, and keying evidence uses
  a per-track adaptive floor/peak envelope derived from robust two-sided
  noise references. The bounded recent unknown-symbol allowance is 30%,
  calibrated so two uncertain characters in a short otherwise-valid segment
  do not erase a verified signal; the hard-negative corpus remains the guard.
  The independent blended character-confidence floor is recalibrated to 0.40:
  hosted live-pipeline evidence showed a valid 15-symbol/85-edge track with
  passing 0.522 cadence and 0.546 pure timing was otherwise held out by the old
  0.50 value. The separate timing gate and hard-negative corpus remain intact.
- Verified-stream exit hysteresis is now six seconds, and frequency prediction
  is capped across key-up gaps rather than extrapolating a noisy drift estimate
  indefinitely. Internal tracking can still follow bounded real drift, while
  the operator-facing marker stays at its identity anchor until a known VFO
  retune moves it. A 750 ms presentation hold bridges normal Morse word gaps.
- The VFO frequency readout is now a large, prominent display (RX in green,
  TX in yellow when split is active) with a distinct SPLIT badge, instead of
  a small single-line label — matching the visual weight of the decoder
  panel it sits beside. Added a placeholder "ON AIR" indicator next to it,
  styled and ready but intentionally not wired to live state: neither the
  CAT4OM protocol nor the OmniRig properties this app currently polls
  expose an actual transmit/PTT signal, and local keying/PTT hardware
  control isn't implemented yet (`KEY-001`, `SAFE-001`).
- The CW pitch guide's axis line now sits exactly on the boundary between
  the spectrum plot and the waterfall history (matching the same 0.36
  height split the renderer already uses), rather than at the very bottom
  of the waterfall — that boundary is where an operator actually reads
  frequency against traces.

### Fixed

- The spectrum signal-picker layer is disabled while no live/replay source is
  active, so it no longer shows a cross cursor or intercepts the central
  **Start live RX** / **Choose WAV recording** empty-state action.

- Decoder cards no longer place a bounded acoustic-consensus transcript below
  the live literal text, where a valid consensus abstention made auto-scroll
  appear to stop decoding. Close and explicit up/down reorder controls now act
  on press so frequent decoder-model refresh cannot cancel them, and the
  receiver toolbar is kept above the clipped spectrum pointer layer.

- Detected-stream left-clicks now pass through one stable plot-level pointer
  router instead of a delegate recreated by live decoder-model refreshes. This
  prevents cursor flicker and lost press/release pairs while preserving
  right-click manual signal picking and marker hover magnification.

- Decoder-card ordering uses explicit up/down controls, so the adjacent close
  button and reorder operations remain reliable while streams continue decoding
  and remain available to reopen from the spectrum marker.

- Reference serial defaults and ADIF/equipment fixtures now initialize every
  aggregate field explicitly, keeping the dependency-free core warning-clean
  under real GCC as well as Clang/MSVC.
- Clicking a verified stream now uses a direct topmost pointer target and
  reliably opens a larger, scrollable, selectable decoded-text window
  in the decoder pane. An operator-opened session follows the retained
  frequency/color identity if the tracker reacquires it under a new internal
  ID, instead of silently closing the card. Its bounded presentation
  transcript survives that replacement; later lifecycle hardening clears the
  confirmed callsign so a new acoustic source cannot inherit an old station.
- Callsign labels now reject noise-like alphanumeric tokens with separated
  digit runs after an ordinary letter prefix, while retaining international,
  portable, numeric-prefix, and contiguous multi-digit special-event calls. A
  structurally plausible token is promoted automatically
  only with decoded exchange evidence (`DE`, `CQ`, `TU`, or `UP`) or exact
  repetition, preventing a lone report-like fragment from becoming the stream
  name. Until then the marker shows only its frequency. Labels use
  an 18 px base size, enlarge to 32 px on hover, and confirmed callsigns also
  appear prominently in the open decoder-card header.
- Retained stream identity no longer treats residual energy at the remembered
  frequency as a live carrier. Only a currently matched spectral peak can fill
  the marker or enable keyed CW-symbol rows, so background noise cannot keep a
  departed stream visibly active or paint random symbols throughout its hold.
- Verified CW areas no longer vanish after the short verification-failure hold
  merely because the carrier is silent: inactive observations now remain for
  the configured decoded-signal timeout (still 30 seconds by default, now
  selectable up to 300 seconds). If a track does expire, its frequency retains
  the same palette color for at least five minutes, including across a known
  radio retune, so later passes do not appear to change identity.
- Verified-stream areas now use a stable 120 Hz presentation width instead of
  following the decoder's rapidly adaptive analysis filter, eliminating size
  flicker while the actual 60/120/240 Hz filter remains visible in diagnostics.
  Inactive retained streams leave the plot empty except for a short horizontal
  identity-color mark on the frequency axis. Stream labels use a larger base
  font and magnify further while their marker is hovered.
- Display timing, level, CW-guide, noise, averaging, and retention controls now
  use labeled sliders with live numeric readouts. The receiver workspace lays
  them out over multiple responsive rows instead of one overflowing strip of
  number boxes; the Settings page uses the same interaction.
- Fixed macOS development archives being rejected as damaged: their custom
  bundle metadata no longer expands the name, executable, identifier, and icon
  to empty values, and deployment now seals the bundle only after its canonical
  `VERSION` resource is installed. Hosted macOS jobs validate every required
  plist value and the complete strict code-signing resource envelope before
  upload.
- Fixed a persistent, unverified frequency track carrying an implausible
  timing/text hypothesis into a later real transmission. When recent
  single-element-dominated output remains rejected and the independent
  acoustic cadence fit confirms Morse timing, only that track's decoder state
  is reacquired; its carrier/noise tracking remains continuous and verified
  text is never rewritten.
- Fixed Audio spectrum showing the receiver passband as a bright textured
  block and CW symbols turning instantaneous broadband fluctuations into
  horizontal confetti. Waterfall suppression now uses a slow per-bin baseline
  plus 55–180 Hz local side references. CW symbols no longer thresholds the
  full FFT at all: it draws only active verified channels' keying envelopes as
  sharp three-bin marks against a neutral background. Unverified/passband noise
  remains available in Audio spectrum without obscuring Morse timing.
- Fixed transient `latest.json`, checksum, or package HTTP 404/server failures
  surfacing immediately during continuous-release replacement. Publication
  now leaves the previous manifest available while binaries/checksums are
  replaced and uploads the new manifest last; the client retries transient
  network and publication failures three times with bounded backoff.

- SSH-queryable hosted job markers now retain the tail of `ctest` output when
  the test stage fails, while preserving pipeline failure through `pipefail`;
  cross-platform failures can therefore be diagnosed without privileged API
  access instead of exposing only the word `failure`.
- Updated the live-audio integration fixture for sustained verification: it
  supplies five keyed `SOS` repetitions, asserts both averaged and
  instantaneous live spectrum bins, and retains a bounded 10-second internal
  failure deadline (15-second outer CTest limit). This corrects the obsolete
  two-repetition/5-second timeout without weakening decoder assertions.
- Live-pipeline test timeouts now print the final frame-valid flag, published
  channel model, and verification summary, making a fail-closed hosted result
  actionable when the expected verified channel is absent.
- Fixed the bounded 24-track bank silently discarding every later carrier once
  full. Strong new candidates can now replace the weakest unmatched
  unverified occupancy, evidence decays while unmatched, and decoded or
  Morse-likely candidates survive normal word gaps. Established tracks reject
  identity-breaking frequency innovations, preventing an old decoder/text
  history from walking onto a different nearby peak. Deterministic regressions
  cover both saturated admission and identity preservation.
- Fixed verification latching forever after a transient pass: every acoustic
  and timing gate is continuously re-evaluated, with hysteresis preventing
  ordinary short fades from making a valid marker flap.
- Fixed `CwChannelBank::shiftTrackedFrequencies()` leaving a nonsensical
  negative-frequency track behind when a VFO retune (or several small
  retunes accumulating, e.g. an operator tuning across the band rather than
  centering on one station) carried a tracked signal's audio-domain
  frequency past 0 Hz. Such a track is now dropped outright instead of
  lingering as an invalid candidate; a track shifted too far *positive*
  already correctly expires through the existing retention timeout once it
  stops matching spectral peaks, so needed no equivalent change. Found via
  a real debug capture spanning a live VFO sweep. Covered by a new core
  test.
- Fixed `timing_quality` and `mean_character_confidence` being mathematically
  forced identical: `CwTimingDecoder::finishCharacter()` fed the exact same
  per-character `confidence_` value into both accumulators, so the two
  separately configured verification-gate thresholds
  (`minimum_verification_timing_quality`, `minimum_character_confidence`)
  were really gating on one blended signal, not independent evidence —
  found via real contest debug-capture data: a track whose text visibly
  contained a legible `TEST` never verified because the combined metric sat
  at 0.338 for its entire life. `timing_quality` now accumulates a genuinely
  separate pure element-duration-ratio precision signal, excluding the
  amplitude/keying-probability component that stays part of
  `mean_character_confidence`. `CwMultiSpeedDecoder::score()` (WPM-hypothesis
  selection) was updated to keep scoring on `mean_character_confidence`,
  preserving its original, already-tuned behavior — an initial attempt to
  leave it pointed at `timing_quality` destabilized WPM lock on the existing
  deterministic test, caught by an instrumented before/after trace before
  shipping. Covered by a new core-test assertion that the two fields
  diverge. The remaining known issue — both are lifetime-cumulative
  averages since track creation rather than windowed, so early garbled
  history can still drag down a currently-clean track's confidence — is
  deferred to its own change (tracked in `BACKLOG.md` under `CW-001`).
- Fixed the root cause behind reports of CW visibly present in the spectrum
  never being identified: analysis of an operator-provided debug capture
  (using the new `OBS-003` capture tool) showed every falsely verified track
  decoding to text overwhelmingly made of just `E` and `T` — the two
  single-element Morse characters, which timing noise reproduces far more
  often than any other character since random on/off fluctuations rarely
  sustain the longer runs needed for anything else — while the existing
  unknown-symbol-fraction gate stayed well under its threshold throughout
  (1-11%), so it never caught this failure mode. Added a character-
  distribution plausibility gate (`CwVerificationReason::
  ImplausibleCharacterDistribution`, config fields
  `minimum_plausibility_check_characters` [default 40] and
  `maximum_simple_character_fraction` [default 0.35]) that holds a track out
  of, or retroactively removes it from, `Verified` once enough decoded text
  has accumulated to judge it — unlike every other gate, this one keeps
  re-checking even an already-verified track, since implausibility can only
  be judged from accumulated text, not a single instant's evidence. The
  0.35 threshold was calibrated directly against the real capture: the three
  false-positive tracks measured 0.59, 0.45, and 0.76, while the most
  plausible real candidate measured 0.27 and the benchmark's own legitimate
  decoded text ("SOSCQTEST123") measures 0.25. The check itself is exposed
  as a standalone, directly testable pure function
  (`isCharacterDistributionImplausible`) rather than inlined into the gate,
  and is covered by dedicated unit tests plus the full existing
  `cwa_verification_benchmark` hard-negative suite (still 0 false
  publications) and `ctest` suite (all 6 tests), confirmed with both the CI
  compiler matrix's designated-initializer-strict GCC and a direct local GCC
  build.
- The hosted live-audio integration fixture now sends actual keyed Morse and
  waits for a verified channel instead of treating a continuous carrier as a
  valid decoded station.
- macOS staged-version verification now reads deterministic bundle metadata
  generated directly from the canonical build value and validates the copy of
  `VERSION` stored inside each self-contained application bundle. Hosted checks
  print the expected and native metadata values when diagnosing a mismatch.

### Changed

- Swapped which of the two spectrum overlays reads as an "area": the CW
  pitch guide is now a pair of dashed vertical boundaries at the configured
  center ± half-width (no filled band), while an active verified CW track is
  identified primarily by a stable-width colored vertical area with a thinner
  keying-state line drawn on top. The two were easy to confuse when both were
  drawn as bands; only the identified-signal highlight is now an area.

### Added

- A VFO frequency readout in the decoder panel showing the connected radio's
  actual RX dial frequency, plus TX dial frequency when split is active.
  Hidden entirely unless a live radio/CAT source is actually driving the
  audio (radio enabled, linked to the current audio input, and a resolved
  frequency plan available) and the source is live audio rather than WAV
  replay — so it stays out of the way for receive-only SWL setups and file
  playback, which have no radio state to show. `AppSettings` gained
  `controlledTxRfHz()`/`controlledSplitActive()` alongside the existing
  `controlledRxRfHz()`, sharing one internal `resolvedControlledFrequencies()`
  helper; `ReplayController::setRadioFrequencyContext()` now threads TX
  frequency and split state through to new `radioFrequencyAvailable`/
  `radioRxFrequencyHz`/`radioTxFrequencyHz`/`radioSplitActive` properties.
  The decoder panel is also renamed from "Full-spectrum CW decoder" to
  "CW Decoder".
- An operator-started, bounded debug capture (initial `OBS-003` slice): a
  "Debug capture" button in the decoder panel records the raw live audio
  feeding the decoder to a WAV file plus a JSON-lines log of every track's
  full private diagnostic state (frequency, SNR, narrowband coherence,
  filter width, verification state/reason, spectral observations, key
  transitions, decoded/unknown symbols, timing/cadence quality, WPM,
  provisional and stable text) once per second, to a timestamped folder
  under the application's standard data location. Capped at 5 minutes;
  never starts implicitly; the button and a status line make an active
  capture clearly visible; the operator is told to review the resulting
  files before sharing them, since the audio is whatever the selected input
  picked up. Backed by a new dependency-free `WavWriter` (round-trip tested
  against the existing `WavReplaySource` reader) and
  `CwChannelBank::allTrackDiagnostics()`, which exposes full per-track state
  for every track, verified or not — distinct from the normal display model,
  which continues to expose only verified tracks. Verified end to end with
  an extended `cwa_live_audio_pipeline_test` that drives a real decode
  through the pipeline and confirms both output files are well-formed.
- A "Diagnostics" toggle in the decoder panel header showing the
  pre-verification candidate/Morse-likely counts and rejection-reason tally
  as an opt-in, once-per-second snapshot (off by default) rather than a
  continuously live-updating label, so it stays available for
  troubleshooting without the flickering the always-on version had.
- Pre-verification pipeline diagnostics (private candidate and Morse-likely
  track counts plus a tally of the specific gate each currently failing track
  is blocked on) are now computed and exposed through the desktop model layer
  from `CwChannelBank::verificationDiagnostics()`, which already tracked this
  internally. An initial always-visible decoder-panel readout of this data
  proved to be constantly flickering and not useful in practice, since
  `narrowband_coherence` is a naturally noisy per-instant metric even for a
  clean tracked tone; the readout was removed from the default view. The data
  remains available on the model for a future dedicated diagnostics view.
  Unverified candidates still receive no spectrum overlay, session row, or
  detected-signal count.
- Fixed a verification-state/reason inconsistency: a track that reached
  Morse-likely could later fail an earlier gate again (for example
  `narrowband_coherence` dropping back under threshold) and stay stuck
  reporting a Morse-likely state alongside a reason that gate no longer
  supports, because `verification_state` only ever advanced and never
  re-derived from current evidence. `CwChannelBank::updateVerification()` now
  re-derives state from current evidence on every call. A deterministic core
  test asserts state/reason consistency is maintained throughout a naturally
  flickering scenario. This was confirmed against two operator screenshots of
  the initial (flickering) diagnostics readout showing Morse-likely tracks
  reporting `low-narrowband-coherence`, which the old gate ordering could
  never produce correctly.
- A configurable decoded-signal timeout (default 30 seconds, replacing the
  previous fixed 8-second value): `CwChannelBank` gained a `configure()`
  method to apply a new configuration to an existing bank without discarding
  current tracks, exposed end to end as Settings → Display → "Decoded signal
  timeout" and applied to both the live-audio and WAV-replay decoder workers.
- A deterministic broad-spectral-hump hard-negative case in the verification
  benchmark. A raised-cosine spectral feature far wider than the near/far
  prominence reference windows (matching adjacent SSB audio, AGC pumping, or
  a receiver-filter skirt rather than a narrowband CW carrier) now has a
  permanent regression test confirming it is rejected before any track is
  created, closing a coverage gap the existing steady-carrier and
  speech-like-AM cases did not exercise (those reject on keying pattern
  rather than peak shape).
- An explicit candidate → Morse-likely → verified → lost lifecycle with
  inspectable rejection reasons and frozen verification-time confidence. The
  gate now combines repeated spectral persistence, keyed edges, narrowband
  coherence, spacing cadence, known/unknown symbols, mark timing, and mean
  character confidence. Persistence tolerates ordinary key-up gaps instead of
  requiring adjacent FFT frames, preserving short high-speed marks. Bounded
  per-character evidence is carried with stable decoder output, exposed to the
  desktop model, and included in conservative decoder-state resource reporting.
- A deterministic verification benchmark with enforceable targets: clean,
  30 WPM, and weak/fading/drifting CW must acquire within six simulated
  seconds; steady carriers, speech-like amplitude modulation, irregular
  impulses, and pumping broadband noise must publish no tracks; processing
  must remain below a 0.20 real-time factor.
- Backlog specification for an explicitly enabled, bounded and redactable full
  diagnostic capture bundle containing audio, spectra, decoder evidence,
  time/frequency references, overruns, and relevant station context.
- Verified-CW publication gate between internal spectral candidates and the
  operator UI. A candidate now needs local peak prominence, repeated spectral
  observations, at least three known Morse symbols, bounded unknown-symbol
  fraction, and adequate timing quality before it receives a colored trace or
  contributes to the detected-signal count. Unverified candidates expire after
  750 ms. A deterministic five-second shaped-noise test publishes zero traces.
- FFT-resolution-aware peak qualification now uses a permissive near-shape
  check plus Hz-scaled far references. This admits real narrowband traces wider
  than a few FFT bins while rejecting broad spectral pumping before decoding.
- Completed-word callsign confirmation: vertical callsign text is withheld
  until the track is verified, timing quality passes, the text is stable, and a
  word gap proves the token is complete. Frequency/callsign annotations now sit
  inside the upper spectrum region instead of across waterfall history.
- The configurable CW guide is now one semi-transparent red band overlay rather
  than two lines that could be mistaken for decoded signal traces.
- One canonical effective build version across the About pane, Qt application
  identity, `--version`, Windows executable metadata and MSI, macOS bundle,
  Debian package, installed `VERSION` file, CAT4OM handshake, and continuous
  release manifest. Hosted builds use `major.minor.workflow-run`; local builds
  retain revision `0`. The QML startup smoke test now verifies the rendered
  About value against the application identity.
- Operator-selected decoded sessions: every detected signal keeps decoding in
  the background, while clicking its colored spectrum/waterfall marker opens a
  session card. Cards can be closed without stopping DSP, reopened from the
  marker, and reordered with explicit up/down controls. Conservative decoded callsign candidates
  and frequency labels run vertically beside the matching colored trace.
  Session reordering uses an explicitly bounded cross-platform index type.
- Checked actual-RF labels for linked live radio audio. Profiles explicitly
  confirm that the selected input belongs to the configured radio and choose
  CW-U/USB or CW-L/LSB tone direction. Live OmniRig frequency polling on Windows
  and pushed CAT4OM frequency state are combined with the RX transverter offset,
  selected CW reference pitch, and decoded audio tone; WAV, SWL, unlinked, and
  unavailable-radio states remain labeled in audio hertz.
- Sub-bin carrier interpolation, bounded frequency/drift prediction, robust
  two-sided local noise tracking, centered-tone rejection, and automatically
  selected 60/120/240 Hz per-track filters. Filter selection holds the stable
  120 Hz acquisition path before adapting, and deterministic tests cover a
  40 Hz/s drifting tone, automatic widening, exact CW-U/CW-L RF mapping, and
  adjacent-signal rejection.
- Bounded multi-speed timing acquisition per frequency. Nine deterministic
  hypotheses spanning 8–60 WPM compete on accumulated timing quality and a
  conservative speed prior; the leader remains explicitly provisional during
  the 2.5-second evidence window, then one adaptive path is locked. A 2.5-second
  transmission gap permits safe speed reacquisition while preserving prior
  stable text. The benchmark now gates six speeds, weak/jittered inputs, WPM
  error, a 12→40 WPM transition, nine-hypothesis state size, CER, false output,
  and real-time factor (0/49 edits and zero speed failures in the deterministic
  baseline).
- Per-frequency raw-audio CW evidence in both live-input and WAV DSP workers.
  Each tracked channel now uses a phase-continuous complex mixer, a three-stage
  120 Hz narrowband filter, 500 Hz evidence updates, and adjacent-band noise
  references before timing decode. Spectrum averaging and display gain remain
  available for visualization/candidate discovery but can no longer directly
  assert key-down. Deterministic tests cover simultaneous independent tones,
  adjacent-tone rejection, the threaded live-audio decoder path, and suppression
  of numerical FFT-floor peaks outside the initial 96 dB acquisition range.
- Full-processed-passband CW detection with a bounded 24-track channel bank,
  automatic peak association, independent decoder/timing state per frequency,
  stable track IDs, bounded silent-track expiry, and distinct shared colors for
  spectrum/waterfall markers and frequency-sliced decode rows. The configurable
  700 Hz boundaries are now explicitly visual-only and absent from decoding;
  the redundant aggregate listening/key-down label was removed from the live
  control bar.
- Soft SNR-to-key probability, smoothed hysteretic transitions, likelihood-
  weighted timing adaptation, common punctuation/prosigns, and separate amber
  provisional versus append-only stable decoder text.
- A deterministic decoder accuracy/resource benchmark reporting CER, no-CW
  false output, processed duration, throughput/real-time factor, and a
  conservative bounded-state estimate with a 256 KiB corpus ceiling across
  slow, weak, jittered, and faster replay cases.
- Backlog/requirements for left-click local CW-slice selection, distinct
  right-click RX CAT centering, actual-RF anchored decoded tracks, in-place
  callsign refinement, loss/reacquisition, and synchronized overlay/list expiry.

- Receive-only SWL station profiles that skip CAT and key/PTT wizard pages,
  positively identified online-radio selection through Windows OmniRig status,
  safe refresh without speculative serial probing, and an explicit manual
  template path for radios that cannot be detected.
- Native Windows, macOS, and Linux audio-input enumeration with system-default
  following, hot-plug refresh, unavailable-device indication, and per-profile
  selection in an always-present wizard step and Settings → Audio page.
- Live sound-card RX with operator-controlled start/stop, microphone-permission
  handling, format conversion/downmix, a bounded allocation-free capture queue,
  overrun telemetry, and a separate DSP worker feeding the real spectrum and
  waterfall. WAV input remains available as an explicit replay mode.
- Per-profile audio conditioning with default DC rejection, optional bounded
  automatic gain and tunable dBFS target, exact manual gain, and automatic or
  user-entered processing bandwidth. Automatic display scaling is now clearly
  identified as visualization-only rather than audio gain.
- A two-tab live spectrum control panel directly below the visualization for
  immediate signal/display tuning and explicit profile saving, plus a clear
  decoder-unavailable state until the decoding milestone is implemented.
- Stable automatic display span, smoothed noise-floor/ceiling tracking,
  adjustable waterfall-only noise suppression, and live noise-floor telemetry.
- A profile-persisted constant waterfall time window that remains stable while
  history fills or the pane is resized, preserves source-time gaps as dark
  rows, and decouples represented seconds from selectable line density.
- Overlapping FFT hops tied to the selected waterfall line rate for real
  high-resolution dit/dah timing updates without sacrificing the 2,048-sample
  frequency window.
- New-profile waterfall defaults of 60 timing lines/s over a constant 10-second
  view, with immediate history control for magnifying short elements.
- A toggleable red CW passband guide with configurable 700 Hz center/width and
  a seven-point frequency scale across the spectrum/waterfall X axis.
- A normalized per-profile own station callsign field under Settings → Station,
  ready for station logging and exact own-call notification matching.
- A modern CW Morse-key application mark with native Windows executable/MSI,
  macOS bundle, Linux desktop, and Qt window icon assets. The Windows installer
  creates the **CW Buddy** Start-menu program group and desktop shortcut.
- Backlog scope for configurable own-callsign decode notification and an
  optional guarded QSO-closing macro.
- Backlog scope for optional real-time callsign prediction/validation using a
  versioned, provenance-visible callsign list while preserving raw decoder text.
- A researched high-accuracy decoder strategy combining a low-resource
  explainable timing baseline, an optional compact causal likelihood model,
  calibrated confidence, and bounded multiple-pass weak-signal refinement.
- Decoder backlog and acceptance gates for co-channel operator fingerprinting,
  joint timing separation, conservative interference cancellation, optional
  coherent receive diversity, data provenance, false-output measurement, and
  CPU/memory budgets.

- GPL-3.0-or-later license text and dependency/contribution licensing policy.
- Editable Yaesu FT-450D and FT-818/FT-818ND reference profiles with tested CAT
  framing, Hamlib IDs, compatible OmniRig command descriptions, and direct-COM
  RTS/PTT plus DTR/KEY starting values.
- Optional Qt Quick desktop target with a modern expandable receiver workspace,
  persistent Settings pane, passive serial-port enumeration, and configurable
  CAT framing, polling, timeout, keying polarity, and display rates/range.
- Windows native OmniRig configuration-dialog invocation from the Settings pane.
- Named, isolated station configuration profiles, a startup profile selector,
  create-profile helper, per-profile first-run setup wizard, and `--profile`
  override for parallel instances and unattended launches.

- Testable compatibility scope for band maps, callsign/watch/validation policy,
  operational DSP controls, I/Q calibration and recording, pointer/keyboard
  tuning, spot/spectrum exports, band plans, auto-start, and health indicators.
- Split-capable CAT domain contract and checked integer-Hz frequency resolution
  with independent signed RX/TX transverter offsets, persisted in station
  profiles and editable from Settings and guided setup.
- ADIF 3.1.7 satellite/split logging fields with exact actual-RF `FREQ`,
  `FREQ_RX`, `BAND`, `BAND_RX`, `PROP_MODE`, `SAT_NAME`, and `SAT_MODE`, plus
  current band-enumeration mapping and a conformance-readiness policy.
- Native Qt 6.11.2 desktop build automation for Windows 11 x64, Ubuntu x64,
  macOS ARM64, and macOS x64 alongside the dependency-free core matrix.
- Ordered ADIF-band station-equipment rules that select radio, transverter, and
  antenna from actual RF frequencies and emit `MY_RIG`/`MY_ANTENNA`, including
  explicit TX/RX descriptions for cross-band operation.
- Project-record verification now runs on the older Bash bundled with macOS as
  well as on Linux CI.
- GitHub-hosted desktop builds now stage Qt runtime/QML dependencies and publish
  downloadable Windows 11 x64, Linux x64, macOS ARM64, and macOS x64 artifacts.
- Native CAT4OM 1.x Control-channel client foundation with read-only monitoring,
  password-proof handshake, pushed multi-VFO/split state, sequence-gap recovery,
  bounded reconnect, explicit ownership requests, and capability-gated frequency
  controls. CAT4OM PTT/CW commands are deliberately excluded.
- Debian/Ubuntu `.deb` generation in hosted CI, including desktop integration,
  license, deployed Qt/QML runtime files, and operator manuals.
- User-facing manuals for first launch, profiles, configuration examples,
  online artifacts, Debian/Ubuntu installation, and CAT4OM operation.
- Persistent repository and CI rules requiring user-manual, changelog, and
  backlog updates to accompany implementation and delivery changes.
- Portable GitHub matrix expressions for conditional Debian package jobs.
- Root binary-download index and machine-readable manifest with exact stable
  platform URLs, plus automatic continuous-prerelease publication and SHA-256
  checksums after the complete hosted matrix succeeds.
- Application About page and Linux metadata identifying Alessio Bravi
  (IU0LFQ / AD2FC) as author and linking to the author website.
- Dependency-free deterministic WAV replay for PCM 8/16/24/32-bit and IEEE
  float32 input, with bounded blocks, multichannel mono downmix, sample-derived
  timestamps, restart behavior, and malformed-format diagnostics.
- Hann-windowed radix-2 audio/IQ spectrum analyzer with coherent-gain dBFS
  normalization, configurable exponential power averaging, exact frequency
  coordinates, and deterministic tone/replay tests.
- Receiver-workspace WAV selection and paced replay through a functional 2D Qt
  scene-graph spectrum/waterfall, with profile-persisted FPS, waterfall rate,
  automatic/manual dBFS range, DSP averaging, grid, progress, and gap metrics.
- Continuous verification marker: the `continuous` tag advances only after the
  full cross-platform desktop/core matrix and release publication succeed.
- Temporary per-platform annotated CI status tags expose exact failed-step
  outcomes and bounded Qt-installer/build diagnostics to authorized Git-only
  automation and are removed after a green run.
- Windows 11 x64 now publishes an upgrade-capable WiX/MSI installer with stable
  upgrade identity, per-run package revision, Start-menu/desktop shortcuts, and
  normal repair/uninstall registration instead of a `.tar.gz` binary archive.
- macOS Sonoma 14 or newer is now the explicit compiled baseline for both Apple
  silicon and Intel x64 artifacts, with hosted Mach-O deployment-target checks.

- Cross-platform C++20/CMake project foundation.
- Dependency-free sample block and bounded SPSC ring-buffer primitives.
- Channel scheduling by signal strength, arrival queue, or operator selection.
- Hardware-neutral interfaces for audio/SDR sources, Hamlib CAT, serial keying,
  and logger adapters.
- Human-confirmed transmission safety state machine.
- ADIF record serialization for the planned Log4OM 2 UDP integration.
- Architecture, requirements, test-data, and delivery-roadmap documentation.
- Native GitHub Actions builds for Windows x64, Linux x64, macOS ARM64, and
  macOS x64.
- Pull-request policy check requiring the changelog and backlog to accompany
  source, test, build, or workflow changes.
- Qt Quick scene-graph rendering decision, including shader palette, waterfall
  ring, clickable overlays, fallbacks, and modularity rules.
- Functional 2D visualization scope, explicitly excluding 3D spectrum and
  ornamental rendering effects.
- Configurable visualization model with independent FPS and waterfall speed,
  bounded manual dB range, averaging, and operational overlays.
- Normalized exact-match callsign ignore policy, enforced again by the TX guard,
  with unit coverage.
- Network receiver directory contracts and a receive-only KiwiSDR-first
  integration policy with safe browser handoff for unsupported protocols.
- Secure standalone/station-server/remote-client architecture with station-local
  CW timing, bandwidth profiles, reconnect snapshots, and authenticated control.
- Dependency-free exclusive per-rig control lease manager with bounded TTL and
  expiry/ownership tests.
- Windows 11 x64 or newer established as the supported Windows baseline.

### Fixed

- Prevented flat receiver noise and radio-AGC level movement from pumping the
  waterfall through bright yellow by anchoring automatic levels to a stable
  minimum span and slowing downward ceiling/floor movement.
- Removed persistent sound-card DC bias from the leftmost spectrum bin by
  default and stopped full-Nyquist audio from compressing CW activity against
  the left edge through a configurable 100–3000 Hz automatic view.
- Fixed blank live-audio spectrum/waterfall output accompanied by rising input
  overruns. The DSP drain timer now follows its worker onto the processing
  thread, with a cross-thread regression test that requires a real FFT frame.
- Kept the standalone spectrum-render startup regression target linked to the
  complete receiver source implementation and made live-audio sample-format
  handling warning-clean across current Qt platform SDKs.
- Widened the Settings drawer and added consistent horizontal content margins,
  preventing Station and device controls from sitting against the window edge.
- Kept the setup wizard's Back/Next/Finish controls in a fixed footer and made
  oversized setup pages scroll, preventing navigation from being clipped on
  small or scaled displays; the complete hosted QML compiler now validates the
  restructured dialog before packaging, including portable Qt/C++ declarations
  used by its runtime navigation smoke test.
- Fixed the cross-platform first-launch crash in the spectrum renderer. The
  waterfall now uses a backend-native image node created only after a valid
  texture exists; it never passes a null texture into the Qt scene graph.
- Added direct empty-render and full-QML startup regression tests to every
  hosted desktop build, plus a native-graphics launch test of each staged
  package with deterministic spectrum injection, covering both the empty launch
  state and waterfall texture creation.
- Windows Qt 6.11 SDK installation uses an immutable upstream downloader commit
  containing the new repository-layout correction until that correction ships
  in a released downloader. This build-only pin does not change the bundled Qt
  runtime, MSI version, or stable Windows upgrade identity.
- Windows MSI generation now supplies the canonical GPL text through CPack's
  supported UTF-8 `.txt` license input and publishes bounded WiX diagnostics on
  packaging failure.
- Corrected invalid setup-wizard QML that stopped hosted desktop compilation.
- macOS packaging now creates and deploys a self-contained `.app` bundle rather
  than archiving a non-bundle executable without its Qt/QML runtime.
- Hosted Qt installation now includes the task-tree dependency required by the
  Qt 6.11 QML plugin metadata, eliminating an incomplete-SDK warning.
- Corrected the Qt 6.11 Linux architecture identifier and added a clean,
  uncached Qt-install retry using the hosted runner's external 7-Zip binary for
  archive failures in the Python extractor.

### Security

- Transmission begins disarmed and cannot be initiated directly by decoder
  output.

[Unreleased]: https://github.com/alessiobravi/CW-Buddy/compare/HEAD
