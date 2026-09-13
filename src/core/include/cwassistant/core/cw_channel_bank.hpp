#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/cw_decoder.hpp"
#include "cwassistant/core/sample_block.hpp"

namespace cwassistant::core {

enum class CwTrackState : std::uint8_t {
  Candidate,
  MorseLikely,
  Verified,
  Lost,
};

enum class CwVerificationReason : std::uint8_t {
  NeedsSpectralPersistence,
  NeedsKeyingEdges,
  NeedsCadenceEvidence,
  LowNarrowbandCoherence,
  LowCadenceQuality,
  NeedsDecodedSymbols,
  TooManyUnknownSymbols,
  LowTimingQuality,
  LowCharacterConfidence,
  NeedsSustainedEvidence,
  ImplausibleCharacterDistribution,
  Verified,
  SignalLost,
  // A real keyed carrier that holds more than one keyer, and that the ordinary
  // per-signal filter cannot separate. The track keeps its detection,
  // frequency, marker and key-state occupancy; only its open-ended text is
  // withheld, because the sum of two keyers is not Morse and a transcript of
  // it misleads the operator about what is on the frequency.
  //
  // This is a statement about the effort spent so far, not a permanent
  // verdict. Carriers this close cannot be separated by narrowing a filter --
  // measured, a 30 Hz filter makes the timing statistics plausible again and
  // leaves the text noise -- but they can be separated by solving neighbouring
  // carriers jointly, which is expensive and belongs in the signal path rather
  // than here. A track refused here therefore also carries, in its diagnostic,
  // how crowded its neighbourhood is and by how much its speed estimate
  // diverged, so a later stage can pick the ones worth that work.
  //
  // Appended after SignalLost rather than filed next to the other post-Morse
  // gates on purpose: the replay model reconstructs a reason from its stored
  // ordinal, so no existing value's index may move.
  UnresolvedKeyerOverlap,
};

inline constexpr std::size_t kCwVerificationReasonCount =
    static_cast<std::size_t>(CwVerificationReason::UnresolvedKeyerOverlap) + 1U;

[[nodiscard]] const char* cwTrackStateName(CwTrackState state) noexcept;
// Whether decoded text contains a token distinctive enough that its presence is
// itself strong evidence the channel is carrying real Morse, independent of any
// timing or confidence measure.
//
// Motivated by a real capture: a contest station whose decoded text plainly
// read TEST never verified, because its timing quality sat under the threshold
// for the track's entire life. Nothing else about the track was in doubt -- it
// had a carrier, keyed edges, cadence and coherence -- so a legible contest
// token was better evidence than the proxy that rejected it.
//
// The list is deliberately short and skewed to tokens noise is unlikely to
// spell by chance. Short, common ones a random keying pattern lands on
// regularly -- K, DE, R, single letters -- are excluded however useful they
// are to a human reader, because the point is evidence, not readability.
[[nodiscard]] bool cwTextContainsDistinctiveToken(
    std::string_view text) noexcept;

[[nodiscard]] const char* cwVerificationReasonName(
    CwVerificationReason reason) noexcept;

// A long run of decoded text dominated by only the two single-element
// characters (E, T) is the statistical signature of timing noise being
// classified as Morse rather than genuine text: random on/off fluctuations
// rarely sustain the longer runs needed for other characters, while real
// ham/English text sits close to natural letter frequency (E+T is typically
// ~20%). False below `minimum_characters` since the fraction is not yet
// statistically meaningful. Exposed standalone (rather than inlined into the
// verification gate) so its threshold behavior can be tested directly
// against literal decoded-text examples.
[[nodiscard]] bool isCharacterDistributionImplausible(
    const std::string& text, std::uint16_t minimum_characters,
    float maximum_simple_character_fraction) noexcept;

// Keying speed, in words per minute, implied by a window of observed key-run
// lengths measured in evidence frames.
//
// The median run is read as one element. In ordinary Morse the one-unit runs
// -- the dits and the gaps between elements inside a character -- outnumber
// every longer run, so the median lands on an element rather than between the
// element lengths, and a minority of runs that a noise excursion split or
// merged cannot move it. A mean can, which is the whole reason for taking the
// median: this measure exists to be compared against itself at another filter
// width, so it has to answer the same way about the same keying twice.
//
// Returns zero for an empty window or a non-positive frame rate, which callers
// read as "no estimate" rather than as a slow signal.
[[nodiscard]] double cwKeyingSpeedFromRuns(
    std::span<const std::uint16_t> run_frames, double frame_rate_hz) noexcept;

// How many times a callsign must have been read before the name it gives a
// stream stops following the newest reading and starts resisting it.
//
// Two, because that is the smallest number that separates the two populations
// an operator actually sees. A station identifies repeatedly -- a captured
// minute of one real stream read EH3ST eight times -- while the misdecodes
// around it (EH3S, EH3SN, EHSMST, EG7S) each appeared once and never again.
// One reading cannot tell those apart; two already can.
inline constexpr std::uint32_t kCwStreamLabelCorroborations = 2;

// Whole-token occurrences of `callsign` in `text`, where a token is bounded by
// anything that is not a letter, digit or slash. Substring matching would
// count the DK7SS inside a glued VDK7SSK, which is a different decode, not
// another reading of the same one.
[[nodiscard]] std::uint32_t cwCountCallsignOccurrences(
    std::string_view text, std::string_view callsign) noexcept;

// The evidence behind the name one stream currently wears.
//
// `support` is the strongest corroboration seen for `callsign` for as long as
// that name stands, so it never falls back while the name holds. That is what
// makes the name survive its own transmission scrolling out of the live text
// window: the evidence is remembered, not re-derived from the text on screen.
struct CwStreamLabel {
  std::string callsign;
  std::string challenger;
  std::uint32_t support{0};
  std::uint32_t challenger_support{0};
  // Set when a name that had not yet been corroborated was displaced by a
  // disagreeing reading no better supported than it was. Two single readings
  // that contradict each other are evidence that the copy is not good enough
  // to read a call out of; they are not evidence for whichever of them was
  // read last. While this stands the stream is published without a name. It
  // clears the moment any reading reaches the corroboration bar.
  bool contested{false};
};

// Folds one reading of a stream's station name into the name it already wears.
//
// The reading must already have been vetted -- complete, not the operator's
// own call, and opening on an allocated prefix -- because this function only
// weighs evidence and never judges a callsign's plausibility.
//
// Hysteresis, in three rules. An empty reading changes nothing at all: a
// station that has stopped sending, or a few seconds of copy too poor to read
// a call out of, is not evidence that a different station has arrived. Below
// `corroborations` the name is provisional and simply follows the newest
// reading, which is how a stream gets named the moment it first identifies.
// At or above it the name is established, and a disagreeing reading becomes a
// challenger that has to reach the same bar itself before it takes over; any
// reading that agrees with the established name discards the challenger
// outright.
//
// Support comes from the strongest of the two decoded paths rather than their
// sum, because the literal and refined texts are two readings of one
// transmission and counting both would let a single identification establish a
// name on its own.
[[nodiscard]] CwStreamLabel cwApplyStreamLabelReading(
    CwStreamLabel label, std::string_view reading,
    std::string_view primary_text, std::string_view refined_text,
    std::uint32_t corroborations = kCwStreamLabelCorroborations);

// Hard ceiling on `CwChannelBankConfig::maximum_tracks`, and the size of every
// fixed array the bank indexes by track slot or by color index.
//
// It is not the default: the default is a processor budget (see
// `maximum_tracks`), while this is the point past which the fixed storage
// itself stops being reasonable. Two independent limits fix it. A track's
// `color_index` is a `std::uint8_t`, so no more than 256 distinct colors can
// ever be addressed; and the arrays sized by this constant are members of the
// bank, so a value in the thousands would put hundreds of kilobytes on a
// structure that an audio thread touches. 128 leaves an operator with a large
// machine roughly twice the measured default to spend, which is more headroom
// than any capture in the corpus has asked for, and costs about three
// kilobytes of leases and one of monitor slots.
inline constexpr std::size_t kCwMaximumTrackCapacity = 128;

struct CwChannelBankConfig {
  // The operator's own callsign, normalized, or empty when unset. A stream is
  // labelled with the station transmitting on it, and the operator's own call
  // is by definition not that station: hearing it means somebody is calling
  // the operator, and the station to name is the one doing the calling. It
  // reaches the decoded text often -- a caller sends it before its own -- and
  // without this a pileup answering the operator would label every stream with
  // the operator's own call.
  std::string own_callsign{};
  float acquisition_snr_db{7.0F};
  // A track weaker than this is acquired, followed and drawn, but not decoded.
  // Below it what reaches the decoder is fragments rather than copy: on the
  // capture corpus such tracks emit streams of one- and two-element characters
  // that fill a transcript with nothing and spend a decoder's worth of
  // processor time each. The default was first set at twelve decibels from the
  // capture corpus, where the weakest track carrying a correctly recovered
  // callsign sits at 19.5 dB. On the air that proved far too aggressive and
  // suppressed signals an operator could work, so the repository owner set it
  // to four. The corpus was never the whole population: it is twenty-two
  // recordings made on one receiver, and a threshold fitted to it does not
  // transfer to a different front end or a quieter band.
  float minimum_decode_snr_db{4.0F};
  // Decode every tracked signal regardless of level. Off by default because
  // the cost is paid in both transcript quality and processor time.
  bool decode_weak_signals{false};
  float retention_snr_db{2.5F};
  float detection_dynamic_range_db{96.0F};
  float minimum_peak_prominence_db{4.5F};
  float minimum_near_peak_prominence_db{0.8F};
  double prominence_reference_offset_hz{160.0};
  double prominence_reference_width_hz{100.0};
  double minimum_separation_hz{45.0};
  double tracking_tolerance_hz{70.0};
  double empty_track_retention_seconds{2.0};
  double decoded_track_retention_seconds{30.0};
  double unverified_track_retention_seconds{0.75};
  // A stream that decoded well enough to name its station keeps its place in
  // the display for this multiple of the decoded retention above. Every other
  // stream keeps the plain value, so a poor or anonymous decode fades on the
  // standard clock.
  //
  // This multiplies the display's retained-observation lifetime and nothing
  // else. It is deliberately not part of track expiry: a Track owns a decoder,
  // a slot in the bounded bank and a frequency cell that a genuinely new
  // station has to be able to take over, and a real new signal on a frequency
  // is better evidence than the memory of an old one. An earlier attempt that
  // held an identified track alive twice as long stopped exactly that takeover
  // and silently broke identity inheritance. What outlives the signal is the
  // remembered region on the waterfall, which is a display concern and already
  // has its own lifetime.
  double identified_stream_retention_multiplier{2.0};
  // How long a track carried out of the processed passband by a receiver
  // retune is held before it is given up.
  //
  // A signal that leaves the passband because the operator turned the dial has
  // not been lost: its position is known exactly, and turning the dial back
  // brings it to a computable place. Expiring it on the ordinary retention
  // timeout destroyed the identity, the transcript and the audio monitor of a
  // station the operator was deliberately tuning around, and it came back as a
  // new, unrecognised track. Generous on purpose -- tuning away and back is
  // measured in tens of seconds -- and bounded so a band-edge sweep cannot
  // accumulate parked tracks without limit.
  double parked_track_retention_seconds{180.0};
  // A verified frequency keeps its display color after the live track expires
  // so later passes from the same carrier do not look like different stations.
  double color_identity_retention_seconds{300.0};
  double color_identity_tolerance_hz{35.0};
  double narrowband_width_hz{120.0};
  double noise_reference_offset_hz{300.0};
  double evidence_rate_hz{500.0};
  // How many carriers the bank follows at once, clamped to
  // `kCwMaximumTrackCapacity`.
  //
  // This was 24 for no measured reason: the display palette happened to hold
  // 24 hand-written colors, the color-lease array was sized to match it, and
  // the track cap was sized to match that. So the number of stations the
  // decoder could follow was set by the length of a table of hex strings.
  //
  // What it has to cover is a pileup. An operator capture of a real one holds
  // about 50 stations inside a 6 kHz window, at a median spacing of 70 Hz and
  // a minimum of 52 Hz -- both above `minimum_separation_hz`, so every one of
  // them is a carrier this bank can resolve and would have to drop. The cap
  // also silently falsified its own diagnostics while it was in force: every
  // live record read `tracks: 24` exactly, which looks like a busy band and
  // was the bank saturated against the cap.
  //
  // 64 is where that demand meets a processor budget. Measured on this machine
  // with an optimized core, driving the bank with the capture's own geometry
  // -- a 6 kHz analyzed window, carriers 58 Hz apart, 10 ms blocks -- over
  // repeated twenty-second runs, the whole bank costs 2.0 to 2.1% of one core
  // at 24 tracks, 3.6 to 3.7% at 48, 4.9 to 5.1% at 64 and 7.2 to 7.5% at 96.
  // Per track that is 0.086%, 0.076%, 0.078% and 0.078%: flat above 24, so the
  // cost is simply linear in the cap and the cap is simply a budget. The worst
  // single block in any run was 1.7 ms at 24 tracks, 1.8 ms at 48, 2.0 ms at 64
  // and 3.4 ms at 96, against a 10 ms block period.
  //
  // The budget held here is a twentieth of one core, and no observed block over
  // a fifth of its own period -- so the audio thread, the display and the
  // optional character model still have a machine to run on, and no block
  // approaches its deadline. 64 tracks sits at both limits, covers the
  // 50-station pileup with room above it, and costs two and a half times what
  // 24 did on a load 24 could not carry at all. 96 was measured and rejected:
  // it breaks both limits by half, and buys capacity no capture in the corpus
  // has ever asked for.
  //
  // The same measurement unoptimized, for comparison with the figure this
  // replaces: 20% of one core at 24 tracks and 51% at 64. A debug build costs
  // about ten times a release one here, which is worth knowing before reading
  // a profile taken from one.
  //
  // Deliberately configurable in both directions rather than fixed, because
  // the measurement is one machine's. An operator on a small one lowers it and
  // pays in stations; one on a large one raises it to
  // `kCwMaximumTrackCapacity`.
  std::size_t maximum_tracks{64};
  // Keep the fast per-frame level tracker anchored to two robust modes from a
  // short history. Exposed to the core benchmark so the production path can
  // be compared with its exact pre-anchor baseline on identical audio; normal
  // application configurations leave this enabled.
  bool robust_keying_level_history{true};
  // The detector averages the supplied spectrum itself, over a fixed time
  // constant, so that display-side averaging and the display frame rate can
  // never change candidate discovery. Callers should supply unaveraged bins.
  double detector_averaging_seconds{0.05};
  // Detection runs on its own fixed cadence. Spectrum frames arriving faster
  // than this are folded into the detector's average but do not run an extra
  // detection pass, so raising the display line rate cannot change decoding.
  // Zero processes every supplied frame.
  double detector_frame_interval_seconds{1.0 / 60.0};
  // Spectral persistence is expressed in 60 Hz-equivalent observations and is
  // accumulated from elapsed time, so a track needs the same wall-clock
  // evidence regardless of the configured display frame rate.
  std::uint16_t minimum_spectral_observations{3};
  std::uint16_t minimum_verification_symbols{3};
  std::uint16_t minimum_key_transitions{6};
  std::uint16_t minimum_cadence_observations{3};
  // Raised from 0.45 once element timing was measured without the systematic
  // mark/gap bias. Real CW now sits at 0.85-0.98 and irregularly keyed noise
  // at about 0.44, so the threshold moves into the gap between them instead
  // of sitting on top of the negative.
  float minimum_verification_timing_quality{0.55F};
  float minimum_verification_cadence_quality{0.42F};
  float minimum_character_confidence{0.40F};
  // A long run of decoded text dominated by only the two single-element
  // characters (E, T) is the statistical signature of timing noise being
  // classified as Morse rather than genuine text: random on/off
  // fluctuations rarely sustain the longer runs needed for other
  // characters, while real ham/English text sits close to natural letter
  // frequency (E+T is typically ~20%). This check only applies once enough
  // text has accumulated to be statistically meaningful, and — unlike every
  // other gate — is re-evaluated even for an already-verified track, since
  // it can only be judged from accumulated text, not a single instant.
  std::uint16_t minimum_plausibility_check_characters{40};
  float maximum_simple_character_fraction{0.35F};
  // Normalized spectral concentration: 0 is approximately wideband noise,
  // 1 is a tone concentrated in the narrowest analysis filter.
  float minimum_narrowband_coherence{0.18F};
  float maximum_verification_unknown_fraction{0.30F};
  double track_identity_tolerance_hz{35.0};
  // Presentation may correct a biased first acquisition and cautiously follow
  // qualified carrier motion, but can never leave this radius around the
  // immutable identity origin. It never participates in DSP association.
  double presentation_reanchor_limit_hz{65.0};
  double presentation_follow_deadband_hz{4.0};
  double presentation_follow_slew_hz_per_second{2.0};
  double presentation_follow_stable_seconds{1.0};
  double presentation_follow_maximum_drift_hz_per_second{5.0};
  double presentation_follow_maximum_mad_hz{6.0};
  float track_replacement_margin_db{3.0F};
  double verification_enter_seconds{0.50};
  double verification_exit_seconds{6.0};
  double decoder_recovery_seconds{3.0};
  // Judge whether each track holds one keyer or several, and withhold the
  // open-ended text of the ones that hold several. Exposed so the production
  // path can be measured against its exact pre-gate baseline on identical
  // audio; normal application configurations leave this enabled.
  bool resolve_overlapping_keyers{true};
  // Above this ratio of the keying-speed estimate at the widest analysis
  // filter to the estimate at the narrowest, the channel is judged to hold
  // more than one keyer.
  //
  // One keyer reads nearly the same speed at both widths, because widening the
  // filter admits more noise but no more keying. Several keyers closer
  // together than any usable filter do not: the wider filter admits the
  // neighbours, their sum beats and their runs fragment, and the estimate
  // roughly doubles.
  //
  // The evidence is one operator capture of a real DX pileup -- about fifty
  // stations in six kilohertz, median spacing 70 Hz, minimum 52 Hz. Of the
  // twenty-one carriers measured in it exactly one read as a single keyer, and
  // it is the one that decodes cleanly: 20.6 WPM at 60 Hz against 24.4 WPM at
  // 240 Hz, a ratio of 1.19. The next-nearest carrier ratio was 1.83 and the
  // rest ran to 3.50. The default sits in that gap, nearer the population that
  // must keep decoding than the one that must be silenced.
  //
  // One clean carrier in one capture is thin evidence for a constant, which is
  // why this is configurable rather than compiled in. The clamp keeps it
  // inside the range where it can still mean something: below about 1.25 a
  // clean but noisy signal would be silenced, and above 4 nothing measured
  // here would ever fire.
  float maximum_single_keyer_speed_ratio{1.5F};
  // Below this acoustic cadence fit, a crowded channel is judged to hold more
  // than one keyer. The second, independent half of the question the ratio
  // above asks, because on real crowded air the ratio mostly cannot answer it.
  //
  // The fit is how well one keyer's cadence explains the channel's envelope,
  // and the decoder computes it for every track already. One keyer scores high
  // on it by construction; a dense pileup has no single cadence to fit and the
  // measure collapses. It is read only where the neighbourhood holds enough
  // other carriers for that reading to mean anything -- see
  // `kKeyerCadenceMinimumNeighbours` -- because alone on a frequency the same
  // collapse means poor copy.
  //
  // The two measures answer different failures and neither covers the other,
  // which is why both are kept. On the operator's capture of a real DX pileup
  // -- about fifty stations in six kilohertz, median spacing 70 Hz, minimum
  // 52 Hz -- the ratio produced no estimate at all for 41 of the 48 carriers
  // the bank published and refused exactly one, which was outside the dense
  // block; the cadence fit refuses 42 of 48 while keeping the DX that decodes
  // cleanly, the second station calling CQ, and the two carriers outside the
  // block. On the two-carrier synthetic fixture it is the other way round: the
  // ratio reads over 1.5 and refuses, while the fit falls only to 0.637 and
  // does not.
  //
  // Swept over the capture, with the decoder surface benchmark's per-seed-set
  // character error identical to the pre-change baseline at every value:
  //
  //   fit     capture carriers refused
  //   0.30          18 of 48
  //   0.40          27 of 46
  //   0.50          42 of 48
  //   0.60          44 of 49
  //   0.70          44 of 49
  //
  // 0.50 is where the curve flattens, and it is the conservative side of the
  // flat part: the weakest clean single carrier on the synthetic corpus
  // reaches 0.701, and the two further carriers 0.60 refuses are worth less
  // than that margin against silencing a station an operator could work. The
  // clamp keeps the setting inside the range where it can still mean
  // something at either end.
  float minimum_single_keyer_cadence_fit{0.50F};
};

struct CwVerificationDiagnostics {
  std::size_t candidate_tracks{0};
  std::size_t morse_likely_tracks{0};
  std::size_t verified_tracks{0};
  std::uint64_t verified_transitions{0};
  std::uint64_t expired_unverified_tracks{0};
  std::uint64_t decoder_reacquisitions{0};
  // Tracks currently verified whose character-quality gates were satisfied by a
  // recognised token rather than by the timing measures. Zero means the path
  // has never been needed; a non-zero count on air is the evidence that it is
  // worth keeping.
  std::size_t pattern_verified_tracks{0};
  // Verified tracks currently judged to hold more than one keyer, whose text
  // is therefore withheld while their occupancy is still published. In a
  // pileup this is most of the window; on a quiet band it should be zero.
  std::size_t unresolved_overlap_tracks{0};
  // Of those, the ones whose neighbourhood is sparse enough that a later joint
  // separation stage could be conditioned. This is the work queue that stage
  // would read; the remainder are genuinely too crowded to attempt.
  std::size_t joint_separation_candidates{0};
  std::uint32_t maximum_decoded_symbols{0};
  std::uint32_t maximum_key_transitions{0};
  float best_timing_quality{0.0F};
  float best_cadence_quality{0.0F};
  float best_narrowband_coherence{0.0F};
  std::array<std::size_t, kCwVerificationReasonCount> current_reason_counts{};
};

struct CwChannelSnapshot {
  std::uint64_t id{0};
  std::uint8_t color_index{0};
  double frequency_hz{0.0};
  // Stable operator-facing center. The adaptive tracker may move within its
  // bounded identity region, but presentation moves only on a known retune.
  double presentation_frequency_hz{0.0};
  double drift_hz_per_second{0.0};
  double filter_width_hz{120.0};
  float snr_db{0.0F};
  double wpm{0.0};
  double acoustic_wpm{0.0};
  float acoustic_cadence_confidence{0.0F};
  float confidence{0.0F};
  float key_down_probability{0.0F};
  bool key_down{false};
  bool active{false};
  bool verified_cw{false};
  // True while this channel is visible because the operator explicitly
  // selected its frequency. Selection creates a bounded analysis probe; it
  // never bypasses the ordinary acoustic verification gates.
  bool operator_selected{false};
  CwTrackState verification_state{CwTrackState::Candidate};
  CwVerificationReason verification_reason{
      CwVerificationReason::NeedsSpectralPersistence};
  float verification_confidence{0.0F};
  float verification_cadence_quality{0.0F};
  float verification_timing_quality{0.0F};
  float verification_character_confidence{0.0F};
  float cadence_quality{0.0F};
  float mean_character_confidence{0.0F};
  float narrowband_coherence{0.0F};
  std::uint32_t key_transitions{0};
  std::vector<CwCharacterEvidence> characters;
  std::string text;
  // Append-only text on which the competitive acoustic timing paths agree.
  // It remains separate from the literal decoder output above.
  std::string refined_text;
  std::vector<CwAcousticAlternative> acoustic_alternatives;
  std::string provisional_text;
  std::string pending_elements;
  std::vector<CwTransmissionTurn> transmissions;
  std::vector<CwSenderCadence> sender_cadences;
  std::uint64_t active_transmission_sequence{0};
  std::string current_sender_callsign;
  double current_sender_wpm{0.0};
  std::string contextual_text;
  std::string callsign;
  // High-confidence CALL1 DE CALL2 participants heard on one carrier. A
  // simplex QSO is one frequency observation containing alternating senders,
  // not two artificial frequency tracks.
  std::vector<std::string> qso_participants;
};

// Full private per-track state, including tracks never shown to the
// operator UI. Intended only for operator-consented diagnostic capture
// (OBS-003); never used to drive the normal display/session models.
struct CwTrackDiagnostic {
  std::uint64_t id{0};
  double frequency_hz{0.0};
  double identity_origin_frequency_hz{0.0};
  double presentation_frequency_hz{0.0};
  double drift_hz_per_second{0.0};
  float snr_db{0.0F};
  float narrowband_coherence{0.0F};
  double filter_width_hz{120.0};
  CwTrackState verification_state{CwTrackState::Candidate};
  CwVerificationReason verification_reason{
      CwVerificationReason::NeedsSpectralPersistence};
  std::uint16_t spectral_observations{0};
  std::uint32_t key_transitions{0};
  std::uint32_t decoded_symbols{0};
  std::uint32_t unknown_symbols{0};
  float timing_quality{0.0F};
  float cadence_quality{0.0F};
  float mean_character_confidence{0.0F};
  double wpm{0.0};
  double acoustic_wpm{0.0};
  float acoustic_cadence_confidence{0.0F};
  float keying_level_separation_db{0.0F};
  float keying_level_explained_variation{0.0F};
  bool robust_keying_level_anchor_active{false};
  // Keying speed measured at the widest analysis filter divided by the same
  // measure at the narrowest. Zero until both widths have carried enough
  // keying to answer. One keyer holds near unity; several superimposed run to
  // two or three.
  float keyer_speed_ratio{0.0F};
  // The standing verdict, after hysteresis, from that ratio and from
  // `acoustic_cadence_confidence` above together: either refusing the track is
  // enough. True means "not resolved by the effort spent so far", not
  // "unresolvable".
  bool keyer_overlap_unresolved{false};
  // The evidence a later, more expensive separation stage needs in order to
  // choose which refused tracks are worth attempting: how many other tracked
  // carriers sit inside the neighbourhood such a stage would have to solve
  // jointly, and how far away the closest of them is. Joint estimation is
  // conditioned by the number of sources, so these say whether the problem is
  // a three-source one worth solving or a seven-source one that is not.
  std::uint8_t overlap_neighbour_count{0};
  double nearest_neighbour_separation_hz{0.0};
  // An unresolved track whose neighbourhood is sparse enough for joint
  // estimation to be conditioned. Withheld text either way; this only marks it
  // as worth a second attempt rather than written off.
  bool joint_separation_candidate{false};
  std::string text;
  std::string refined_text;
  std::vector<CwAcousticAlternative> acoustic_alternatives;
  std::string provisional_text;
  double match_age_seconds{0.0};
  std::uint8_t color_index{0};
  bool matched{false};
  bool active{false};
  bool key_down{false};
  bool operator_selected{false};
};

// Allocation-light view used by the optional character frontend on every
// audio block. Keeping transcript strings, character vectors, and acoustic
// alternatives out of this hot path avoids deep diagnostic snapshots at the
// capture cadence; the complete structure above remains available for the
// explicitly rate-limited debug capture.
struct CwCharacterTrackSnapshot {
  std::uint64_t id{0};
  double frequency_hz{0.0};
  double presentation_frequency_hz{0.0};
  float snr_db{0.0F};
  CwTrackState verification_state{CwTrackState::Candidate};
  bool active{false};
  bool operator_selected{false};
};

enum class CwMonitorMode : std::uint8_t {
  Off,
  FullReceiver,
  SelectedTrack,
};

class CwChannelBank {
 public:
  explicit CwChannelBank(CwChannelBankConfig config = {});
  // Applies a new configuration to future evaluation without discarding
  // existing tracks; every field is sanitized exactly as at construction.
  void configure(CwChannelBankConfig config) noexcept;
  // Separate from configure() deliberately: configure() replaces the whole
  // configuration, so a caller that later adjusts one unrelated field would
  // otherwise silently clear this.
  void setOwnCallsign(std::string callsign);
  // Switches the keying model on every live track without disturbing anything
  // else about them. Their decoders restart -- a different technique cannot
  // inherit another's partial state -- but track identity, frequency and the
  // callsign evidence gathered so far all survive, so an operator can compare
  // two models on the same station without losing it.
  // Changing these affects only which tracks are decoded, never which are
  // detected, so it does not disturb tracking or discard decoder state.
  void setWeakSignalDecoding(bool enabled,
                             float minimum_decode_snr_db) noexcept;

