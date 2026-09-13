#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cwassistant/core/cw_channel_bank.hpp"
#include <sstream>
#include "cwassistant/core/cw_vocabulary.hpp"
#include "cwassistant/core/iq_replay_source.hpp"
#include "cwassistant/core/iq_writer.hpp"
#include "cwassistant/core/spectrum_analyzer.hpp"
#include "cwassistant/core/wav_replay_source.hpp"
#include "support/decoder_evaluation.hpp"
#include "support/file_sha256.hpp"
#include "support/iq_replay_chain.hpp"
#include "support/receiver_annotations.hpp"

namespace {

struct TextPoint {
  std::uint64_t sample{0};
  std::string text;
  std::string provisional_text;
  double frequency_hz{0.0};
};

struct ObservedTrack {
  std::uint64_t id{0};
  double frequency_hz{0.0};
  std::uint64_t first_sample{0};
  std::uint64_t last_sample{0};
  std::vector<TextPoint> points;
  std::vector<cwassistant::test::TimestampedPublication> publication_points;
  std::string published_callsign;
  bool published{false};
};

std::string textAt(const ObservedTrack& track, const std::uint64_t sample,
                   const bool provisional) {
  std::string result;
  for (const auto& point : track.points) {
    if (point.sample > sample) break;
    result = provisional ? point.provisional_text : point.text;
  }
  return result;
}

std::string textDelta(const std::string& before, const std::string& after) {
  if (!before.empty() && after.starts_with(before))
    return after.substr(before.size());
  return after;
}

std::string joinedCallsigns(const std::vector<std::string>& callsigns) {
  std::string result;
  for (const auto& callsign : callsigns) {
    if (!result.empty()) result.push_back(',');
    result.append(callsign);
  }
  return result;
}

bool overlaps(const ObservedTrack& track,
              const cwassistant::test::ReceiverAnnotation& event) {
  return track.last_sample >= event.start_sample &&
         track.first_sample < event.end_sample;
}

double eventFrequencyDelta(
    const ObservedTrack& track,
    const cwassistant::test::ReceiverAnnotation& event,
    const cwassistant::test::SampleEpisode* episode = nullptr) {
  const std::uint64_t start_sample = episode == nullptr
      ? event.start_sample : std::max(event.start_sample,
                                      episode->start_sample);
  const std::uint64_t end_sample = episode == nullptr
      ? event.end_sample : std::min(event.end_sample, episode->end_sample);
  if (start_sample >= end_sample) return std::numeric_limits<double>::infinity();
  double result = std::numeric_limits<double>::infinity();
  double frequency_at_start = 0.0;
  bool has_frequency_at_start = false;
  for (const auto& point : track.points) {
    if (point.sample <= start_sample) {
      frequency_at_start = point.frequency_hz;
      has_frequency_at_start = true;
    }
    if (point.sample < start_sample || point.sample >= end_sample) continue;
    result = std::min(result, std::abs(point.frequency_hz - event.frequency_hz));
  }
  if (has_frequency_at_start) {
    result = std::min(result,
                      std::abs(frequency_at_start - event.frequency_hz));
  }
  return result;
}

bool episodeOverlapsCoverage(
    const cwassistant::test::SampleEpisode& episode,
    const std::vector<cwassistant::test::ReceiverAnnotationCoverage>& coverage) {
  return std::any_of(coverage.cbegin(), coverage.cend(),
                     [&episode](const auto& interval) {
    return cwassistant::test::halfOpenOverlaps(
        episode, {interval.start_sample, interval.end_sample});
  });
}

bool episodeMatchesEvent(
    const ObservedTrack& track,
    const cwassistant::test::SampleEpisode& episode,
    const cwassistant::test::ReceiverAnnotation& event,
    const double maximum_frequency_delta_hz) {
  return cwassistant::test::halfOpenOverlaps(
             episode, {event.start_sample, event.end_sample}) &&
         eventFrequencyDelta(track, event, &episode) <=
             maximum_frequency_delta_hz;
}

void evaluateAnnotations(
    const cwassistant::test::ReceiverAnnotationManifest& manifest,
    const std::unordered_map<std::uint64_t, ObservedTrack>& observations,
    const std::uint64_t total_samples) {
  using namespace cwassistant::test;
  constexpr double kMaximumFrequencyDeltaHz = 120.0;
  ErrorRate total_cer;
  ErrorRate total_wer;
  CallsignCounts total_callsigns;
  CallsignCounts total_transcript_callsigns;
  std::size_t scored_events = 0;
  std::size_t missed_events = 0;
  std::size_t revisions = 0;
  double scored_seconds = 0.0;
  double provisional_latency_sum = 0.0;
  double stable_latency_sum = 0.0;
  std::size_t provisional_latency_count = 0;
  std::size_t stable_latency_count = 0;

  std::vector<ReceiverAnnotationCoverage> reviewed_coverage =
      manifest.coverage;
  if (reviewed_coverage.empty()) {
    reviewed_coverage.push_back({0U, total_samples});
  }

  for (const auto& event : manifest.events) {
    const ObservedTrack* selected = nullptr;
    double best_delta = std::numeric_limits<double>::infinity();
    for (const auto& [id, track] : observations) {
      static_cast<void>(id);
      if (!overlaps(track, event)) continue;
      const double delta = eventFrequencyDelta(track, event);
      if (delta <= kMaximumFrequencyDeltaHz && delta < best_delta) {
        selected = &track;
        best_delta = delta;
      }
    }
    if (event.uncertain) continue;
    ++scored_events;
    const double event_seconds = static_cast<double>(
        event.end_sample - event.start_sample) / manifest.sample_rate_hz;
    scored_seconds += event_seconds;
    std::string actual;
    if (selected == nullptr) {
      ++missed_events;
    } else {
      const std::string before = textAt(
          *selected, event.start_sample == 0U ? 0U : event.start_sample - 1U,
          false);
      actual = textDelta(
          before, textAt(*selected, event.end_sample - 1U, false));
      const std::string provisional_before = textAt(
          *selected, event.start_sample == 0U ? 0U : event.start_sample - 1U,
          true);
      std::string previous_provisional = provisional_before;
      bool provisional_seen = false;
      bool stable_seen = false;
      for (const auto& point : selected->points) {
        if (point.sample < event.start_sample || point.sample >= event.end_sample)
          continue;
        const std::string stable = textDelta(before, point.text);
        const std::string provisional = textDelta(
            provisional_before, point.provisional_text);
        if (point.provisional_text != previous_provisional &&
            !point.provisional_text.starts_with(previous_provisional)) {
          ++revisions;
        }
        previous_provisional = point.provisional_text;
        if (!provisional_seen &&
            !normalizedText(provisional).empty()) {
          provisional_latency_sum += static_cast<double>(
              point.sample - event.start_sample) / manifest.sample_rate_hz;
          ++provisional_latency_count;
          provisional_seen = true;
        }
        if (!stable_seen && !normalizedText(stable).empty()) {
          stable_latency_sum += static_cast<double>(
              point.sample - event.start_sample) / manifest.sample_rate_hz;
          ++stable_latency_count;
          stable_seen = true;
        }
      }
    }

    const auto cer = characterErrorRate(event.normalized_text, actual);
    const auto wer = wordErrorRate(event.normalized_text, actual);
    total_cer.edits += cer.edits;
    total_cer.references += cer.references;
    total_wer.edits += wer.edits;
    total_wer.references += wer.references;
    std::vector<std::string> transcript_callsigns =
        cwassistant::core::CallsignPolicy::qso_participants_in_text(actual + " ");
    if (const auto call = cwassistant::core::CallsignPolicy::best_complete_in_text(
            actual + " "); call) {
      transcript_callsigns.push_back(*call);
    }
    const auto published_callsigns = selected == nullptr
        ? std::vector<std::string>{}
        : callsignsAtOrBefore(selected->publication_points,
                              event.end_sample - 1U);
    const auto calls = callsignCounts(event.callsigns, published_callsigns);
    total_callsigns.true_positives += calls.true_positives;
    total_callsigns.false_positives += calls.false_positives;
    total_callsigns.false_negatives += calls.false_negatives;
    const auto transcript_calls = callsignCounts(
        event.callsigns, transcript_callsigns);
    total_transcript_callsigns.true_positives +=
        transcript_calls.true_positives;
    total_transcript_callsigns.false_positives +=
        transcript_calls.false_positives;
    total_transcript_callsigns.false_negatives +=
        transcript_calls.false_negatives;
    std::cout << "annotation start_sample=" << event.start_sample
              << " end_sample=" << event.end_sample
              << " frequency_hz=" << event.frequency_hz
              << " matched_track=" << (selected == nullptr ? 0U : selected->id)
              << " expected=\"" << event.normalized_text
              << "\" actual=\"" << normalizedText(actual)
              << "\" cer=" << cer.rate() << " wer=" << wer.rate()
              << " published_callsigns=\""
              << joinedCallsigns(published_callsigns)
              << "\" transcript_callsigns=\""
              << joinedCallsigns(transcript_callsigns) << '"'
              << '\n';
  }

  std::size_t unmatched_publications = 0;
  std::size_t unmatched_callsigns = 0;
  for (const auto& [id, track] : observations) {
    static_cast<void>(id);
    for (const auto& episode : publicationEpisodes(
             track.publication_points, total_samples)) {
      if (!episodeOverlapsCoverage(episode, reviewed_coverage)) continue;
      const bool protected_by_event = std::any_of(
          manifest.events.cbegin(), manifest.events.cend(),
          [&](const auto& event) {
        return episodeMatchesEvent(track, episode, event,
                                   kMaximumFrequencyDeltaHz);
      });
      if (!protected_by_event) ++unmatched_publications;
    }
    for (const auto& call_episode : callsignEpisodes(
             track.publication_points, total_samples)) {
      if (!episodeOverlapsCoverage(call_episode, reviewed_coverage)) continue;
      bool matches_expected_call = false;
      bool matches_uncertain_event = false;
      bool overlaps_certain_event = false;
      for (const auto& event : manifest.events) {
        if (!episodeMatchesEvent(track, call_episode, event,
                                 kMaximumFrequencyDeltaHz)) {
          continue;
        }
        if (event.uncertain) {
          matches_uncertain_event = true;
          continue;
        }
        overlaps_certain_event = true;
        if (std::find(event.callsigns.cbegin(), event.callsigns.cend(),
                      call_episode.callsign) != event.callsigns.cend()) {
          matches_expected_call = true;
        }
      }
      const bool protected_by_event = callsignEpisodeIsProtected(
          matches_expected_call, overlaps_certain_event,
          matches_uncertain_event);
      if (!protected_by_event) ++unmatched_callsigns;
    }
  }
  const double reviewed_minutes =
      static_cast<double>(reviewedCoverageSamples(reviewed_coverage)) /
      manifest.sample_rate_hz / 60.0;
  std::cout << "annotation_summary scored_events=" << scored_events
            << " missed_events=" << missed_events
            << " reviewed_seconds=" << reviewed_minutes * 60.0
            << " cer=" << total_cer.rate()
            << " wer=" << total_wer.rate()
            << " callsign_precision=" << total_callsigns.precision()
            << " callsign_recall=" << total_callsigns.recall()
            << " transcript_callsign_precision="
            << total_transcript_callsigns.precision()
            << " transcript_callsign_recall="
            << total_transcript_callsigns.recall()
            << " mean_provisional_latency_seconds="
            << (provisional_latency_count == 0U ? -1.0 :
                provisional_latency_sum / provisional_latency_count)
            << " mean_stable_latency_seconds="
            << (stable_latency_count == 0U ? -1.0 :
                stable_latency_sum / stable_latency_count)
            << " revisions_per_annotated_minute="
            << (scored_seconds <= 0.0 ? 0.0 :
                static_cast<double>(revisions) * 60.0 / scored_seconds)
            << " false_publication_episodes=" << unmatched_publications
            << " false_callsign_episodes=" << unmatched_callsigns
            << " unmatched_publications_per_minute="
            << (reviewed_minutes <= 0.0 ? 0.0 :
                unmatched_publications / reviewed_minutes)
            << " unmatched_callsigns_per_minute="
            << (reviewed_minutes <= 0.0 ? 0.0 :
                unmatched_callsigns / reviewed_minutes) << '\n';
}

// Everything a replay accumulates about the decode, independent of which kind
// of recording produced the samples. Split out of the audio replay when the
// SigMF path arrived: the two sources differ entirely in how a block reaches
// the channel bank and not at all in what is observed once it has, so a second
// copy of this bookkeeping would be two scores that drift apart silently.
struct ReplayState {
  std::unordered_set<std::uint64_t> published_ids;
  std::unordered_map<std::uint64_t, cwassistant::core::CwChannelSnapshot>
      latest_published;
  std::unordered_map<std::uint64_t, ObservedTrack> observations;
  std::size_t maximum_tracks{0};
  std::size_t maximum_published{0};
};

void observeBlock(
    const std::string& path,
    const std::vector<cwassistant::core::CwTrackDiagnostic>& diagnostics,
    const std::vector<cwassistant::core::CwChannelSnapshot>& published,
    const std::uint64_t current_sample, const double elapsed_seconds,
    ReplayState& state) {
  for (const auto& diagnostic : diagnostics) {
    auto [entry, inserted] = state.observations.try_emplace(diagnostic.id);
    auto& observed = entry->second;
    if (inserted) {
      observed.id = diagnostic.id;
      observed.first_sample = current_sample;
    }
    observed.last_sample = current_sample;
    observed.frequency_hz = diagnostic.presentation_frequency_hz;
    const bool changed = observed.points.empty() ||
        observed.points.back().text != diagnostic.text ||
        observed.points.back().provisional_text != diagnostic.provisional_text ||
        std::abs(observed.points.back().frequency_hz -
                 diagnostic.presentation_frequency_hz) > 0.01;
    if (changed) {
      observed.points.push_back({current_sample, diagnostic.text,
                                 diagnostic.provisional_text,
                                 diagnostic.presentation_frequency_hz});
    }
  }
  state.maximum_tracks = std::max(state.maximum_tracks, diagnostics.size());
  state.maximum_published =
      std::max(state.maximum_published, published.size());
  std::unordered_set<std::uint64_t> current_published_ids;
  for (const auto& channel : published) {
    current_published_ids.insert(channel.id);
    state.latest_published[channel.id] = channel;
    auto& observed = state.observations[channel.id];
    observed.published_callsign = channel.callsign;
    observed.published = true;
    std::vector<std::string> published_callsigns = channel.qso_participants;
    if (!channel.callsign.empty())
      published_callsigns.push_back(channel.callsign);
    std::sort(published_callsigns.begin(), published_callsigns.end());
    published_callsigns.erase(
        std::unique(published_callsigns.begin(), published_callsigns.end()),
        published_callsigns.end());
    if (observed.publication_points.empty() ||
        !observed.publication_points.back().published ||
        observed.publication_points.back().callsigns != published_callsigns) {
      observed.publication_points.push_back(
          {current_sample, true, std::move(published_callsigns)});
    }
    if (!state.published_ids.insert(channel.id).second) continue;
    std::cout << "published path=\"" << path << "\" time_s="
              << elapsed_seconds << " id=" << channel.id
              << " color=" << static_cast<unsigned>(channel.color_index)
              << " frequency_hz=" << channel.frequency_hz
              << " presentation_frequency_hz="
              << channel.presentation_frequency_hz
              << " wpm=" << channel.wpm
              << " acoustic_wpm=" << channel.acoustic_wpm
              << " cadence_fit="
              << channel.acoustic_cadence_confidence
              << " confidence=" << channel.verification_confidence
              << " text=\"" << channel.text << "\""
              << " refined_text=\"" << channel.refined_text << "\"\n";
  }
  for (auto& [id, observed] : state.observations) {
    if (!current_published_ids.contains(id) &&
        !observed.publication_points.empty() &&
        observed.publication_points.back().published) {
      observed.publication_points.push_back({current_sample, false, {}});
    }
  }
}

void reportDecode(const std::string& path, const ReplayState& state,
                  const cwassistant::core::CwChannelBank& channels,
                  const double duration_seconds) {
  for (const auto& [id, channel] : state.latest_published) {
    std::cout << "final path=\"" << path << "\" id=" << id
              << " frequency_hz=" << channel.frequency_hz
              << " timing_quality=" << channel.verification_timing_quality
              << " snr_db=" << channel.snr_db
              << " callsign=\"" << channel.callsign << "\""
              << " turns=" << channel.transmissions.size()
              << " sender=\"" << channel.current_sender_callsign << "\""
              << " sender_wpm=" << channel.current_sender_wpm
              << " text=\"" << channel.text << "\""
              << " refined_text=\"" << channel.refined_text << "\""
              << " alternatives=" << channel.acoustic_alternatives.size();
    if (!channel.acoustic_alternatives.empty()) {
      const auto& best = channel.acoustic_alternatives.front();
      std::cout << " best_alternative=\"" << best.text << "\""
                << " best_cost=" << best.acoustic_cost
                << " best_confidence=" << best.evidence_confidence;
    }
    std::cout << '\n';
    for (const auto& turn : channel.transmissions) {
      std::cout << "turn path=\"" << path << "\" id=" << id
                << " sequence=" << turn.sequence
                << " sender=\"" << turn.sender_callsign << "\""
                << " wpm=" << turn.wpm
                << " cadence_confidence=" << turn.cadence_confidence;
      if (!turn.timing_fingerprint) {
        std::cout << " timing_fingerprint=unavailable\n";
        continue;
      }
      const auto& timing = *turn.timing_fingerprint;
      std::cout << " timing_first_observation_id="
                << timing.first_observation_id
                << " timing_last_observation_id="
                << timing.last_observation_id
                << " timing_started_ns=" << timing.evidence_started_ns
                << " timing_ended_ns=" << timing.evidence_ended_ns
                << " timing_mark_count=" << timing.mark_count
                << " timing_gap_count=" << timing.gap_count
                << " timing_dit_count=" << timing.dit_count
                << " timing_dah_count=" << timing.dah_count
                << " timing_element_gap_count="
                << timing.element_gap_count
                << " timing_character_gap_count="
                << timing.character_gap_count
                << " timing_word_gap_count=" << timing.word_gap_count
                << " timing_dit_median_ms=" << timing.dit_median_ms
                << " timing_dah_median_ms=" << timing.dah_median_ms
                << " timing_element_gap_median_ms="
                << timing.element_gap_median_ms
                << " timing_character_gap_median_ms="
                << timing.character_gap_median_ms
                << " timing_word_gap_median_ms="
                << timing.word_gap_median_ms
                << " timing_keying_weight=" << timing.keying_weight
                << " timing_normalized_mark_residual="
                << timing.normalized_mark_residual
                << " timing_evidence_confidence="
                << timing.evidence_confidence << '\n';
    }
  }

  const auto verification = channels.verificationDiagnostics();
  std::cout << "summary path=\"" << path << "\" duration_s="
            << duration_seconds << " maximum_tracks="
            << state.maximum_tracks << " maximum_published="
            << state.maximum_published
            << " verified_transitions=" << verification.verified_transitions
            << " decoder_reacquisitions="
            << verification.decoder_reacquisitions
            << " expired_unverified="
            << verification.expired_unverified_tracks << '\n';
}

int replayAudio(
    const std::string& path,
    const cwassistant::test::ReceiverAnnotationManifest* annotations) {
  using namespace std::chrono_literals;
  using namespace cwassistant::core;

  WavReplaySource source;
  if (!source.open(path, {.kind = StreamKind::Audio})) {
    std::cerr << path << ": " << source.last_error() << '\n';
    return 1;
  }
  if (annotations != nullptr && annotations->sample_rate_hz !=
                                    static_cast<std::uint32_t>(
                                        source.stream_descriptor()
                                            .sample_rate_hz)) {
    std::cerr << path << ": annotation sample rate does not match audio\n";
    return 2;
  }
  if (annotations != nullptr) {
    if (!cwassistant::test::receiverAnnotationsFitAudio(
            *annotations, source.total_frames())) {
      std::cerr << path << ": annotation data exceeds audio duration\n";
      return 2;
    }
  }
  if (annotations != nullptr &&
      cwassistant::test::fileSha256(path) != annotations->audio_sha256) {
    std::cerr << path << ": annotation audio SHA-256 does not match\n";
    return 2;
  }
  source.start();
  SpectrumAnalyzer analyzer({.audio_upper_frequency_hz = 3'000.0});
  CwChannelBank channels;
  ReplayState state;

  RealtimeSampleBlock block;
  while (source.read(block, 0ms)) {
    for (const auto& spectrum : analyzer.process(block)) {
      static_cast<void>(channels.updateSpectrum(
          spectrum.timestamp_ns, spectrum.lower_frequency_hz,
          spectrum.upper_frequency_hz, spectrum.bins_dbfs));
    }
    const auto& published = channels.processSamples(block);
    const std::uint64_t current_sample =
        source.position_frames() == 0U ? 0U : source.position_frames() - 1U;
    observeBlock(path, channels.allTrackDiagnostics(), published,
                 current_sample,
                 static_cast<double>(source.position_frames()) /
                     source.stream_descriptor().sample_rate_hz,
                 state);
  }

  reportDecode(path, state, channels, source.duration_seconds());
  if (annotations != nullptr) {
    evaluateAnnotations(*annotations, state.observations,
                        source.total_frames());
  }
  return 0;
}

// The decoder window and how it was arrived at. Kept separate from the parsed
// command line so that what the replay actually decoded is reportable: a
// window taken from the capture's own centre and one an operator typed are the
// same numbers by the time the decimator sees them, and a reader of the output
// has to be able to tell which happened.
struct CaptureReplayRequest {
  bool have_center_frequency{false};
  double center_frequency_hz{0.0};
  double bandwidth_hz{cwassistant::test::kDefaultIqDecoderBandwidthHz};
};

// An RF frequency needs more than six significant digits before it is a
// frequency at all: the stream default prints 7'015'005 Hz as 7.01501e+06,
// which cannot be compared with an annotation, a band plan or another track
// 70 Hz away. Restored on the way out so an audio replay in the same run keeps
// the output it has always produced.
class ScopedRfPrecision {
 public:
  // Both streams. Every guard below reports the window it refused on cerr,
  // and a refusal that cannot name the frequency it refused is no more usable
  // than a decode that cannot name the frequency it found.
  ScopedRfPrecision()
      : previous_out_(std::cout.precision(10)),
        previous_err_(std::cerr.precision(10)) {}
  ~ScopedRfPrecision() {
    std::cout.precision(previous_out_);
    std::cerr.precision(previous_err_);
  }
  ScopedRfPrecision(const ScopedRfPrecision&) = delete;
  ScopedRfPrecision& operator=(const ScopedRfPrecision&) = delete;

 private:
  std::streamsize previous_out_;
  std::streamsize previous_err_;
};

std::string_view blockStatusName(
    const cwassistant::core::IqBlockStatus status) {
  using cwassistant::core::IqBlockStatus;
  switch (status) {
    case IqBlockStatus::Accepted: return "accepted";
    case IqBlockStatus::AcceptedAfterDiscontinuity:
      return "accepted_after_discontinuity";
    case IqBlockStatus::RejectedStreamKind: return "not_complex_iq";
    case IqBlockStatus::RejectedDescriptor: return "window_outside_passband";
    case IqBlockStatus::RejectedSampleCount: return "empty_block";
    case IqBlockStatus::RejectedNonFiniteSample: return "non_finite_sample";
    case IqBlockStatus::RejectedSampleMagnitude: return "sample_over_range";
  }
  return "unknown";
}

int replayCapture(const std::string& path,
                  const cwassistant::test::ReceiverAnnotationManifest*
                      annotations,
                  const CaptureReplayRequest& request) {
  using namespace std::chrono_literals;
  using namespace cwassistant::core;

  const ScopedRfPrecision precision;
  IqReplaySource source;
  if (!source.open(path, {.kind = StreamKind::ComplexIq})) {
    std::cerr << path << ": " << source.last_error() << '\n';
    return 1;
  }
  const std::string data_path = IqReplaySource::dataPathFor(path);
  const std::string metadata_path = IqWriter::metadataPathFor(data_path);
  const auto& metadata = source.capture_metadata();
  const auto& segments = source.capture_segments();
  std::cout << "capture path=\"" << data_path << "\" sample_rate_hz="
            << metadata.sample_rate_hz << " samples=" << source.total_frames()
            << " declared_samples=" << source.declared_sample_count()
            << " duration_s=" << source.duration_seconds()
            << " center_frequency_hz=" << metadata.center_frequency_hz
            << " segments=" << segments.size()
            << " format="
            << (metadata.format == IqSampleFormat::Cf32Le ? "cf32_le"
                                                          : "ci16_le")
            // A short recording is normal when the writer says why it stopped.
            // Printed next to the duration so a reader is never left deciding
            // between "the operator pressed stop" and "the capture failed".
            << " stop_reason=\"" << source.recorded_stop_reason() << "\""
            << " ends_mid_sample=" << (source.ends_mid_sample() ? 1 : 0)
            << " hardware=\"" << metadata.hardware << "\""
            << " description=\"" << metadata.description << "\"\n";
  for (std::size_t index = 0; index < segments.size(); ++index) {
    std::cout << "capture_segment path=\"" << data_path << "\" index="
              << index << " sample_start=" << segments[index].sample_start
              << " frequency_hz=" << segments[index].frequency_hz
              << " datetime=\"" << segments[index].datetime_utc << "\"\n";
  }

  // The capture's own centre is the only default that cannot be wrong about
  // the recording, but it is frequently wrong about the operator: a receiver
  // tuned 16 kHz away from the signals it was watching produces a capture
  // whose centre holds nothing at all. The sidecar's description records the
  // window that was actually in use, which is why it is printed above.
  cwassistant::test::IqDecoderWindow window =
      cwassistant::test::defaultIqDecoderWindow(source);
  window.bandwidth_hz = request.bandwidth_hz;
  if (request.have_center_frequency)
    window.center_frequency_hz = request.center_frequency_hz;
  std::size_t covered_segments = 0;
  for (const auto& segment : segments) {
    if (cwassistant::test::iqDecoderWindowFitsPassband(
            window, {.kind = StreamKind::ComplexIq,
                     .sample_rate_hz = metadata.sample_rate_hz,
                     .center_frequency_hz = segment.frequency_hz,
                     .channel_count = 1})) {
      ++covered_segments;
    }
  }
  std::cout << "decoder_window path=\"" << data_path << "\" center_frequency_hz="
            << window.center_frequency_hz
            << " bandwidth_hz=" << window.bandwidth_hz
            << " source=" << (request.have_center_frequency ? "requested"
                                                            : "capture_center")
            << " covered_segments=" << covered_segments << '/'
            << segments.size() << '\n';
  if (covered_segments == 0) {
    // Refused rather than replayed. The decimator would answer
    // RejectedDescriptor for every block while the overview transform kept
    // painting a normal spectrum, and the report would read as a decoder that
    // found nothing in a busy band.
    std::cerr << data_path << ": decoder window "
              << window.center_frequency_hz << " Hz +/- "
              << window.bandwidth_hz * 0.5
              << " Hz lies outside the captured passband ("
              << metadata.sample_rate_hz << " Hz wide); nothing would reach "
                 "the decoder\n";
    return 2;
  }

  cwassistant::test::IqReplayChain chain;
  if (!chain.configure(window)) {
    std::cerr << data_path << ": the decimator refused a "
              << window.bandwidth_hz << " Hz decoder window\n";
    return 2;
  }

  if (annotations != nullptr) {
    // The capture rate, not the decoder branch rate. Annotation sample
    // positions index the recording, which is what total_frames() counts and
    // what the observations below are stamped with.
    if (annotations->sample_rate_hz !=
        static_cast<std::uint32_t>(metadata.sample_rate_hz)) {
      std::cerr << data_path << ": annotation sample rate does not match the "
                   "capture\n";
      return 2;
    }
    if (!cwassistant::test::receiverAnnotationsFitAudio(
            *annotations, source.total_frames())) {
      std::cerr << data_path << ": annotation data exceeds capture duration\n";
      return 2;
    }
    // Both halves of the pair. The payload alone would let the sidecar -- and
    // with it the declared sample rate and every capture segment's centre
    // frequency, which decide what a track's absolute RF means -- be rewritten
    // under an annotation set that still verified.
    if (cwassistant::test::filesSha256({data_path, metadata_path}) !=
        annotations->audio_sha256) {
      std::cerr << data_path << ": annotation capture SHA-256 does not match "
                   "the .sigmf-data and .sigmf-meta pair\n";
      return 2;
    }
    // Tracks from a complex capture carry absolute RF, so annotations must
    // too. An audio sidecar's frequencies are hundreds of hertz; against a
    // 7 MHz capture every event would simply miss, and the report would blame
    // the decoder for a coordinate system.
    double lowest_hz = std::numeric_limits<double>::infinity();
    double highest_hz = -std::numeric_limits<double>::infinity();
    for (const auto& segment : segments) {
      lowest_hz = std::min(lowest_hz,
                           segment.frequency_hz - metadata.sample_rate_hz * 0.5);
      highest_hz = std::max(
          highest_hz, segment.frequency_hz + metadata.sample_rate_hz * 0.5);
    }
    for (const auto& event : annotations->events) {
      if (event.frequency_hz < lowest_hz || event.frequency_hz > highest_hz) {
        std::cerr << data_path << ": annotation frequency "
                  << event.frequency_hz << " Hz lies outside the captured RF "
                     "span " << lowest_hz << ".." << highest_hz
                  << " Hz; a capture's annotations are absolute RF\n";
        return 2;
      }
    }
  }

  if (!source.start()) {
    std::cerr << data_path << ": " << source.last_error() << '\n';
    return 1;
  }
  CwChannelBank channels;
  ReplayState state;

  RealtimeSampleBlock block;
  while (source.read(block, 0ms)) {
    const auto frame = chain.process(block);
    // Stamped with the capture position rather than the decoder branch's own
    // sample count, so an annotation written against the recording and a track
    // observed through the decimator are on one timeline.
    const std::uint64_t current_sample =
        source.position_frames() == 0U ? 0U : source.position_frames() - 1U;
    if (frame.block != nullptr) {
      for (const auto& spectrum : frame.spectra) {
        const auto view = chain.detectorView(spectrum);
        // Unaveraged bins and no snapshot rebuild, exactly as the live worker
        // feeds detection: averaging is a display setting and must not decide
        // which signals are discovered.
        static_cast<void>(channels.updateSpectrum(
            spectrum.timestamp_ns, view.lower_frequency_hz,
            view.upper_frequency_hz, view.bins_dbfs, false));
      }
      const auto& published = channels.processSamples(*frame.block);
      observeBlock(data_path, channels.allTrackDiagnostics(), published,
                   current_sample,
                   static_cast<double>(source.position_frames()) /
                       metadata.sample_rate_hz,
                   state);
    }
    chain.advance();
  }

  std::cout << "capture_chain path=\"" << data_path << "\" wide_blocks="
            << chain.wideBlocks() << " accepted_blocks="
            << chain.acceptedBlocks() << " rejected_blocks="
            << chain.rejectedBlocks() << " decoder_blocks="
            << chain.decoderBlocks() << " decoder_samples="
            << chain.decoderSamples() << " decoder_sample_rate_hz="
            << chain.decoderSampleRateHz() << " detector_frames="
            << chain.detectorFrames();
  // Named, not counted. A capture that retunes into a segment the window does
  // not cover rejects blocks halfway through an otherwise normal replay, and
  // "some blocks were refused" does not say whether the window left the
  // passband or the samples themselves were bad.
  if (chain.rejectedBlocks() > 0U)
    std::cout << " rejection=" << blockStatusName(chain.lastRejection());
  std::cout << '\n';
  reportDecode(data_path, state, channels, source.duration_seconds());
  if (annotations != nullptr) {
    evaluateAnnotations(*annotations, state.observations,
                        source.total_frames());
  }
  return 0;
}

int replay(const std::string& path,
           const cwassistant::test::ReceiverAnnotationManifest* annotations,
           const CaptureReplayRequest& request) {
  // Either half of a SigMF pair is accepted, because an operator handing over
  // a capture reaches for whichever name their file browser showed them.
  if (path.ends_with(".sigmf-data") || path.ends_with(".sigmf-meta"))
    return replayCapture(path, annotations, request);
  return replayAudio(path, annotations);
}

}  // namespace

namespace {

// The replay tool decodes with the same vocabulary the application loads, so a
// before/after audit measures the shipped dictionaries rather than an empty
// one. Without this the context rescorer would contribute nothing here.
void loadShippedDictionaries() {
  const auto read = [](const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
  };
  const std::string directory = CWA_DICTIONARY_DIR;
  auto& vocabulary = cwassistant::core::cwSharedVocabulary();
  vocabulary.clear();
  static_cast<void>(vocabulary.importExchangeWords(
      read(directory + "/cw-abbreviations.txt")));
  static_cast<void>(vocabulary.importWordGapPrefixes(
      read(directory + "/cw-word-gap-prefixes.txt")));
  static_cast<void>(vocabulary.importDistinctiveTokens(
      read(directory + "/cw-distinctive-tokens.txt")));
}

// Locale-independent, and refuses anything the whole field is not, so a
// mistyped frequency becomes a diagnosed argument rather than a silently
// truncated one.
bool parseFrequencyArgument(const std::string_view text, double& value) {
  std::istringstream input{std::string(text)};
  input.imbue(std::locale::classic());
  input >> value;
  return input && input.peek() == std::char_traits<char>::eof() &&
         std::isfinite(value) && value > 0.0;
}

}  // namespace

int main(const int argc, char** argv) {
  loadShippedDictionaries();
  const auto usage = [] {
    std::cerr << "usage: cwa_capture_replay [--annotations sidecar.tsv]\n"
                 "                          [--decoder-center-hz <Hz>]\n"
                 "                          [--decoder-bandwidth-hz <Hz>]\n"
                 "                          <recording> [recording ...]\n"
                 "  recording: an audio .wav, or either half of a SigMF pair\n"
                 "             (.sigmf-data / .sigmf-meta).\n"
                 "  The decoder window applies to SigMF captures only and\n"
                 "  defaults to the capture's own centre frequency at "
              << cwassistant::test::kDefaultIqDecoderBandwidthHz
              << " Hz wide.\n";
  };
  cwassistant::test::ReceiverAnnotationManifest annotations;
  const cwassistant::test::ReceiverAnnotationManifest* annotation_pointer =
      nullptr;
  CaptureReplayRequest request;
  int index = 1;
  for (; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (!argument.starts_with("--")) break;
    if (index + 1 >= argc) {
      std::cerr << argument << " needs a value\n";
      usage();
      return 2;
    }
    const std::string_view value{argv[++index]};
    if (argument == "--annotations") {
      std::ifstream input{std::string(value)};
      std::string error;
      if (!input.is_open() || !cwassistant::test::parseReceiverAnnotations(
                                  input, annotations, error)) {
        std::cerr << "annotation sidecar: "
                  << (error.empty() ? "cannot open file" : error) << '\n';
        return 2;
      }
      annotation_pointer = &annotations;
    } else if (argument == "--decoder-center-hz") {
      if (!parseFrequencyArgument(value, request.center_frequency_hz)) {
        std::cerr << "--decoder-center-hz: not a frequency: " << value << '\n';
        return 2;
      }
      request.have_center_frequency = true;
    } else if (argument == "--decoder-bandwidth-hz") {
      if (!parseFrequencyArgument(value, request.bandwidth_hz)) {
        std::cerr << "--decoder-bandwidth-hz: not a width: " << value << '\n';
        return 2;
      }
    } else {
      std::cerr << "unknown option " << argument << '\n';
      usage();
      return 2;
    }
  }
  if (index >= argc) {
    usage();
    return 2;
  }
  // One sidecar describes one recording: its sample positions and its digest
  // are both specific to that file.
  if (annotation_pointer != nullptr && argc - index != 1) {
    std::cerr << "annotation mode binds one sidecar to exactly one "
                 "recording\n";
    return 2;
  }
  int status = 0;
  for (; index < argc; ++index) {
    status = std::max(status,
                      replay(argv[index], annotation_pointer, request));
  }
  return status;
}