  void setKeyingModel(CwKeyingModel model) noexcept;
  // What the operator is doing. It decides whose callsign a monitored stream is
  // expected to carry, which exchange context alone cannot always settle.
  void setOperatorRole(CwOperatorRole role) noexcept { operator_role_ = role; }
  [[nodiscard]] CwOperatorRole operatorRole() const noexcept {
    return operator_role_;
  }
  [[nodiscard]] CwKeyingModel keyingModel() const noexcept {
    return keying_model_;
  }
  void reset() noexcept;

  // The sample stream jumped, but the stations did not.
  //
  // Re-centring the SDR decoder window, which happens on every receiver
  // retune, changes the slice of spectrum being decoded. That is a real
  // discontinuity in the audio -- filters and timing have to start again --
  // but it says nothing about the signals themselves: a track's frequency is
  // absolute RF, and a station on 14.025 MHz is still on 14.025 MHz after the
  // receiver moves. reset() was called here and destroyed every track,
  // transcript and identity on each retune, which is exactly what an operator
  // sees as "I move the RF spectrum and I lose the tracks".
  //
  // This resets the signal path and leaves the tracks standing. Those the new
  // window no longer covers are parked by the ordinary out-of-band rule and
  // return when the receiver does.
  void noteInputDiscontinuity() noexcept;
  // Re-centers every current track by a known audio-domain frequency shift
  // (for example, the shift implied by an operator retuning the linked
  // radio's RX VFO) and resynchronizes each track's narrowband mixer/filter
  // at its new position, without discarding decoded text, verification
  // state/history, or spectral-observation evidence — unlike a track that
  // drifts or jumps far enough to be lost and re-acquired from scratch, a
  // known, deliberate retune should not interrupt an already-identified
  // signal's identity.
  void shiftTrackedFrequencies(double audio_hz_delta) noexcept;
  // Callers that immediately follow with processSamples() may defer snapshot
  // rebuilding to that call, avoiding a duplicate deep presentation copy for
  // the same audio block. The returned reference then remains the prior view.
  [[nodiscard]] const std::vector<CwChannelSnapshot>& updateSpectrum(
      std::uint64_t timestamp_ns, double lower_frequency_hz,
      double upper_frequency_hz, std::span<const float> bins_dbfs,
      bool rebuild_snapshot = true);
  [[nodiscard]] const std::vector<CwChannelSnapshot>& processSamples(
      const RealtimeSampleBlock& block);
  // Selects the provider-neutral receive monitor. Selected-track audio is
  // taken from the same tracking mixers and adaptive narrow filters used by
  // the decoder, mixed with bounded gain, then translated to the requested
  // sidetone. It never affects decoding, radio state, PTT, or KEY.
  void setMonitor(CwMonitorMode mode, std::uint64_t track_id = 0,
                  double reference_tone_hz = 700.0) noexcept;
  void setMonitorTracks(CwMonitorMode mode,
                        std::span<const std::uint64_t> track_ids,
                        double reference_tone_hz = 700.0) noexcept;
  [[nodiscard]] CwMonitorMode monitorMode() const noexcept {
    return monitor_mode_;
  }
  [[nodiscard]] std::uint64_t monitoredTrackId() const noexcept {
    return monitored_track_count_ == 0 ? 0U : monitored_track_ids_[0];
  }
  [[nodiscard]] std::span<const std::uint64_t> monitoredTrackIds()
      const noexcept {
    return {monitored_track_ids_.data(), monitored_track_count_};
  }
  [[nodiscard]] const std::vector<float>& monitorAudio() const noexcept {
    return monitor_audio_;
  }
  // Creates or refreshes a bounded analysis probe at an operator-selected
  // frequency. Returns its track ID, or zero when no valid spectrum range is
  // available or the frequency is outside that range.
  [[nodiscard]] std::uint64_t selectFrequency(double frequency_hz) noexcept;
  // Supplies append-only, overlap-confirmed output from the optional local
  // character model. It may confirm an already Morse-likely acoustic track,
  // but cannot create a track or bypass carrier/keying/cadence qualification.
  [[nodiscard]] bool acceptCharacterRefinement(
      std::uint64_t track_id, const std::string& stable_text,
      std::uint64_t evidence_timestamp_ns);
  [[nodiscard]] const std::vector<CwChannelSnapshot>& channels() const noexcept;
  [[nodiscard]] CwVerificationDiagnostics verificationDiagnostics() const;
  [[nodiscard]] const std::vector<CwCharacterTrackSnapshot>&
  characterRefinementTracks() const noexcept;
  [[nodiscard]] std::vector<CwTrackDiagnostic> allTrackDiagnostics() const;

 private:
  struct Track {
    Track(std::uint64_t track_id, double frequency, std::uint64_t timestamp_ns);

    std::uint64_t id;
    std::uint8_t color_index{0};
    bool color_assigned{false};
    double frequency_hz;
    // Immutable except for a known receiver retune.
    double identity_origin_frequency_hz;
    // Operator-facing center; independent from DSP and identity association.
    double presentation_frequency_hz;
    double drift_hz_per_second{0.0};
    // Carried outside the processed passband by a receiver retune, and waiting
    // for the dial to bring it back rather than being treated as a lost
    // signal. A parked track is still published, so its marker keeps its place
    // on the frequency it belongs to even while that place is off-screen.
    bool parked{false};
    std::uint64_t parked_since_ns{0};
    std::uint64_t last_detected_ns;
    std::uint64_t last_frequency_update_ns;
    std::uint64_t last_candidate_match_ns;
    CwMultiSpeedDecoder decoder;
    CwDecoderUpdate update;
    float snr_db{0.0F};
    float spectral_snr_db{0.0F};
    bool matched{false};
    // A missing spectral association may be an ordinary Morse gap. Once that
    // bounded hold expires, close the current timing segment exactly once and
    // stop feeding residual/adjacent audio until a real candidate matches.
    bool decoder_input_suspended{false};
    bool operator_selected{false};
    std::uint64_t operator_selected_ns{0};
    CwTrackState verification_state{CwTrackState::Candidate};
    CwVerificationReason verification_reason{
        CwVerificationReason::NeedsSpectralPersistence};
    float verification_confidence{0.0F};
    float verification_cadence_quality{0.0F};
    float verification_timing_quality{0.0F};
    float verification_character_confidence{0.0F};
    float narrowband_coherence{0.0F};
    std::uint16_t spectral_observations{0};
    std::uint16_t consecutive_spectrum_misses{0};
    // Elapsed matched/unmatched spectrum time not yet converted into a
    // 60 Hz-equivalent observation step. Keeping the credit in milliseconds
    // makes persistence independent of the spectrum frame rate.
    double matched_evidence_credit_ms{0.0};
    double unmatched_evidence_credit_ms{0.0};
    std::uint16_t verification_pass_samples{0};
    std::uint16_t verification_fail_samples{0};
    std::uint16_t decoder_rejection_samples{0};
    bool ever_verified{false};
    // The strongest level this track has reached. The decode gate reads this
    // rather than the level of the moment, because a signal is weak while it
    // is still being acquired and gating on that suppresses it before it can
    // establish itself -- measured, that lost a callsign whose settled level
    // was thirty-five decibels.
    float peak_decode_level_db{0.0F};
    // How many sample blocks this track has been filtered for. A track is
    // only judged too weak to be worth filtering once it has had long enough
    // to show what its level actually is.
    std::uint32_t decode_level_observations{0};
    bool ever_morse_likely{false};
    // Sticky for the life of the track: having once said CQ, a station does not
    // stop being a station when the word scrolls out of the recent window.
    bool distinctive_token_seen{false};
    std::size_t distinctive_token_scanned_length{0};
    std::uint64_t character_refinement_timestamp_ns{0};
    float keying_snr_db{0.0F};
    // Two-component keying level model held in LINEAR power relative to the
    // side noise reference. Thresholding a dB-domain span at a fixed fraction
    // put the decision far below half amplitude, which lengthened every mark
    // and shortened every gap; the bias grew with signal strength because a
    // stronger carrier widens the dB span.
    // Signal level presented to the operator: the estimated MARK level, not
    // the instantaneous reading. A keyed carrier is present only half the
    // time, so an instantaneous figure swings between roughly +28 dB inside a
    // mark and below zero inside a gap, and whichever value the display
    // sampled tells the operator nothing about the signal. Observed on a
    // receiver capture: a perfectly readable station reported -6.3 dB because
    // the sample landed in a gap.
    float keying_mark_snr_db{0.0F};
    float keying_space_power{0.0F};
    float keying_mark_power{0.0F};
    // Scatter of the observed amplitude about the level it was assigned to.
    // This is the noise on the keying decision itself, and dividing the level
    // separation by it is what converts a bare amplitude distance into
    // evidence: the same gap is decisive on a quiet channel and meaningless
    // on a noisy one.
    float keying_space_variance{0.0F};
    float keying_mark_variance{0.0F};
    bool keying_envelope_initialized{false};
    // A short, allocation-free amplitude history periodically anchors the
    // online two-level tracker to two robust modes.  The per-frame tracker is
    // still needed for attack, fading, and weighting; the history prevents a
    // run of ambiguous edge samples from walking both levels together.
    static constexpr std::size_t kKeyingLevelHistorySize = 256;
    std::array<float, kKeyingLevelHistorySize> keying_level_history{};
    std::size_t keying_level_history_count{0};
    std::size_t keying_level_history_index{0};
    std::uint8_t keying_level_fit_countdown{0};
    float keying_level_explained_variation{0.0F};
    bool robust_keying_level_anchor_active{false};

    // One analysis width's key-run history, kept only to estimate how fast
    // this channel appears to be keyed when heard through that width.
    //
    // The two-level tracker here is deliberately cruder than the decoder's.
    // The decoder wants the best possible decision about each element; this
    // wants to count how often the envelope crosses between its own two
    // levels, and a soft decision would blur exactly the fragmentation being
    // measured. It also carries no minimum separation floor, unlike the
    // decoder's: a model that collapses onto one point means the channel is
    // not keyed at all right now, and that is the honest answer to admit
    // rather than a state to be prevented.
    struct KeyingRuns {
      float space_power{0.0F};
      float mark_power{0.0F};
      bool initialized{false};
      bool key_down{false};
      // Fraction of recent frames on which this width showed a believable
      // two-level split. Smoothed over about a second, so an ordinary word gap
      // barely moves it while a channel that only occasionally looks keyed
      // cannot pretend otherwise.
      float believable_duty{0.0F};
      std::uint32_t run_frames{0};
      static constexpr std::size_t kRunHistorySize = 64;
      std::array<std::uint16_t, kRunHistorySize> run_history{};
      std::size_t run_count{0};
      std::size_t run_index{0};
    };
    // Index 0 is the narrowest analysis width, index 1 the widest.
    std::array<KeyingRuns, 2> keyer_runs{};
    std::uint16_t keyer_evaluation_countdown{0};
    float keyer_speed_ratio{0.0F};
    // Best acoustic cadence fit this track has ever reached.
    //
    // A high-water mark rather than a sample or an interval peak. The fit is
    // measured over the decoder's own cadence window and falls back through a
    // word gap, a pause, or a stretch of the transmission the pileup happened
    // to bury, so a sample of it says as much about when it was taken as about
    // the channel. The question asked here is whether one keyer's cadence ever
    // explained this channel, and a station does not become several keyers by
    // going quiet. Measured: reading the interval peak instead costs a further
    // 0.01 of paired character error on the decoder surface benchmark and
    // refuses no additional carrier on the capture.
    float keyer_cadence_peak{0.0F};
    bool keyer_overlap_unresolved{false};
    // Consecutive ratio evaluations agreeing with each verdict. A verdict only
    // changes once one of these reaches the required run, so a single
    // measurement -- a pileup momentarily thinning, a DX pausing mid-word --
    // cannot flip the track between publishing text and withholding it.
    std::uint8_t keyer_overlap_evaluations{0};
    std::uint8_t keyer_single_evaluations{0};
    // How crowded this carrier's neighbourhood is, refreshed once per audio
    // block from the other live tracks. Recorded for every track, not only
    // refused ones, so the figure is available the moment a verdict changes.
    std::uint8_t overlap_neighbour_count{0};
    double nearest_neighbour_separation_hz{0.0};

    std::array<std::array<std::complex<float>, 3>, 3> center_filters{};
    std::array<std::complex<float>, 3> lower_filter{};
    std::array<std::complex<float>, 3> upper_filter{};
    std::complex<float> center_oscillator{1.0F, 0.0F};
    std::complex<float> lower_oscillator{1.0F, 0.0F};
    std::complex<float> upper_oscillator{1.0F, 0.0F};
    std::array<float, 3> center_power_sums{};
    float lower_power_sum{0.0F};
    float upper_power_sum{0.0F};
    std::size_t accumulated_samples{0};
    float lower_noise_power{0.0F};
    float upper_noise_power{0.0F};
    float monitor_peak_envelope{0.0F};
    std::uint8_t selected_width_index{1};
    std::uint8_t pending_width_index{1};
    std::uint16_t pending_width_observations{0};
    std::uint16_t total_width_observations{0};
    bool noise_initialized{false};
    bool filter_initialized{false};
    static constexpr std::size_t kPresentationEvidenceWindow = 15;
    std::array<double, kPresentationEvidenceWindow>
        presentation_frequency_evidence{};
    std::size_t presentation_frequency_evidence_count{0};
    std::size_t presentation_frequency_evidence_index{0};
    double presentation_follow_median_hz{0.0};
    std::uint64_t presentation_follow_stable_since_ns{0};
    std::uint64_t last_presentation_follow_update_ns{0};
    std::uint64_t last_presentation_evidence_ns{0};
  };

  struct Candidate {
    double frequency_hz{0.0};
    float snr_db{0.0F};
    // An established track may reserve its nearest raw ridge before global
    // peak-separation ranking. This prevents a stronger adjacent skirt/noise
    // peak from suppressing the real ridge and spawning a duplicate identity.
    std::uint64_t preferred_track_id{0};
  };

  struct ColorLease {
    double frequency_hz{0.0};
    std::uint64_t last_seen_ns{0};
    bool occupied{false};
  };

  struct RetainedObservation {
    CwChannelSnapshot snapshot;
    // Presentation continuity has explicit provenance. `source_track_id`
    // owns the live suffix; `inherited_text_prefix` is frozen only when a
    // genuine predecessor is replaced, so composed UI text is never fed back
    // through callsign scoring or appended again on the next refresh.
    std::uint64_t source_track_id{0};
    std::string inherited_text_prefix;
    // The station name this observation earned, latched for as long as one
    // source track owns the slot, together with the evidence that earned it
    // and whatever is currently arguing against it. It is also what marks the
    // observation as identified for retention: a callsign only lands here
    // after a verified track decoded a complete, allocated call, and it
    // survives the call scrolling out of the live text window -- which a rule
    // keyed on the present verification state cannot, because verification
    // decays while a station is not sending. Replacement below resets the
    // whole label, so a successor neither wears the predecessor's name, nor
    // inherits its longer hold, nor has to out-argue evidence that was never
    // about itself.
    CwStreamLabel label;
    std::vector<std::string> confirmed_qso_participants;
    std::uint64_t last_seen_ns{0};
    bool refreshed{false};
  };

  // One color lease per addressable track slot, not per palette entry.
  //
  // It used to be 24 because the display held 24 hand-written colors, and the
  // track cap was then sized down to it. Now the display generates a color for
  // any index, so the palette imposes no limit and the question is only what
  // the lease table is for. It is what stops two concurrently published tracks
  // from wearing the same color, and it is what remembers a color for a
  // frequency across `color_identity_retention_seconds`. Both jobs need one
  // slot per track that can exist at once; with fewer, `assignOrRefreshColor`
  // would find every lease taken as soon as the bank filled, fall through to
  // its modulo fallback, and hand neighbouring carriers matching colors --
  // exactly when the operator needs them apart.
  //
  // Letting colors repeat instead was the alternative, and it is the wrong
  // trade here: repeating costs nothing while two stations sharing a color sit
  // kilohertz apart, but nothing keeps them apart, and the readability this
  // buys back is worth four bytes per slot.
  static constexpr std::size_t kColorLeaseCount = kCwMaximumTrackCapacity;

  [[nodiscard]] float estimateNoise(std::span<const float> bins_dbfs) const;
  [[nodiscard]] float spectralSnr(const Track& track, double lower_frequency_hz,
                                  double bin_width_hz,
                                  std::span<const float> bins_dbfs,
                                  float noise_dbfs) const;
  void sanitizeConfig() noexcept;
  void applyKeyingModel() noexcept;
  // Deliberately not part of CwChannelBankConfig. configure() replaces the
  // whole config, and callers legitimately build one with a single designated
  // initialiser to change one unrelated setting -- the decoded-track retention
  // does exactly that. A keying model living in there would revert to the
  // default every time an unrelated slider moved, silently and only sometimes.
  CwKeyingModel keying_model_{CwKeyingModel::AdaptiveThreshold};
  // Kept out of the config for the same reason as the keying model: configure()
  // replaces the whole config, and a caller changing one unrelated setting with
  // a designated initialiser would silently reset this.
  CwOperatorRole operator_role_{CwOperatorRole::Monitor};
  void resetFilter(Track& track) noexcept;
  // Parks every track the analysed band no longer covers and revives every
  // one it has come back to. One rule, used by both a VFO move and a retune of
  // the IQ decoder window, so the two cannot disagree about what "out of band"
  // means.
  void parkTracksOutsideBand(std::uint64_t timestamp_ns) noexcept;
  // Folds one evidence frame's narrow- and wide-filter levels into the
  // keying-speed estimates, and periodically turns the pair into a standing
  // verdict about how many keyers this channel holds.
  void updateKeyerResolution(Track& track, float narrow_snr_db,
                             float wide_snr_db,
                             double evidence_frame_rate_hz) noexcept;
  // Records, for every track, how many other tracked carriers a joint
  // separation stage would have to solve alongside it and how close the
  // nearest of them is. Run once per audio block rather than per track,
  // because it is one pass over a list every track shares.
  void measureTrackNeighbourhoods() noexcept;
  void updateVerification(Track& track, std::uint64_t timestamp_ns);
  void recoverRejectedDecoder(Track& track);
  void assignOrRefreshColor(Track& track, std::uint64_t timestamp_ns) noexcept;
  void observePresentationFrequency(Track& track, double candidate_frequency_hz,
                                    std::uint64_t timestamp_ns) noexcept;
  void reanchorPresentationOnFirstVerification(
      Track& track, std::uint64_t timestamp_ns) noexcept;
  void followVerifiedPresentation(Track& track,
                                  std::uint64_t timestamp_ns) noexcept;
  [[nodiscard]] bool colorLeaseIsCurrent(
      const ColorLease& lease, std::uint64_t timestamp_ns) const noexcept;
  void rebuildSnapshots(std::uint64_t timestamp_ns);

  CwChannelBankConfig config_;
  std::vector<Track> tracks_;
  std::vector<CwChannelSnapshot> snapshots_;
  std::vector<CwCharacterTrackSnapshot> character_refinement_tracks_;
  std::vector<float> monitor_audio_;
  std::vector<RetainedObservation> retained_observations_;
  std::array<ColorLease, kColorLeaseCount> color_leases_{};
  std::uint64_t next_track_id_{1};
  CwMonitorMode monitor_mode_{CwMonitorMode::Off};
  // One monitor slot per addressable track slot, so every track the bank can
  // hold is a track the operator can put in the headphones. Sized by the track
  // capacity rather than by the color-lease count it used to borrow: the two
  // numbers are equal, but a monitor array has nothing to do with colors, and
  // taking its size from them is how the track cap came to be set by a palette
  // in the first place.
  std::array<std::uint64_t, kCwMaximumTrackCapacity> monitored_track_ids_{};
  std::size_t monitored_track_count_{0};
  double monitor_reference_tone_hz_{700.0};
  std::complex<float> monitor_oscillator_{1.0F, 0.0F};
  StreamDescriptor stream_{};
  std::uint64_t expected_sample_timestamp_ns_{0};
  std::uint64_t last_spectrum_timestamp_ns_{0};
  // Detector-owned averaged spectrum. Display averaging is deliberately not an
  // input to detection; this buffer is smoothed over a fixed time constant so
  // the same signal produces the same candidates at any display frame rate.
  std::vector<float> detector_bins_dbfs_;
  std::vector<float> detector_bins_power_;
  bool detector_average_initialized_{false};
  std::uint64_t last_detector_pass_ns_{0};
  bool detector_pass_initialized_{false};
  double last_spectrum_lower_frequency_hz_{0.0};
  double last_spectrum_upper_frequency_hz_{0.0};
  bool spectrum_range_initialized_{false};
  bool stream_initialized_{false};
  bool sample_timing_initialized_{false};
  std::uint64_t verified_transitions_{0};
  std::uint64_t expired_unverified_tracks_{0};
  std::uint64_t decoder_reacquisitions_{0};
};

}  // namespace cwassistant::core
