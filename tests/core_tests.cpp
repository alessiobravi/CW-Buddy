#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "cwassistant/core/adif.hpp"
#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/cat4om_protocol.hpp"
#include "cwassistant/core/channel_scheduler.hpp"
#include "cwassistant/core/cw_channel_bank.hpp"
#include <sstream>
#include "cwassistant/core/cw_callsign_prefixes.hpp"
#include "cwassistant/core/cw_morse_alphabet.hpp"
#include "cwassistant/core/cw_vocabulary.hpp"
#include "cwassistant/core/cw_context_rescorer.hpp"
#include "cwassistant/core/cw_decoder.hpp"
#include "cwassistant/core/cw_transmit_encoder.hpp"
#include "cwassistant/core/frequency_plan.hpp"
#include "cwassistant/core/reference_rig_profiles.hpp"
#include "cwassistant/core/remote_control.hpp"
#include "cwassistant/core/spectrum_analyzer.hpp"
#include "cwassistant/core/spectrum_visualization_settings.hpp"
#include "cwassistant/core/spsc_ring_buffer.hpp"
#include "cwassistant/core/station_equipment.hpp"
#include "cwassistant/core/transmit_guard.hpp"
#include "cwassistant/core/wav_replay_source.hpp"
#include "cwassistant/core/wav_writer.hpp"

namespace {

static_assert(
    std::is_trivially_copyable_v<cwassistant::core::CwCharacterTrackSnapshot>);
static_assert(sizeof(cwassistant::core::CwCharacterTrackSnapshot) <= 64U);

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void write_u16(std::ostream& stream, const std::uint16_t value) {
  stream.put(static_cast<char>(value & 0xFFU));
  stream.put(static_cast<char>((value >> 8U) & 0xFFU));
}

void write_u32(std::ostream& stream, const std::uint32_t value) {
  stream.put(static_cast<char>(value & 0xFFU));
  stream.put(static_cast<char>((value >> 8U) & 0xFFU));
  stream.put(static_cast<char>((value >> 16U) & 0xFFU));
  stream.put(static_cast<char>((value >> 24U) & 0xFFU));
}

std::filesystem::path write_test_wav() {
  constexpr std::uint32_t sample_rate = 8'000;
  constexpr std::uint16_t channels = 2;
  constexpr std::uint32_t frame_count = 5'000;
  constexpr std::uint32_t data_size = frame_count * channels * 2U;
  const auto path =
      std::filesystem::temp_directory_path() / "cwassistant-replay-test.wav";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write("RIFF", 4);
  write_u32(output, 36U + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16);
  write_u16(output, 1);
  write_u16(output, channels);
  write_u32(output, sample_rate);
  write_u32(output, sample_rate * channels * 2U);
  write_u16(output, channels * 2U);
  write_u16(output, 16);
  output.write("data", 4);
  write_u32(output, data_size);
  for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
    write_u16(output, static_cast<std::uint16_t>(16'384));
    write_u16(output, static_cast<std::uint16_t>(8'192));
  }
  return path;
}

void test_ring_buffer() {
  cwassistant::core::SpscRingBuffer<int, 2> queue;
  expect(queue.empty(), "new ring is empty");
  expect(queue.try_push(10), "push first item");
  expect(queue.try_push(20), "push second item");
  expect(!queue.try_push(30), "bounded ring reports full");

  int value = 0;
  expect(queue.try_pop(value) && value == 10, "ring preserves FIFO order");
  expect(queue.try_pop(value) && value == 20, "ring pops second item");
  expect(!queue.try_pop(value), "empty ring reports empty");
}

void test_scheduler() {
  using namespace cwassistant::core;
  const std::vector<DetectedChannel> channels{
      {.id = 1, .snr_db = 4.0F, .arrival_sequence = 30},
      {.id = 2, .snr_db = 18.0F, .arrival_sequence = 20},
      {.id = 3, .snr_db = 8.0F, .arrival_sequence = 10, .user_selected = true},
  };
  ChannelScheduler scheduler;
  expect(
      scheduler.select(channels, 2, ChannelSelectionPolicy::StrongestSignal) ==
          std::vector<std::uint64_t>({2, 3}),
      "strongest policy ranks by SNR");
  expect(scheduler.select(channels, 2, ChannelSelectionPolicy::ArrivalQueue) ==
             std::vector<std::uint64_t>({3, 2}),
         "queue policy ranks by arrival");
  expect(scheduler.select(channels, 2,
                          ChannelSelectionPolicy::UserSelectedFirst) ==
             std::vector<std::uint64_t>({3, 2}),
         "manual choice is scheduled first");
}

void test_cw_timing_decoder() {
  using cwassistant::core::CwTimingDecoder;
  CwTimingDecoder decoder({.initial_wpm = 20.0});
  std::uint64_t now = 0;
  const auto feed = [&](const bool down, const int milliseconds) {
    const int steps = milliseconds / 10;
    for (int i = 0; i < steps; ++i) {
      now += 10'000'000;
      static_cast<void>(decoder.process(now, down ? 12.0F : 0.0F));
    }
  };
  feed(false, 100);
  feed(true, 60);
  feed(false, 60);
  feed(true, 60);
  feed(false, 200);
  feed(true, 60);
  feed(false, 60);
  feed(true, 60);
  feed(false, 60);
  feed(true, 60);
  feed(false, 200);
  const auto result = decoder.flush(now + 500'000'000);
  expect(result.text.find("IS") != std::string::npos,
         "adaptive CW timing decodes deterministic dit sequences");
  expect(result.wpm > 18.0 && result.wpm < 22.0,
         "adaptive CW timing reports the keyed speed");
  expect(result.provisional_text.empty(),
         "flush promotes every provisional character to stable text");
  expect(result.key_down_probability < 0.1F,
         "soft key evidence returns near zero after a completed signal");

  // The same keying, decoded by the duration model instead. It needs a longer
  // trailing gap than the threshold does before it will commit the last
  // character, which is the latency the two techniques genuinely differ by.
  CwTimingDecoder semi_markov(
      {.keying_model = cwassistant::core::CwKeyingModel::SemiMarkov,
       .initial_wpm = 20.0});
  std::uint64_t semi_now = 0;
  const auto feed_semi = [&](const bool down, const int milliseconds) {
    const int steps = milliseconds / 2;
    for (int i = 0; i < steps; ++i) {
      semi_now += 2'000'000;
      static_cast<void>(semi_markov.process(semi_now, down ? 12.0F : 0.0F));
    }
  };
  feed_semi(false, 300);
  feed_semi(true, 60);
  feed_semi(false, 60);
  feed_semi(true, 60);
  feed_semi(false, 200);
  feed_semi(true, 60);
  feed_semi(false, 60);
  feed_semi(true, 60);
  feed_semi(false, 60);
  feed_semi(true, 60);
  feed_semi(false, 600);
  const auto semi_result = semi_markov.flush(semi_now + 500'000'000);
  expect(semi_result.text.find("IS") != std::string::npos,
         "the duration model decodes the same keying as the threshold");
  expect(semi_result.wpm > 17.0 && semi_result.wpm < 23.0,
         "the duration model reports the keyed speed");

  // Selecting a model must be the only thing that changes. A decoder left on
  // the default must behave exactly as it did before the choice existed, which
  // is what lets the threshold stay the shipped default while another
  // technique is offered beside it.
  expect(CwTimingDecoder({.initial_wpm = 20.0}).usesSegmenter() == false &&
             semi_markov.usesSegmenter(),
         "only a segmenting model builds a segmenter");
  using cwassistant::core::CwKeyingModel;
  expect(cwassistant::core::cwKeyingModelFromName(
             cwassistant::core::cwKeyingModelName(CwKeyingModel::SemiMarkov)) ==
                 CwKeyingModel::SemiMarkov &&
             cwassistant::core::cwKeyingModelFromName(
                 cwassistant::core::cwKeyingModelName(
                     CwKeyingModel::AdaptiveThreshold)) ==
                 CwKeyingModel::AdaptiveThreshold,
         "keying model names round-trip");
  expect(cwassistant::core::cwKeyingModelFromName("no-such-model") ==
             CwKeyingModel::AdaptiveThreshold,
         "an unknown stored model falls back to the shipped default");

  using cwassistant::core::CallsignPolicy;
  using cwassistant::core::CwOperatorRole;
  // A split runner signs and sends UP; the station named after TU is the one it
  // has just worked. Context alone scores TU as highly as UP, so the worked
  // station wins and the run is labelled with the wrong call. Knowing the
  // operator is hunting says the monitored stream is the runner. (The trailing
  // K matters: a call is only complete once a following word confirms it, so a
  // fixture whose last token is the callsign never labels anything.)
  {
    const std::string_view exchange = "5NN TU DL1NKB OK5OO UP K";
    const auto neutral = CallsignPolicy::best_complete_in_text(exchange);
    const auto hunting = CallsignPolicy::best_complete_in_text(
        exchange, CwOperatorRole::SearchAndPounce, "");
    expect(neutral.has_value() && *neutral == "DL1NKB",
           "without a role the station just worked outranks the runner");
    expect(hunting.has_value() && *hunting == "OK5OO",
           "searching and pouncing labels the runner, not the station it "
           "just worked");
  }
  // Running, the monitored stream is somebody answering. Context already
  // resolves this one, so the role must leave it alone rather than improve it.
  {
    const std::string_view exchange = "CQ OK5OO DL1NKB DL1NKB K";
    const auto neutral = CallsignPolicy::best_complete_in_text(exchange);
    const auto running = CallsignPolicy::best_complete_in_text(
        exchange, CwOperatorRole::Runner, "");
    expect(neutral.has_value() && *neutral == "DL1NKB" && running.has_value() &&
               *running == "DL1NKB",
           "running keeps labelling the station answering");
  }
  // The operator's own call identifies the operator. Whoever else is on the
  // frequency, it is never the name of somebody else's stream.
  {
    const auto labelled = CallsignPolicy::best_complete_in_text(
        "CQ TEST DE IU0LFQ IU0LFQ K", CwOperatorRole::SearchAndPounce,
        "IU0LFQ");
    expect(!labelled.has_value(),
           "a stream is never labelled with the operator's own callsign");
    const auto other = CallsignPolicy::best_complete_in_text(
        "CQ TEST DE OK5OO OK5OO K", CwOperatorRole::SearchAndPounce, "IU0LFQ");
    expect(other.has_value() && *other == "OK5OO",
           "excluding the operator's own call leaves other stations labelled");
  }

  using cwassistant::core::cwTextContainsDistinctiveToken;
  expect(cwTextContainsDistinctiveToken("CQ TEST DE OK5OO") &&
             cwTextContainsDistinctiveToken("R 5NN TU") &&
             cwTextContainsDistinctiveToken("UP") &&
             cwTextContainsDistinctiveToken("599"),
         "distinctive contest and calling tokens are recognised");
  // Short, common tokens are excluded on purpose: noise spells them often
  // enough that accepting them would hand the verification gate back the
  // problem its plausibility check closed.
  expect(!cwTextContainsDistinctiveToken("K DE R E T") &&
             !cwTextContainsDistinctiveToken("") &&
             !cwTextContainsDistinctiveToken("     "),
         "short common tokens are not treated as evidence");
  // Whole tokens only. A CQ inside a longer run is far more likely to be three
  // noise elements that landed together than a station calling.
  expect(!cwTextContainsDistinctiveToken("XCQY") &&
             !cwTextContainsDistinctiveToken("ACQ") &&
             !cwTextContainsDistinctiveToken("TESTING"),
         "a token embedded in a longer run is not evidence");

  CwTimingDecoder immediate_flush({.initial_wpm = 20.0});
  static_cast<void>(immediate_flush.process(0, 12.0F));
  const auto forced_up = immediate_flush.flush(2'000'000);
  expect(!forced_up.key_down && forced_up.key_down_probability == 0.0F,
         "flush forces key up even before probability smoothing naturally "
         "crosses the off threshold");

  CwTimingDecoder staged_decoder({.initial_wpm = 20.0});
  std::uint64_t staged_now = 0;
  cwassistant::core::CwDecoderUpdate staged;
  const auto staged_feed = [&](const bool down, const int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) {
      staged_now += 10'000'000;
      staged = staged_decoder.process(staged_now, down ? 12.0F : 0.0F);
    }
  };
  staged_feed(false, 100);
  staged_feed(true, 60);
  // Three dots: an unambiguous character gap. This was two and a half, which
  // is neither an element gap (one) nor a character gap (three) but between
  // them, so it asserted where the classification threshold happens to sit
  // rather than the staging behaviour it exists to cover. The replacement
  // fixture below was corrected for the same reason.
  staged_feed(false, 180);
  expect(staged.text.empty() && staged.provisional_text == "E",
         "completed character is exposed provisionally before confirmation");
  staged_feed(false, 60);
  expect(staged.text == "E" && staged.provisional_text.empty(),
         "confirmation delay promotes provisional text to append-only stable "
         "text");

  cwassistant::core::CwMultiSpeedDecoder cadence_decoder;
  std::uint64_t cadence_now = 0;
  cwassistant::core::CwDecoderUpdate cadence;
  const auto cadence_feed = [&](const bool down, const int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 5) {
      cadence_now += 5'000'000;
      cadence = cadence_decoder.process(cadence_now, down ? 12.0F : 0.0F);
    }
  };
  cadence_feed(false, 200);
  for (int word = 0; word < 3; ++word) {
    for (int element = 0; element < 3; ++element) {
      cadence_feed(true, 50);
      cadence_feed(false, element == 2 ? 150 : 50);
    }
    for (int element = 0; element < 3; ++element) {
      cadence_feed(true, 150);
      cadence_feed(false, element == 2 ? 150 : 50);
    }
  }
  expect(cadence.acoustic_wpm >= 22.0 && cadence.acoustic_wpm <= 26.0 &&
             cadence.acoustic_cadence_confidence >= 0.70F,
         "independent run-length fitting derives CW speed from 1:3 marks "
         "and 1:3 gaps without using decoded characters");
}

void test_cw_channel_bank() {
  using cwassistant::core::CwChannelBank;
  CwChannelBank bank({.minimum_verification_symbols = 0});
  std::vector<float> bins(101, -100.0F);
  std::uint64_t now = 0;
  double phase_low = 0.0;
  double phase_high = 0.0;
  constexpr double sample_rate = 8'000.0;
  const auto feed = [&](const bool low_tone, const bool high_tone,
                        const int milliseconds) {
    const int steps = milliseconds / 10;
    for (int step = 0; step < steps; ++step) {
      bins.assign(bins.size(), -100.0F);
      if (low_tone) bins[30] = -80.0F;
      if (high_tone) bins[70] = -78.0F;
      static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = now;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        const float sample =
            (low_tone ? 0.35F * static_cast<float>(std::sin(phase_low))
                      : 0.0F) +
            (high_tone ? 0.35F * static_cast<float>(std::sin(phase_high))
                       : 0.0F);
        block.samples[index] = {sample, 0.0F};
        phase_low += 2.0 * std::numbers::pi * 300.0 / sample_rate;
        phase_high += 2.0 * std::numbers::pi * 700.0 / sample_rate;
      }
      static_cast<void>(bank.processSamples(block));
      now += 10'000'000;
    }
  };

  feed(true, true, 60);
  feed(false, true, 120);
  feed(false, false, 350);
  const auto& channels = bank.channels();
  expect(channels.size() == 2,
         "full-passband channel bank retains two independent CW signals");
  if (channels.size() == 2) {
    const auto low_id = channels[0].id;
    const auto high_id = channels[1].id;
    const auto low_color = channels[0].color_index;
    const auto high_color = channels[1].color_index;
    expect(std::abs(channels[0].frequency_hz - 300.0) < 5.0 &&
               (channels[0].text + channels[0].provisional_text).find('E') !=
                   std::string::npos,
           "lower-frequency slice decodes its own dit");
    expect(std::abs(channels[1].frequency_hz - 700.0) < 5.0 &&
               (channels[1].text + channels[1].provisional_text).find('T') !=
                   std::string::npos,
           "upper-frequency slice decodes its own dah");
    expect(channels[0].color_index != channels[1].color_index,
           "simultaneous tracks receive stable distinct colors");
    expect(channels[0].active && channels[1].active,
           "short Morse word gaps retain presentation activity without "
           "flickering the stream areas");
    feed(false, false, 1'000);
    const auto& held = bank.channels();
    expect(held.size() == 2 && held[0].id == low_id && held[1].id == high_id &&
               held[0].color_index == low_color &&
               held[1].color_index == high_color && !held[0].active &&
               !held[1].active && !held[0].key_down && !held[1].key_down,
           "frequency identity survives keyed gaps without presenting a "
           "retained track as active or keyed");
    feed(false, false, 3'000);
    const auto& silent_held = bank.channels();
    expect(silent_held.size() == 2 && silent_held[0].id == low_id &&
               silent_held[1].id == high_id &&
               silent_held[0].color_index == low_color &&
               silent_held[1].color_index == high_color &&
               !silent_held[0].active && !silent_held[1].active,
           "silence cannot bypass the decoded-signal retention timeout by "
           "demoting verified tracks or keeping their carrier active");
    bank.configure({.empty_track_retention_seconds = 2.0,
                    .decoded_track_retention_seconds = 2.0,
                    .minimum_verification_symbols = 0});
    feed(false, false, 2'500);
    expect(bank.channels().empty(),
           "silent decoded tracks expire from the full-spectrum model");
    // Exercise the lease close to its promised five-minute boundary without
    // making the deterministic test wait in real time.
    now += 292'000'000'000ULL;
    feed(true, false, 100);
    const auto& reacquired = bank.channels();
    expect(reacquired.size() == 1 && reacquired.front().id != low_id &&
               reacquired.front().color_index == low_color,
           "a verified frequency reuses its color after track expiry within "
           "the five-minute identity lease");
  }

  {
    CwChannelBank nearby_bank({.minimum_separation_hz = 15.0,
                               .color_identity_tolerance_hz = 35.0,
                               .minimum_spectral_observations = 1,
                               .minimum_verification_symbols = 0,
                               .track_identity_tolerance_hz = 10.0});
    std::vector<float> nearby_bins(201, -110.0F);
    std::uint64_t nearby_now = 0;
    double first_phase = 0.0;
    double second_phase = 0.0;
    for (int step = 0; step < 20; ++step) {
      nearby_bins.assign(nearby_bins.size(), -110.0F);
      nearby_bins[60] = -55.0F;
      nearby_bins[66] = -57.0F;
      static_cast<void>(
          nearby_bank.updateSpectrum(nearby_now, 0.0, 1'000.0, nearby_bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = nearby_now;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            0.30F * static_cast<float>(std::sin(first_phase)) +
                0.30F * static_cast<float>(std::sin(second_phase)),
            0.0F};
        first_phase += 2.0 * std::numbers::pi * 300.0 / sample_rate;
        second_phase += 2.0 * std::numbers::pi * 330.0 / sample_rate;
      }
      static_cast<void>(nearby_bank.processSamples(block));
      nearby_now += 10'000'000;
    }
    const auto first_nearby = nearby_bank.channels();
    expect(first_nearby.size() == 2 &&
               first_nearby[0].id != first_nearby[1].id &&
               first_nearby[0].color_index != first_nearby[1].color_index,
           "simultaneous verified nearby identities keep separate retained "
           "observations and exclusive colors");
    if (first_nearby.size() == 2) {
      const auto first_id = first_nearby[0].id;
      const auto second_id = first_nearby[1].id;
      const auto first_color = first_nearby[0].color_index;
      const auto second_color = first_nearby[1].color_index;
      for (int refresh = 0; refresh < 8; ++refresh) {
        static_cast<void>(
            nearby_bank.updateSpectrum(nearby_now, 0.0, 1'000.0, nearby_bins));
        nearby_now += 10'000'000;
      }
      const auto& stable_nearby = nearby_bank.channels();
      expect(stable_nearby.size() == 2 && stable_nearby[0].id == first_id &&
                 stable_nearby[1].id == second_id &&
                 stable_nearby[0].color_index == first_color &&
                 stable_nearby[1].color_index == second_color,
             "nearby retained identities cannot overwrite or ping-pong "
             "during repeated presentation rebuilds");
    }
  }

  {
    CwChannelBank reservation_bank({
        .minimum_separation_hz = 45.0,
        .tracking_tolerance_hz = 70.0,
        .empty_track_retention_seconds = 10.0,
        .unverified_track_retention_seconds = 10.0,
        .minimum_spectral_observations = 1,
        .minimum_verification_symbols = 100,
        .track_identity_tolerance_hz = 50.0,
    });
    std::vector<float> reservation_bins(1'001, -110.0F);
    reservation_bins[500] = -55.0F;
    static_cast<void>(
        reservation_bank.updateSpectrum(0, 0.0, 1'000.0, reservation_bins));
    reservation_bins.assign(reservation_bins.size(), -110.0F);
    reservation_bins[460] = -55.0F;
    reservation_bins[540] = -56.0F;
    static_cast<void>(reservation_bank.updateSpectrum(10'000'000, 0.0, 1'000.0,
                                                      reservation_bins));
    const auto diagnostics = reservation_bank.allTrackDiagnostics();
    const auto matched =
        std::count_if(diagnostics.cbegin(), diagnostics.cend(),
                      [](const auto& track) { return track.matched; });
    expect(diagnostics.size() == 1 && matched == 1,
           "changing sidelobes inside one identity cell cannot clone an "
           "automatic carrier track");
  }

  for (const bool keep_unverified : {false, true}) {
    CwChannelBank alternating_bank({
        .minimum_separation_hz = 45.0,
        .empty_track_retention_seconds = 10.0,
        .decoded_track_retention_seconds = 10.0,
        .unverified_track_retention_seconds = 10.0,
        .minimum_spectral_observations = 1,
        .minimum_verification_symbols =
            static_cast<std::uint16_t>(keep_unverified ? 100 : 0),
        .minimum_key_transitions = 0,
        .minimum_cadence_observations = 0,
        .minimum_verification_timing_quality = 0.0F,
        .minimum_verification_cadence_quality = 0.0F,
        .minimum_character_confidence = 0.0F,
        .minimum_narrowband_coherence = 0.0F,
        .maximum_verification_unknown_fraction = 1.0F,
        .track_identity_tolerance_hz = 35.0,
        .verification_enter_seconds = 0.0,
    });
    constexpr double first_hz = 500.0;
    constexpr double neighbor_hz = 585.0;
    std::vector<float> alternating_bins(1'001, -110.0F);
    std::uint64_t alternating_now = 0;
    double first_phase = 0.0;
    double neighbor_phase = 0.0;
    const auto step = [&](const int keyed_carrier) {
      alternating_bins.assign(alternating_bins.size(), -110.0F);
      if (keyed_carrier == 0) alternating_bins[500] = -62.0F;
      if (keyed_carrier == 1) alternating_bins[585] = -58.0F;
      static_cast<void>(alternating_bank.updateSpectrum(
          alternating_now, 0.0, 1'000.0, alternating_bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = alternating_now;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        float sample = 0.0F;
        if (keyed_carrier == 0)
          sample = 0.32F * static_cast<float>(std::sin(first_phase));
        if (keyed_carrier == 1)
          sample = 0.42F * static_cast<float>(std::sin(neighbor_phase));
        block.samples[index] = {sample, 0.0F};
        first_phase += 2.0 * std::numbers::pi * first_hz / sample_rate;
        neighbor_phase += 2.0 * std::numbers::pi * neighbor_hz / sample_rate;
      }
      static_cast<void>(alternating_bank.processSamples(block));
      alternating_now += 10'000'000;
    };
    const auto feed = [&](const int keyed_carrier, const int milliseconds) {
      for (int elapsed = 0; elapsed < milliseconds; elapsed += 10)
        step(keyed_carrier);
    };
    const auto dits = [&](const int carrier, const int count) {
      for (int index = 0; index < count; ++index) {
        feed(carrier, 60);
        feed(-1, 180);
      }
    };

    dits(0, 12);
    step(0);
    auto diagnostics = alternating_bank.allTrackDiagnostics();
    auto first = std::find_if(
        diagnostics.begin(), diagnostics.end(), [](const auto& track) {
          return std::abs(track.frequency_hz - first_hz) < 10.0;
        });
    expect(first != diagnostics.end(),
           "alternating-tone fixture acquires the first carrier");
    if (first == diagnostics.end()) continue;
    const std::uint64_t first_id = first->id;
    const std::uint32_t symbols_before_gap = first->decoded_symbols;
    if (keep_unverified) {
      expect(first->verification_state ==
                 cwassistant::core::CwTrackState::MorseLikely,
             "unverified alternating-tone fixture reaches Morse-likely but "
             "stays private");
      alternating_bins.assign(alternating_bins.size(), -110.0F);
      alternating_bins[500] = -62.0F;
      alternating_bins[530] = -48.0F;
      static_cast<void>(alternating_bank.updateSpectrum(
          alternating_now, 0.0, 1'000.0, alternating_bins));
      const auto skirt_diagnostics = alternating_bank.allTrackDiagnostics();
      const auto protected_first = std::find_if(
          skirt_diagnostics.cbegin(), skirt_diagnostics.cend(),
          [first_id](const auto& track) { return track.id == first_id; });
      expect(protected_first != skirt_diagnostics.cend() &&
                 protected_first->matched &&
                 std::abs(protected_first->frequency_hz - first_hz) < 10.0,
             "a Morse-likely track reserves its carrier ahead of a stronger "
             "within-cell skirt during model acquisition");
    } else {
      expect(first->verification_state ==
                 cwassistant::core::CwTrackState::Verified,
             "verified alternating-tone fixture reaches publication");
    }

    feed(-1, 600);
    dits(0, 3);
    diagnostics = alternating_bank.allTrackDiagnostics();
    first = std::find_if(
        diagnostics.begin(), diagnostics.end(),
        [first_id](const auto& track) { return track.id == first_id; });
    expect(first != diagnostics.end() &&
               first->decoded_symbols > symbols_before_gap,
           "a same-frequency sender resumes through a normal 600 ms word "
           "gap without freezing or replacing its decoder");

    dits(1, 10);
    diagnostics = alternating_bank.allTrackDiagnostics();
    first = std::find_if(
        diagnostics.begin(), diagnostics.end(),
        [first_id](const auto& track) { return track.id == first_id; });
    expect(first != diagnostics.end() && !first->key_down,
           "an unmatched adjacent carrier forces the old decoder key up");
    if (first == diagnostics.end()) continue;
    const std::string frozen_text = first->text;
    const std::string frozen_provisional = first->provisional_text;
    const std::uint32_t frozen_symbols = first->decoded_symbols;
    const std::uint32_t frozen_transitions = first->key_transitions;

    dits(1, 14);
    diagnostics = alternating_bank.allTrackDiagnostics();
    first = std::find_if(
        diagnostics.begin(), diagnostics.end(),
        [first_id](const auto& track) { return track.id == first_id; });
    expect(first != diagnostics.end() && first->text == frozen_text &&
               first->provisional_text == frozen_provisional &&
               first->decoded_symbols == frozen_symbols &&
               first->key_transitions == frozen_transitions && !first->key_down,
           "verified and unverified tracks freeze all decoder output after "
           "the unmatched gap hold despite a stronger 85 Hz neighbor");

    if (keep_unverified && first != diagnostics.end()) {
      expect(!alternating_bank.acceptCharacterRefinement(first_id, "NOISE",
                                                         alternating_now),
             "local character evidence without a complete callsign cannot "
             "confirm a stream");
      expect(alternating_bank.acceptCharacterRefinement(
                 first_id, "CQ DE 4X5LL ", alternating_now),
             "overlap-confirmed local callsign evidence is accepted for an "
             "already Morse-likely acoustic stream");
      expect(!alternating_bank.acceptCharacterRefinement(
                 first_id, "CQ DE 4X5LL ", alternating_now),
             "retained model text cannot refresh the same acoustic evidence "
             "timestamp");
      expect(!alternating_bank.acceptCharacterRefinement(
                 first_id, "CQ DE 4X5LL ", alternating_now + 2'000'000'000ULL),
             "future model evidence cannot advance verification state");
      feed(0, 700);
      diagnostics = alternating_bank.allTrackDiagnostics();
      first = std::find_if(
          diagnostics.begin(), diagnostics.end(),
          [first_id](const auto& track) { return track.id == first_id; });
      expect(first != diagnostics.end() &&
                 first->verification_state ==
                     cwassistant::core::CwTrackState::Verified,
             "local character evidence confirms only after the ordinary "
             "sustained acoustic entry interval");
    }
  }

  {
    // A label whose opening characters name no allocated country must never
    // reach the operator. QK7SS keys exactly as cleanly as DK7SS -- the
    // acoustic evidence is identical and the decode is confident -- but no
    // administration holds a Q prefix, so it cannot be a station. This is the
    // common shape of a wrong label: one missed or added element turns a real
    // prefix into an impossible one, and nothing downstream of the decoder can
    // tell the difference without knowing what prefixes exist.
    //
    // The transcript is untouched. Only the stream's name is refused, so an
    // operator still reads what was copied and can see for themselves.
    const auto label_for = [](const std::string_view call_elements_owner)
        -> std::pair<std::string, std::string> {
      CwChannelBank bank({.empty_track_retention_seconds = 0.5,
                          .decoded_track_retention_seconds = 10.0,
                          .detector_frame_interval_seconds = 0.0,
                          .minimum_spectral_observations = 1,
                          .minimum_verification_symbols = 0,
                          .verification_enter_seconds = 0.0,
                          .verification_exit_seconds = 0.0});
      std::vector<float> bins(201, -110.0F);
      std::uint64_t now = 0;
      double phase = 0.0;
      const auto step = [&](const bool keyed) {
        bins.assign(bins.size(), -110.0F);
        if (keyed) bins[60] = -55.0F;
        static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));
        cwassistant::core::RealtimeSampleBlock block;
        block.stream.sample_rate_hz = sample_rate;
        block.timestamp_ns = now;
        block.sample_count = 80;
        for (std::size_t index = 0; index < block.sample_count; ++index) {
          block.samples[index] = {
              keyed ? 0.40F * static_cast<float>(std::sin(phase)) : 0.0F,
              0.0F};
          phase += 2.0 * std::numbers::pi * 300.0 / sample_rate;
        }
        static_cast<void>(bank.processSamples(block));
        now += 10'000'000;
      };
      const auto gap = [&](const int steps) {
        for (int index = 0; index < steps; ++index) step(false);
      };
      const auto character = [&](const std::string_view elements) {
        for (std::size_t index = 0; index < elements.size(); ++index) {
          for (int repeat = 0; repeat < (elements[index] == '.' ? 6 : 18);
               ++repeat) {
            step(true);
          }
          if (index + 1U < elements.size()) gap(6);
        }
        gap(18);
      };
      const auto word_gap = [&] { gap(24); };
      character("-.-.");  // C
      character("--.-");  // Q
      word_gap();
      character("-..");  // D
      character(".");    // E
      word_gap();
      // The callsign under test, then a closing token so it is complete.
      character(call_elements_owner == std::string_view{"D"} ? "-.."   // D
                                                             : "--.-");  // Q
      character("-.-");    // K
      character("--...");  // 7
      character("...");    // S
      character("...");    // S
      word_gap();
      character(".");
      const auto channels = bank.channels();
      if (channels.empty()) return {std::string{}, std::string{}};
      return {channels.front().callsign, channels.front().text};
    };
    const auto [allocated_label, allocated_text] = label_for("D");
    const auto [impossible_label, impossible_text] = label_for("Q");
    expect(allocated_label == "DK7SS",
           "an allocated prefix still labels the stream");
    expect(impossible_label.empty(),
           "a callsign whose prefix names no country never labels a stream");
    expect(impossible_text.find("QK7SS") != std::string::npos,
           "refusing the label leaves the decoded transcript untouched");
  }

  {
    CwChannelBank replacement_bank({.empty_track_retention_seconds = 0.5,
                                    .decoded_track_retention_seconds = 10.0,
                                    // Drives fabricated spectra faster than the
                                    // detector cadence and asserts the exact
                                    // replacement/inheritance lifecycle, so
                                    // cadence decimation is disabled here.
                                    .detector_frame_interval_seconds = 0.0,
                                    .minimum_spectral_observations = 1,
                                    .minimum_verification_symbols = 0,
                                    .verification_enter_seconds = 0.0,
                                    .verification_exit_seconds = 0.0});
    std::vector<float> replacement_bins(201, -110.0F);
    std::uint64_t replacement_now = 0;
    double replacement_phase = 0.0;
    const auto replacement_step = [&](const bool keyed) {
      replacement_bins.assign(replacement_bins.size(), -110.0F);
      if (keyed) replacement_bins[60] = -55.0F;
      static_cast<void>(replacement_bank.updateSpectrum(
          replacement_now, 0.0, 1'000.0, replacement_bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = replacement_now;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            keyed ? 0.40F * static_cast<float>(std::sin(replacement_phase))
                  : 0.0F,
            0.0F};
        replacement_phase += 2.0 * std::numbers::pi * 300.0 / sample_rate;
      }
      static_cast<void>(replacement_bank.processSamples(block));
      replacement_now += 10'000'000;
    };
    const auto replacement_mark = [&](const int steps) {
      for (int step = 0; step < steps; ++step) replacement_step(true);
    };
    const auto replacement_gap = [&](const int steps) {
      for (int step = 0; step < steps; ++step) replacement_step(false);
    };
    const auto replacement_character = [&](const std::string_view elements) {
      for (std::size_t index = 0; index < elements.size(); ++index) {
        replacement_mark(elements[index] == '.' ? 6 : 18);
        if (index + 1U < elements.size()) replacement_gap(6);
      }
      // Standard Morse spacing: three dots between characters. This fixture
      // previously used five, which is neither a character gap (three) nor a
      // word gap (seven) but exactly between them, so whether it produced a
      // word space depended on the decoder's element-length estimate being
      // wrong by a specific amount. The fixture exists to exercise the
      // replacement/inheritance lifecycle, not gap classification, so it now
      // sends unambiguous spacing.
      replacement_gap(18);
    };
    // Extend the preceding three-dot character gap to a seven-dot word gap.
    const auto replacement_word_gap = [&] { replacement_gap(24); };
    // Establish an acoustic callsign on the predecessor so the replacement
    // lifecycle test can distinguish transcript continuity from station-name
    // continuity. A new tracker may inherit readable session history, but it
    // must earn its own callsign from its own acoustic suffix.
    replacement_character("-.-.");  // C
    replacement_character("--.-");  // Q
    replacement_word_gap();
    replacement_character("-..");  // D
    replacement_character(".");    // E
    replacement_word_gap();
    replacement_character("-..");    // D
    replacement_character("-.-");    // K
    replacement_character("--...");  // 7
    replacement_character("...");    // S
    replacement_character("...");    // S
    replacement_word_gap();
    replacement_character(".");  // Close the preceding callsign token.
    const auto predecessor = replacement_bank.channels();
    expect(predecessor.size() == 1 && !predecessor.front().text.empty(),
           "replacement fixture starts with stable predecessor text");
    if (predecessor.size() == 1 && !predecessor.front().text.empty()) {
      const auto predecessor_id = predecessor.front().id;
      const auto predecessor_color = predecessor.front().color_index;
      const std::string predecessor_text = predecessor.front().text;
      expect(predecessor.front().callsign == "DK7SS",
             "replacement fixture confirms a predecessor callsign");

      replacement_bank.configure({.empty_track_retention_seconds = 0.5,
                                  .decoded_track_retention_seconds = 10.0,
                                  .detector_frame_interval_seconds = 0.0,
                                  .minimum_spectral_observations = 1,
                                  .minimum_verification_symbols = 20,
                                  .verification_enter_seconds = 0.0,
                                  .verification_exit_seconds = 0.0});
      replacement_step(true);
      replacement_now += 1'000'000'000;
      replacement_bins.assign(replacement_bins.size(), -110.0F);
      static_cast<void>(replacement_bank.updateSpectrum(
          replacement_now, 0.0, 1'000.0, replacement_bins));

      replacement_bank.configure({.empty_track_retention_seconds = 0.5,
                                  .decoded_track_retention_seconds = 10.0,
                                  .detector_frame_interval_seconds = 0.0,
                                  .minimum_spectral_observations = 1,
                                  .minimum_verification_symbols = 0,
                                  .verification_enter_seconds = 0.0,
                                  .verification_exit_seconds = 0.0});
      replacement_step(true);
      const auto& replacement = replacement_bank.channels();
      expect(replacement.size() == 1 &&
                 replacement.front().id != predecessor_id &&
                 replacement.front().color_index == predecessor_color &&
                 replacement.front().text == predecessor_text &&
                 replacement.front().callsign.empty(),
             "genuine replacement inherits its predecessor text exactly "
             "once and reuses the identity color without inheriting its "
             "callsign");
      replacement_step(true);
      expect(replacement_bank.channels().size() == 1 &&
                 replacement_bank.channels().front().text == predecessor_text &&
                 replacement_bank.channels().front().callsign.empty(),
             "refreshing a replacement cannot append its inherited prefix "
             "again or restore a predecessor callsign");
    }
  }

  {
    CwChannelBank admission_bank(
        {.maximum_tracks = 2, .minimum_spectral_observations = 50});
    std::vector<float> admission_bins(1'001, -110.0F);
    admission_bins[200] = -76.0F;
    admission_bins[400] = -74.0F;
    static_cast<void>(
        admission_bank.updateSpectrum(0, 0.0, 1'000.0, admission_bins));
    expect(admission_bank.allTrackDiagnostics().size() == 2,
           "track bank reaches its configured candidate capacity");
    admission_bins[800] = -45.0F;
    static_cast<void>(admission_bank.updateSpectrum(20'000'000, 0.0, 1'000.0,
                                                    admission_bins));
    const auto admitted = admission_bank.allTrackDiagnostics();
    const bool admitted_strong_new_peak =
        std::any_of(admitted.begin(), admitted.end(), [](const auto& track) {
          return std::abs(track.frequency_hz - 800.0) < 2.0;
        });
    expect(admitted.size() == 2 && admitted_strong_new_peak,
           "a saturated track bank replaces weak unverified occupancy with a "
           "stronger new carrier");
  }

  {
    CwChannelBank identity_bank({.minimum_spectral_observations = 3,
                                 .track_identity_tolerance_hz = 35.0});
    std::vector<float> identity_bins(1'001, -110.0F);
    for (std::uint64_t frame = 0; frame < 3; ++frame) {
      identity_bins.assign(identity_bins.size(), -110.0F);
      identity_bins[250] = -65.0F;
      static_cast<void>(identity_bank.updateSpectrum(frame * 20'000'000, 0.0,
                                                     1'000.0, identity_bins));
    }
    const auto original_tracks = identity_bank.allTrackDiagnostics();
    const auto original_id = original_tracks.front().id;
    identity_bins.assign(identity_bins.size(), -110.0F);
    identity_bins[300] = -55.0F;
    static_cast<void>(
        identity_bank.updateSpectrum(80'000'000, 0.0, 1'000.0, identity_bins));
    const auto separated_tracks = identity_bank.allTrackDiagnostics();
    const auto new_signal =
        std::min_element(separated_tracks.begin(), separated_tracks.end(),
                         [](const auto& left, const auto& right) {
                           return std::abs(left.frequency_hz - 300.0) <
                                  std::abs(right.frequency_hz - 300.0);
                         });
    expect(separated_tracks.size() == 2 &&
               new_signal != separated_tracks.end() &&
               new_signal->id != original_id,
           "an established track cannot carry decoder history across an "
           "identity-breaking frequency jump");
  }

  CwChannelBank rejection_bank;
  bins.assign(bins.size(), -100.0F);
  bins[70] = -75.0F;
  static_cast<void>(rejection_bank.updateSpectrum(0, 0.0, 1'000.0, bins));
  double interference_phase = 0.0;
  for (int step = 0; step < 30; ++step) {
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = static_cast<std::uint64_t>(step) * 10'000'000;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          0.45F * static_cast<float>(std::sin(interference_phase)), 0.0F};
      interference_phase += 2.0 * std::numbers::pi * 880.0 / sample_rate;
    }
    static_cast<void>(rejection_bank.processSamples(block));
  }
  expect(rejection_bank.channels().empty(),
         "an adjacent non-tracked carrier is never published as verified CW");

  CwChannelBank shaped_noise_bank;
  std::uint32_t noise_state = 0x13579BDFU;
  double noise_time = 0.0;
  for (int step = 0; step < 500; ++step) {
    for (std::size_t bin = 0; bin < bins.size(); ++bin) {
      bins[bin] = -88.0F +
                  8.0F * static_cast<float>(std::sin(0.08 * bin + noise_time)) +
                  2.0F * static_cast<float>(std::sin(0.91 * bin - noise_time));
    }
    const auto timestamp = static_cast<std::uint64_t>(step) * 10'000'000;
    static_cast<void>(
        shaped_noise_bank.updateSpectrum(timestamp, 0.0, 1'000.0, bins));
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = timestamp;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      noise_state = noise_state * 1'664'525U + 1'013'904'223U;
      const float noise =
          static_cast<float>((noise_state >> 8U) & 0xFFFFU) / 32'767.5F - 1.0F;
      block.samples[index] = {0.18F * noise, 0.0F};
    }
    static_cast<void>(shaped_noise_bank.processSamples(block));
    noise_time += 0.03;
  }
  expect(shaped_noise_bank.channels().empty(),
         "five seconds of shaped broadband noise publishes no CW traces");

  CwChannelBank drift_bank({.minimum_verification_symbols = 0});
  std::vector<float> fine_bins(1'001, -110.0F);
  double drifting_phase = 0.0;
  constexpr double initial_tone_hz = 500.0;
  constexpr double requested_drift_hz_per_second = 40.0;
  for (int step = 0; step < 120; ++step) {
    const double elapsed = static_cast<double>(step) * 0.01;
    const double tone_hz =
        initial_tone_hz + requested_drift_hz_per_second * elapsed;
    fine_bins.assign(fine_bins.size(), -110.0F);
    fine_bins[static_cast<std::size_t>(std::llround(tone_hz))] = -68.0F;
    const auto timestamp = static_cast<std::uint64_t>(step) * 10'000'000;
    static_cast<void>(
        drift_bank.updateSpectrum(timestamp, 0.0, 1'000.0, fine_bins));
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = timestamp;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          0.32F * static_cast<float>(std::sin(drifting_phase)), 0.0F};
      drifting_phase += 2.0 * std::numbers::pi * tone_hz / sample_rate;
    }
    static_cast<void>(drift_bank.processSamples(block));
  }
  expect(drift_bank.channels().size() == 1,
         "a steadily drifting signal retains one channel identity");
  if (!drift_bank.channels().empty()) {
    const auto& drifting = drift_bank.channels().front();
    expect(std::abs(drifting.frequency_hz - 547.6) < 4.0,
           "sub-bin tracker follows the current drifting tone frequency");
    expect(std::abs(drifting.presentation_frequency_hz - initial_tone_hz) < 2.0,
           "operator marker remains anchored while internal tracking follows "
           "bounded oscillator drift");
    expect(drifting.drift_hz_per_second > 20.0 &&
               drifting.drift_hz_per_second < 65.0,
           "frequency tracker reports a bounded tone drift estimate");
    expect(drifting.filter_width_hz == 240.0,
           "automatic narrowband selection widens for a fast drifting tone");
  }

  CwChannelBank slow_bank;
  double slow_phase = 0.0;
  std::vector<bool> slow_keying;
  const auto append_units = [&slow_keying](const bool keyed, const int units) {
    slow_keying.insert(slow_keying.end(), units * 10, keyed);
  };
  const auto append_letter = [&append_units](const std::string_view elements) {
    for (std::size_t index = 0; index < elements.size(); ++index) {
      append_units(true, elements[index] == '.' ? 1 : 3);
      append_units(false, index + 1 == elements.size() ? 3 : 1);
    }
  };
  append_letter("...");
  append_letter("---");
  append_letter("...");
  append_units(false, 4);  // Complete the seven-unit word gap.
  const int slow_steps = static_cast<int>(slow_keying.size()) * 5;
  for (int step = 0; step < slow_steps; ++step) {
    const bool keyed =
        slow_keying[static_cast<std::size_t>(step) % slow_keying.size()];
    fine_bins.assign(fine_bins.size(), -110.0F);
    if (keyed) fine_bins[400] = -68.0F;
    const auto timestamp = static_cast<std::uint64_t>(step) * 10'000'000;
    static_cast<void>(
        slow_bank.updateSpectrum(timestamp, 0.0, 1'000.0, fine_bins));
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = timestamp;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          keyed ? 0.25F * static_cast<float>(std::sin(slow_phase)) : 0.0F,
          0.0F};
      slow_phase += 2.0 * std::numbers::pi * 400.0 / sample_rate;
    }
    static_cast<void>(slow_bank.processSamples(block));
  }
  expect(!slow_bank.channels().empty(),
         "clean slow keyed signal retains a tracked channel");
  if (!slow_bank.channels().empty()) {
    const auto& verified = slow_bank.channels().front();
    expect(verified.filter_width_hz == 60.0,
           "automatic narrowband selection narrows a clean slow signal");
    expect(verified.verification_state ==
                   cwassistant::core::CwTrackState::Verified &&
               verified.verification_confidence >= 0.55F &&
               verified.verification_cadence_quality >= 0.45F &&
               verified.verification_timing_quality >= 0.55F &&
               verified.verification_character_confidence >= 0.55F &&
               verified.key_transitions >= 6,
           "published CW exposes the evidence that verified its cadence");
    expect(verified.characters.size() >= 3 && verified.characters.back().known,
           "stable decoded characters retain bounded per-character evidence");
    expect(std::abs(verified.verification_timing_quality -
                    verified.verification_character_confidence) > 0.01F,
           "timing quality and character confidence are genuinely "
           "independent signals, not the same value reported twice");
  }
  const auto slow_diagnostics = slow_bank.verificationDiagnostics();
  expect(slow_diagnostics.verified_tracks == 1 &&
             slow_diagnostics.verified_transitions == 1,
         "verification diagnostics report the candidate lifecycle transition");
  const auto slow_track_diagnostics = slow_bank.allTrackDiagnostics();
  expect(slow_track_diagnostics.size() == 1 &&
             slow_track_diagnostics.front().verification_state ==
                 cwassistant::core::CwTrackState::Verified &&
             !slow_track_diagnostics.front().text.empty(),
         "per-track diagnostics for operator-consented debug capture expose "
         "full private state including decoded text");

  // A VFO retune (a known, deliberate audio-domain shift) must preserve the
  // track's identity and decoded history, unlike an unexplained jump that
  // exceeds normal tracking tolerance and would be treated as a lost track.
  // Stop here rather than dereference what the expectations above just failed
  // on. Every check from this point reads front() of these containers, so a
  // decoder that produced no track turns a clear list of failures into a
  // crash, and the crash is what gets reported instead of the cause.
  if (slow_track_diagnostics.empty() || slow_bank.channels().empty()) {
    expect(false,
           "slow-track checks need a tracked channel; skipping the rest");
    return;
  }
  const auto text_before_shift = slow_track_diagnostics.front().text;
  const auto frequency_before_shift = slow_bank.channels().front().frequency_hz;
  slow_bank.shiftTrackedFrequencies(300.0);
  expect(!slow_bank.channels().empty() &&
             std::abs(slow_bank.channels().front().frequency_hz -
                      (frequency_before_shift + 300.0)) < 0.01 &&
             std::abs(slow_bank.channels().front().presentation_frequency_hz -
                      700.0) < 2.0 &&
             slow_bank.channels().front().verification_state ==
                 cwassistant::core::CwTrackState::Verified &&
             slow_bank.channels().front().text == text_before_shift,
         "shiftTrackedFrequencies re-centers a track by exactly the given "
         "delta while preserving its verification state and decoded text");
  for (int step = 0; step < slow_steps; ++step) {
    const bool keyed =
        slow_keying[static_cast<std::size_t>(step) % slow_keying.size()];
    fine_bins.assign(fine_bins.size(), -110.0F);
    if (keyed) fine_bins[700] = -68.0F;
    const auto timestamp =
        static_cast<std::uint64_t>(slow_steps + step) * 10'000'000;
    static_cast<void>(
        slow_bank.updateSpectrum(timestamp, 0.0, 1'000.0, fine_bins));
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = timestamp;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          keyed ? 0.25F * static_cast<float>(std::sin(slow_phase)) : 0.0F,
          0.0F};
      slow_phase += 2.0 * std::numbers::pi * 700.0 / sample_rate;
    }
    static_cast<void>(slow_bank.processSamples(block));
  }
  expect(
      slow_bank.channels().size() == 1 &&
          slow_bank.channels().front().verification_state ==
              cwassistant::core::CwTrackState::Verified &&
          slow_bank.channels().front().text.size() > text_before_shift.size(),
      "decoding continues on the same track identity at the shifted "
      "frequency, growing its text, rather than starting a new track");

  // A large shift (an operator tuning across the band, not centering on one
  // station -- or several small shifts accumulating the same way) can carry
  // a track's audio frequency past 0 Hz, where it no longer corresponds to
  // anything real. It must be dropped outright rather than left behind as a
  // nonsensical negative-frequency candidate. Uses its own bank so the
  // retention test just below still has slow_bank's live verified track.
  {
    CwChannelBank drop_bank;
    std::vector<float> drop_bins(1'001, -110.0F);
    double drop_phase = 0.0;
    std::vector<bool> drop_keying;
    const auto append_drop_units = [&drop_keying](const bool keyed,
                                                  const int units) {
      drop_keying.insert(drop_keying.end(), units * 10, keyed);
    };
    const auto append_drop_letter =
        [&append_drop_units](const std::string_view elements) {
          for (std::size_t index = 0; index < elements.size(); ++index) {
            append_drop_units(true, elements[index] == '.' ? 1 : 3);
            append_drop_units(false, index + 1 == elements.size() ? 3 : 1);
          }
        };
    append_drop_letter("...");
    append_drop_letter("---");
    append_drop_letter("...");
    append_drop_units(false, 4);
    const int drop_steps = static_cast<int>(drop_keying.size()) * 5;
    for (int step = 0; step < drop_steps; ++step) {
      const bool keyed =
          drop_keying[static_cast<std::size_t>(step) % drop_keying.size()];
      drop_bins.assign(drop_bins.size(), -110.0F);
      if (keyed) drop_bins[500] = -68.0F;
      const auto timestamp = static_cast<std::uint64_t>(step) * 10'000'000;
      static_cast<void>(
          drop_bank.updateSpectrum(timestamp, 0.0, 1'000.0, drop_bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = timestamp;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            keyed ? 0.25F * static_cast<float>(std::sin(drop_phase)) : 0.0F,
            0.0F};
        drop_phase += 2.0 * std::numbers::pi * 500.0 / sample_rate;
      }
      static_cast<void>(drop_bank.processSamples(block));
    }
    expect(!drop_bank.channels().empty(),
           "the drop-test scenario actually creates a track before "
           "exercising the shift, so the check below is not vacuous");
    drop_bank.shiftTrackedFrequencies(-100'000.0);
    expect(drop_bank.channels().empty(),
           "a shift that would carry a track past 0 Hz drops it instead of "
           "leaving a negative-frequency candidate behind");
  }

  // Tuning a station out of the passband and back must not cost its identity.
  //
  // A signal that leaves the processed band because the operator turned the
  // dial has not been lost: its position is known exactly, and turning back
  // puts it at a computable place. It used to expire on the ordinary retention
  // timeout -- that was deliberate, and wrong -- so the identity, the
  // transcript and the audio monitor of the very station being tuned around
  // were destroyed, and it returned as a new unrecognised track. This is the
  // operator's report "I still lose the CW stream if I move the VFO".
  {
    CwChannelBank park_bank;
    std::vector<float> park_bins(1'001, -110.0F);
    double park_phase = 0.0;
    std::vector<bool> park_keying;
    const auto append_park_units = [&park_keying](const bool keyed,
                                                  const int units) {
      park_keying.insert(park_keying.end(), units * 10, keyed);
    };
    const auto append_park_letter =
        [&append_park_units](const std::string_view elements) {
          for (std::size_t index = 0; index < elements.size(); ++index) {
            append_park_units(true, elements[index] == '.' ? 1 : 3);
            append_park_units(false, index + 1 == elements.size() ? 3 : 1);
          }
        };
    append_park_letter("...");
    append_park_letter("---");
    append_park_letter("...");
    append_park_units(false, 4);
    const int park_steps = static_cast<int>(park_keying.size()) * 5;
    std::uint64_t last_ns = 0;
    for (int step = 0; step < park_steps; ++step) {
      const bool keyed =
          park_keying[static_cast<std::size_t>(step) % park_keying.size()];
      park_bins.assign(park_bins.size(), -110.0F);
      if (keyed) park_bins[500] = -68.0F;
      last_ns = static_cast<std::uint64_t>(step) * 10'000'000;
      static_cast<void>(
          park_bank.updateSpectrum(last_ns, 0.0, 1'000.0, park_bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = last_ns;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            keyed ? 0.25F * static_cast<float>(std::sin(park_phase)) : 0.0F,
            0.0F};
        park_phase += 2.0 * std::numbers::pi * 500.0 / sample_rate;
      }
      static_cast<void>(park_bank.processSamples(block));
    }
    expect(park_bank.channels().size() == 1,
           "the park scenario creates exactly one track before the retune, so "
           "the identity check below is not vacuous");
    const auto parked_id = park_bank.channels().front().id;

    // Tune away: +40 kHz carries the 500 Hz track far outside the 0-1 kHz
    // processed band, the case that used to be left to expire.
    park_bank.shiftTrackedFrequencies(40'000.0);
    expect(park_bank.channels().size() == 1,
           "a retune that carries a track out of the passband parks it rather "
           "than dropping it: the operator turned the dial, the station is "
           "not lost");

    // Hold there, silent, well past the ordinary retention timeout. This is
    // the step that failed before: the track expired while parked.
    //
    // 4'500 steps of 10 ms is 45 seconds -- comfortably beyond the 30-second
    // decoded-track retention that used to end it, and comfortably inside the
    // 180-second parked retention. An earlier version of this test used 400
    // steps, four seconds, which neither retention would have ended, so it
    // passed with the fix reverted and proved nothing.
    for (int step = 1; step <= 4'500; ++step) {
      park_bins.assign(park_bins.size(), -110.0F);
      const std::uint64_t timestamp = last_ns +
          static_cast<std::uint64_t>(step) * 10'000'000;
      static_cast<void>(
          park_bank.updateSpectrum(timestamp, 0.0, 1'000.0, park_bins));
    }
    // While parked the track is outside the processed band, so it is not
    // published -- its marker would be off-screen regardless. What must
    // survive is the identity, so that tuning back returns the same station
    // rather than a new card.
    park_bank.shiftTrackedFrequencies(-40'000.0);
    expect(park_bank.channels().size() == 1 &&
               park_bank.channels().front().id == parked_id,
           "tuning a station out of the passband and back after far longer "
           "than the ordinary retention timeout returns the SAME track, so "
           "its transcript, colour and audio monitor follow the station "
           "instead of a new card appearing for it");
  }

  // The same must hold when the analysed band itself moves, which is what a
  // receiver retune looks like on direct IQ: the decoder window is re-centred
  // on the new capture, so the slice being decoded changes while the stations
  // in it keep their absolute frequencies. This is the operator's report "if I
  // move the RF spectrum I still lose the tracks". No shiftTrackedFrequencies
  // call is involved here at all -- only new spectrum bounds.
  {
    CwChannelBank band_bank;
    std::vector<float> band_bins(1'001, -110.0F);
    double band_phase = 0.0;
    std::vector<bool> band_keying;
    const auto append_band_units = [&band_keying](const bool keyed,
                                                  const int units) {
      band_keying.insert(band_keying.end(), units * 10, keyed);
    };
    const auto append_band_letter =
        [&append_band_units](const std::string_view elements) {
          for (std::size_t index = 0; index < elements.size(); ++index) {
            append_band_units(true, elements[index] == '.' ? 1 : 3);
            append_band_units(false, index + 1 == elements.size() ? 3 : 1);
          }
        };
    append_band_letter("...");
    append_band_letter("---");
    append_band_letter("...");
    append_band_units(false, 4);
    const int band_steps = static_cast<int>(band_keying.size()) * 5;
    std::uint64_t band_ns = 0;
    // Acquire at 500 Hz inside a 0-1000 Hz analysed window. The absolute
    // numbers are the audio scale rather than RF because the bank correlates
    // the spectral peak with the tone actually present in the samples; what is
    // under test is the window moving, which is the same code path either way.
    for (int step = 0; step < band_steps; ++step) {
      const bool keyed =
          band_keying[static_cast<std::size_t>(step) % band_keying.size()];
      band_bins.assign(band_bins.size(), -110.0F);
      if (keyed) band_bins[500] = -68.0F;
      band_ns = static_cast<std::uint64_t>(step) * 10'000'000;
      static_cast<void>(
          band_bank.updateSpectrum(band_ns, 0.0, 1'000.0, band_bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = band_ns;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            keyed ? 0.25F * static_cast<float>(std::sin(band_phase)) : 0.0F,
            0.0F};
        band_phase += 2.0 * std::numbers::pi * 500.0 / sample_rate;
      }
      static_cast<void>(band_bank.processSamples(block));
    }
    expect(band_bank.channels().size() == 1,
           "the retune scenario creates exactly one track before the window "
           "moves, so the identity check below is not vacuous");
    const auto retuned_id = band_bank.channels().front().id;

    // Retune: the analysed window moves entirely away from the station.
    for (int step = 1; step <= 4'500; ++step) {
      band_bins.assign(band_bins.size(), -110.0F);
      const std::uint64_t timestamp =
          band_ns + static_cast<std::uint64_t>(step) * 10'000'000;
      static_cast<void>(band_bank.updateSpectrum(timestamp, 40'000.0,
                                                 41'000.0, band_bins));
    }

    // Back again. The station is where it always was.
    const std::uint64_t return_ns = band_ns + 4'600ULL * 10'000'000ULL;
    static_cast<void>(
        band_bank.updateSpectrum(return_ns, 0.0, 1'000.0, band_bins));
    expect(band_bank.channels().size() == 1 &&
               band_bank.channels().front().id == retuned_id,
           "moving the analysed band away from a station and back returns the "
           "SAME track: a receiver retune does not move the station, so its "
           "identity, transcript and audio monitor must survive it");
  }

  slow_bank.configure({.empty_track_retention_seconds = 2.0,
                       .decoded_track_retention_seconds = 2.0});
  for (int silence_step = 0; silence_step < 250; ++silence_step) {
    fine_bins.assign(fine_bins.size(), -110.0F);
    const auto timestamp =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(2 * slow_steps) +
                                   silence_step) *
        10'000'000;
    static_cast<void>(
        slow_bank.updateSpectrum(timestamp, 0.0, 1'000.0, fine_bins));
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = timestamp;
    block.sample_count = 80;
    static_cast<void>(slow_bank.processSamples(block));
  }
  expect(slow_bank.channels().empty(),
         "configure() applies a shorter decoded-signal timeout immediately "
         "to an already-verified track rather than only at construction");

  cwassistant::core::SpectrumAnalyzer pipeline_analyzer({
      .fft_size = 2'048,
      .averaging_frames = 1,
      .frame_rate_hz = 60,
      .audio_dc_rejection = true,
      .audio_automatic_gain = false,
      .audio_gain_db = 0.0F,
      .audio_automatic_gain_target_dbfs = -12.0F,
      .audio_automatic_bandwidth = false,
      .audio_lower_frequency_hz = 0.0,
      .audio_upper_frequency_hz = 24'000.0,
  });
  CwChannelBank pipeline_bank(
      {.minimum_spectral_observations = 1, .minimum_verification_symbols = 0});
  cwassistant::core::RealtimeSampleBlock pipeline_block;
  pipeline_block.stream.sample_rate_hz = 48'000.0;
  pipeline_block.sample_count = 2'048;
  for (std::size_t index = 0; index < pipeline_block.sample_count; ++index) {
    const float phase = 2.0F * std::numbers::pi_v<float> * 1'000.0F *
                        static_cast<float>(index) / 48'000.0F;
    pipeline_block.samples[index] = {0.5F * std::sin(phase), 0.0F};
  }
  const auto pipeline_frames = pipeline_analyzer.process(pipeline_block);
  for (const auto& frame : pipeline_frames) {
    static_cast<void>(pipeline_bank.updateSpectrum(
        frame.timestamp_ns, frame.lower_frequency_hz, frame.upper_frequency_hz,
        frame.bins_dbfs));
  }
  static_cast<void>(pipeline_bank.processSamples(pipeline_block));
  expect(pipeline_frames.size() == 1 && pipeline_bank.channels().size() == 1 &&
             std::abs(pipeline_bank.channels().front().frequency_hz - 1'000.0) <
                 30.0 &&
             pipeline_bank.channels().front().snr_db > 6.0F,
         "shared FFT discovery feeds raw narrowband channel evidence");
}

void test_established_cw_track_reserves_its_carrier_ridge() {
  using namespace cwassistant::core;
  CwChannelBank bank({
      .minimum_separation_hz = 45.0,
      .minimum_spectral_observations = 1,
      .minimum_verification_symbols = 0,
      .minimum_key_transitions = 0,
      .minimum_cadence_observations = 0,
      .minimum_verification_timing_quality = 0.0F,
      .minimum_verification_cadence_quality = 0.0F,
      .minimum_character_confidence = 0.0F,
      .minimum_plausibility_check_characters = 10,
      .maximum_simple_character_fraction = 0.80F,
      .minimum_narrowband_coherence = 0.0F,
      .maximum_verification_unknown_fraction = 1.0F,
      .verification_enter_seconds = 0.0,
  });
  constexpr double sample_rate_hz = 8'000.0;
  constexpr double carrier_hz = 500.0;
  constexpr double stronger_skirt_hz = 530.0;
  std::vector<float> bins(1'001, -110.0F);
  std::uint64_t now = 0;
  double phase = 0.0;

  const auto step = [&](const bool keyed, const bool stronger_skirt) {
    bins.assign(bins.size(), -110.0F);
    if (keyed) {
      bins[static_cast<std::size_t>(carrier_hz)] = -65.0F;
      if (stronger_skirt) {
        bins[static_cast<std::size_t>(stronger_skirt_hz)] = -55.0F;
      }
    }
    static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));

    RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate_hz;
    block.timestamp_ns = now;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          keyed ? 0.35F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
      phase += 2.0 * std::numbers::pi * carrier_hz / sample_rate_hz;
    }
    static_cast<void>(bank.processSamples(block));
    now += 10'000'000U;
  };
  const auto feed = [&](const bool keyed, const int milliseconds,
                        const bool stronger_skirt = false) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) {
      step(keyed, stronger_skirt);
    }
  };

  for (int symbol = 0; symbol < 8; ++symbol) {
    feed(true, 60);
    feed(false, 60);
  }
  auto diagnostics = bank.allTrackDiagnostics();
  auto established = std::find_if(
      diagnostics.cbegin(), diagnostics.cend(), [](const auto& track) {
        return track.verification_state == CwTrackState::Verified &&
               std::abs(track.frequency_hz - carrier_hz) < 2.0;
      });
  expect(established != diagnostics.cend(),
         "ridge-reservation fixture establishes the true CW carrier");
  if (established == diagnostics.cend()) return;
  const std::uint64_t established_id = established->id;

  // Add enough deliberately implausible one-element text to demote the live
  // verification state while retaining its established identity. This is the
  // field lifecycle in which an adjacent duplicate used to appear.
  for (int symbol = 0; symbol < 30; ++symbol) {
    feed(true, 60, true);
    feed(false, 180, true);
  }
  diagnostics = bank.allTrackDiagnostics();
  established = std::find_if(diagnostics.cbegin(), diagnostics.cend(),
                             [established_id](const auto& track) {
                               return track.id == established_id;
                             });
  expect(established != diagnostics.cend() &&
             established->verification_state == CwTrackState::Candidate,
         "ridge reservation survives demotion of an established identity");

  // The 30 Hz neighbor is deliberately 10 dB stronger but remains inside the
  // 45 Hz global peak-separation radius. The established decoder must reserve
  // its own nearest raw ridge before strength ranking considers that neighbor.
  feed(true, 1'200, true);
  diagnostics = bank.allTrackDiagnostics();
  established = std::find_if(diagnostics.cbegin(), diagnostics.cend(),
                             [established_id](const auto& track) {
                               return track.id == established_id;
                             });
  const auto adjacent_duplicates = std::count_if(
      diagnostics.cbegin(), diagnostics.cend(), [](const auto& track) {
        return std::abs(track.frequency_hz - carrier_hz) <= 45.0;
      });
  expect(established != diagnostics.cend() &&
             std::abs(established->frequency_hz - carrier_hz) < 2.0 &&
             adjacent_duplicates == 1,
         "an established CW track retains the true carrier and does not "
         "spawn a duplicate when a stronger adjacent skirt appears");
}

void test_cw_channel_bank_state_reason_consistency() {
  using cwassistant::core::CwChannelBank;
  using cwassistant::core::CwVerificationReason;
  constexpr double sample_rate = 8'000.0;
  // An impossible symbol requirement guarantees tracks can reach
  // Morse-likely but never Verified, and default coherence/cadence
  // thresholds against a plain tone naturally flicker (confirmed by direct
  // measurement: even a clean single tone's narrowband_coherence oscillates
  // above and below its threshold from one keying edge to the next). That
  // flicker is exactly what must never leave a track reporting an
  // inconsistent state/reason pair.
  CwChannelBank bank({.minimum_verification_symbols = 1'000});
  std::vector<float> bins(1'001, -100.0F);
  double phase = 0.0;
  std::uint64_t now = 0;
  bool observed_any_morse_likely = false;

  std::vector<bool> keying;
  const auto append_units = [&keying](const bool keyed, const int units) {
    keying.insert(keying.end(), units * 10, keyed);
  };
  const auto append_letter = [&append_units](const std::string_view elements) {
    for (std::size_t index = 0; index < elements.size(); ++index) {
      append_units(true, elements[index] == '.' ? 1 : 3);
      append_units(false, index + 1 == elements.size() ? 3 : 1);
    }
  };
  append_letter("...");
  append_letter("---");
  append_letter("...");
  append_units(false, 4);
  const int steps = static_cast<int>(keying.size()) * 40;
  for (int step = 0; step < steps; ++step) {
    const bool keyed = keying[static_cast<std::size_t>(step) % keying.size()];
    bins.assign(bins.size(), -100.0F);
    if (keyed) bins[700] = -68.0F;
    static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = now;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          keyed ? 0.32F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
      phase += 2.0 * std::numbers::pi * 700.0 / sample_rate;
    }
    static_cast<void>(bank.processSamples(block));
    now += 10'000'000;

    const auto diagnostics = bank.verificationDiagnostics();
    if (diagnostics.morse_likely_tracks > 0) observed_any_morse_likely = true;
    std::size_t pre_morse_likely_reasons = 0;
    std::size_t post_morse_likely_reasons = 0;
    for (std::size_t reason = 0;
         reason < diagnostics.current_reason_counts.size(); ++reason) {
      const auto count = diagnostics.current_reason_counts[reason];
      if (reason <=
          static_cast<std::size_t>(CwVerificationReason::LowCadenceQuality)) {
        pre_morse_likely_reasons += count;
      } else if (reason <= static_cast<std::size_t>(
                               CwVerificationReason::NeedsSustainedEvidence)) {
        post_morse_likely_reasons += count;
      }
    }
    expect(pre_morse_likely_reasons == diagnostics.candidate_tracks,
           "every candidate-state track reports a pre-Morse-likely gate "
           "reason, and no other track does, at every measured instant");
    expect(post_morse_likely_reasons == diagnostics.morse_likely_tracks,
           "every Morse-likely track reports a post-Morse-likely gate "
           "reason, and no other track does, at every measured instant");
  }
  expect(observed_any_morse_likely,
         "the test scenario actually exercises the Morse-likely state at "
         "least once, so the consistency checks above are not vacuous");
}

void test_operator_selected_cw_probe() {
  using namespace cwassistant::core;
  CwChannelBank bank(
      {.decoded_track_retention_seconds = 7.0, .maximum_tracks = 2});
  std::vector<float> quiet_spectrum(101, -100.0F);

  expect(bank.selectFrequency(700.0) == 0,
         "manual probe requires a current spectrum range");
  static_cast<void>(
      bank.updateSpectrum(1'000'000'000ULL, 200.0, 1'200.0, quiet_spectrum));
  expect(bank.selectFrequency(150.0) == 0,
         "manual probe rejects a frequency outside the displayed passband");

  const std::uint64_t selected_id = bank.selectFrequency(700.0);
  expect(selected_id != 0 && bank.channels().size() == 1,
         "manual probe is published immediately for its operator session");
  if (!bank.channels().empty()) {
    const auto& selected = bank.channels().front();
    expect(selected.id == selected_id && selected.operator_selected,
           "manual probe retains its explicit operator-selected identity");
    expect(!selected.verified_cw &&
               selected.verification_state != CwTrackState::Verified,
           "manual selection does not bypass CW verification");
    expect(selected.callsign.empty() && selected.text.empty() &&
               selected.provisional_text.empty(),
           "a newly selected probe exposes no unverified decoded identity");
  }

  expect(
      bank.selectFrequency(710.0) == selected_id && bank.channels().size() == 1,
      "nearby repeated clicks refresh one bounded probe");
  const std::uint64_t close_pileup_id = bank.selectFrequency(739.0);
  expect(close_pileup_id != 0 && close_pileup_id != selected_id &&
             bank.channels().size() == 2,
         "manual clicks create distinct probes for pileup lanes 29 Hz apart");

  std::vector<float> weak_spectrum(101, -100.0F);
  weak_spectrum[50] = -96.0F;  // 4 dB: below normal 7 dB acquisition.
  static_cast<void>(
      bank.updateSpectrum(1'100'000'000ULL, 200.0, 1'200.0, weak_spectrum));
  const auto diagnostics = bank.allTrackDiagnostics();
  const auto weak_probe =
      std::find_if(diagnostics.cbegin(), diagnostics.cend(),
                   [selected_id](const CwTrackDiagnostic& diagnostic) {
                     return diagnostic.id == selected_id;
                   });
  expect(diagnostics.size() == 2 && weak_probe != diagnostics.cend() &&
             weak_probe->operator_selected && weak_probe->matched &&
             weak_probe->spectral_observations == 1 &&
             std::abs(weak_probe->frequency_hz - 710.0) < 0.1,
         "manual probe accumulates measured sub-threshold center evidence");
  expect(!bank.channels().empty() && !bank.channels().front().verified_cw,
         "manual weak-signal priority still does not bypass verification");

  static_cast<void>(
      bank.updateSpectrum(8'000'000'001ULL, 200.0, 1'200.0, quiet_spectrum));
  expect(
      bank.channels().empty(),
      "an unverified manual probe expires after the configured stream timeout");

  CwChannelBank following_bank({
      .decoded_track_retention_seconds = 7.0,
      .detector_averaging_seconds = 0.0,
      .detector_frame_interval_seconds = 0.0,
      .minimum_spectral_observations = 200,
      .minimum_narrowband_coherence = 0.0F,
      .presentation_follow_deadband_hz = 1.0,
      .presentation_follow_slew_hz_per_second = 100.0,
      .presentation_follow_stable_seconds = 0.0,
      .presentation_follow_maximum_drift_hz_per_second = 200.0,
      .presentation_follow_maximum_mad_hz = 20.0,
  });
  std::vector<float> following_spectrum(1'001U, -100.0F);
  static_cast<void>(following_bank.updateSpectrum(1'000'000'000ULL, 200.0,
                                                  1'200.0, following_spectrum));
  const std::uint64_t following_id = following_bank.selectFrequency(700.0);
  for (std::uint64_t frame = 1; frame <= 26; ++frame) {
    std::fill(following_spectrum.begin(), following_spectrum.end(), -100.0F);
    following_spectrum[515U] = -20.0F;  // Carrier moved from 700 to 715 Hz.
    static_cast<void>(
        following_bank.updateSpectrum(1'000'000'000ULL + frame * 100'000'000ULL,
                                      200.0, 1'200.0, following_spectrum));
  }
  const auto followed = std::find_if(
      following_bank.channels().cbegin(), following_bank.channels().cend(),
      [following_id](const CwChannelSnapshot& channel) {
        return channel.id == following_id;
      });
  expect(followed != following_bank.channels().cend() &&
             followed->operator_selected && !followed->verified_cw &&
             followed->frequency_hz > 710.0 &&
             followed->presentation_frequency_hz > 705.0,
         "an unverified manual region follows sustained carrier movement");

  CwChannelBank close_lane_bank({.maximum_tracks = 3});
  static_cast<void>(close_lane_bank.updateSpectrum(1'000'000'000ULL, 200.0,
                                                   1'200.0, quiet_spectrum));
  const std::uint64_t lower_lane_id = close_lane_bank.selectFrequency(710.0);
  const std::uint64_t upper_lane_id = close_lane_bank.selectFrequency(739.0);
  std::vector<float> lower_lane_spectrum(101, -100.0F);
  lower_lane_spectrum[51] = -80.0F;  // 710 Hz, 29 Hz below the click.
  static_cast<void>(close_lane_bank.updateSpectrum(
      1'100'000'000ULL, 200.0, 1'200.0, lower_lane_spectrum));
  const auto close_lane_diagnostics = close_lane_bank.allTrackDiagnostics();
  const auto upper_lane = std::find_if(
      close_lane_diagnostics.cbegin(), close_lane_diagnostics.cend(),
      [upper_lane_id](const CwTrackDiagnostic& diagnostic) {
        return diagnostic.id == upper_lane_id;
      });
  const auto lower_lane = std::find_if(
      close_lane_diagnostics.cbegin(), close_lane_diagnostics.cend(),
      [lower_lane_id](const CwTrackDiagnostic& diagnostic) {
        return diagnostic.id == lower_lane_id;
      });
  expect(close_lane_diagnostics.size() == 2 &&
             lower_lane != close_lane_diagnostics.cend() &&
             lower_lane->matched &&
             upper_lane != close_lane_diagnostics.cend() &&
             std::abs(upper_lane->frequency_hz - 739.0) < 0.1 &&
             !upper_lane->matched,
         "two close manual regions preserve the specifically matched lane");
}

void test_cw_channel_presentation_frequency_model() {
  using namespace cwassistant::core;
  CwChannelBank bank({
      .decoded_track_retention_seconds = 4.0,
      // This fixture drives fabricated instantaneous spectra and asserts exact
      // presentation geometry, so detector smoothing is disabled to isolate the
      // centering/deadband/slew logic. Frame-rate and averaging invariance of
      // the detector itself is covered separately.
      .detector_averaging_seconds = 0.0,
      .detector_frame_interval_seconds = 0.0,
      .minimum_verification_symbols = 1,
      .minimum_key_transitions = 2,
      .minimum_cadence_observations = 1,
      .minimum_verification_timing_quality = 0.0F,
      .minimum_verification_cadence_quality = 0.0F,
      .minimum_character_confidence = 0.0F,
      .minimum_narrowband_coherence = 0.0F,
      .maximum_verification_unknown_fraction = 1.0F,
      .presentation_follow_stable_seconds = 0.5,
      .verification_enter_seconds = 0.0,
  });
  constexpr double sample_rate = 8'000.0;
  constexpr double acquired_hz = 500.0;
  constexpr double carrier_hz = 558.0;
  std::vector<float> bins(1'001, -110.0F);
  std::uint64_t now = 0;
  double phase = 0.0;

  // The first acquisition is deliberately 58 Hz low. Subsequent observations
  // remain within the immutable origin's hard association radius and converge
  // the adaptive DSP center before verification.
  bins[static_cast<std::size_t>(acquired_hz)] = -68.0F;
  static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));
  now += 10'000'000U;

  const auto step = [&](const bool keyed, const double frequency_hz,
                        const bool adjacent = false) {
    bins.assign(bins.size(), -110.0F);
    if (keyed) {
      bins[static_cast<std::size_t>(std::llround(frequency_hz))] = -68.0F;
      if (adjacent) bins[630] = -67.0F;
    }
    static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));
    RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = now;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          keyed ? 0.32F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
      phase += 2.0 * std::numbers::pi * frequency_hz / sample_rate;
    }
    static_cast<void>(bank.processSamples(block));
    now += 10'000'000U;
  };
  const auto feed = [&](const bool keyed, const int milliseconds,
                        const double frequency_hz) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10)
      step(keyed, frequency_hz);
  };
  const auto letter = [&](const std::string_view elements,
                          const double frequency_hz) {
    for (std::size_t index = 0; index < elements.size(); ++index) {
      feed(true, elements[index] == '.' ? 60 : 180, frequency_hz);
      feed(false, index + 1U == elements.size() ? 180 : 60, frequency_hz);
    }
  };
  for (int repeat = 0; repeat < 3; ++repeat) {
    letter("...", carrier_hz);
    letter("---", carrier_hz);
    letter("...", carrier_hz);
  }

  auto diagnostics = bank.allTrackDiagnostics();
  auto main_track = std::find_if(
      diagnostics.begin(), diagnostics.end(), [](const auto& track) {
        return track.verification_state == CwTrackState::Verified &&
               std::abs(track.frequency_hz - carrier_hz) < 10.0;
      });
  expect(main_track != diagnostics.end(),
         "biased acquisition still produces one verified carrier track");
  if (main_track == diagnostics.end()) return;
  const std::uint64_t main_id = main_track->id;
  expect(
      std::abs(main_track->identity_origin_frequency_hz - acquired_hz) < 2.0 &&
          std::abs(main_track->presentation_frequency_hz - carrier_hz) < 3.0,
      "first verification robustly centers presentation without moving "
      "identity origin");
  expect(!main_track->matched && main_track->active &&
             main_track->match_age_seconds <= 0.75 &&
             main_track->color_index == 0,
         "diagnostics expose match, activity, and stable color state");

  // Symmetric carrier jitter remains inside the deadband and cannot flicker
  // the presentation center.
  const double centered = main_track->presentation_frequency_hz;
  for (int index = 0; index < 160; ++index) {
    const double jittered = carrier_hz + (index % 2 == 0 ? -3.0 : 3.0);
    step(index % 12 < 6, jittered);
  }
  diagnostics = bank.allTrackDiagnostics();
  main_track = std::find_if(
      diagnostics.begin(), diagnostics.end(),
      [main_id](const auto& track) { return track.id == main_id; });
  expect(main_track != diagnostics.end() &&
             std::abs(main_track->presentation_frequency_hz - centered) < 1.0,
         "bounded jitter does not move the presentation center");

  // A genuine slow carrier motion must persist beyond the robust-window window
  // and stable-time guard, then follows at the configured slew rate. Its
  // absolute center remains capped relative to the immutable identity origin.
  for (int index = 0; index < 700; ++index) {
    const double moving = carrier_hz + 9.0 * static_cast<double>(index) / 699.0;
    step(index % 12 < 6, moving);
  }
  diagnostics = bank.allTrackDiagnostics();
  main_track = std::find_if(
      diagnostics.begin(), diagnostics.end(),
      [main_id](const auto& track) { return track.id == main_id; });
  expect(main_track != diagnostics.end() &&
             main_track->presentation_frequency_hz > centered + 2.0 &&
             main_track->presentation_frequency_hz <= acquired_hz + 65.01 &&
             std::abs(main_track->identity_origin_frequency_hz - acquired_hz) <
                 2.0,
         "sustained slow motion follows within an immutable-origin hard bound");

  // An adjacent strong carrier gets a separate candidate and cannot pull the
  // verified marker. Small cumulative innovations likewise stop at the same
  // absolute origin bound rather than moving that bound themselves.
  const double before_adjacent = main_track->presentation_frequency_hz;
  for (int index = 0; index < 80; ++index) step(index % 12 < 6, 565.0, true);
  for (int index = 0; index < 120; ++index)
    step(true, 565.0 + 0.5 * static_cast<double>(index));
  diagnostics = bank.allTrackDiagnostics();
  main_track = std::find_if(
      diagnostics.begin(), diagnostics.end(),
      [main_id](const auto& track) { return track.id == main_id; });
  expect(main_track != diagnostics.end() &&
             main_track->presentation_frequency_hz <= acquired_hz + 65.01 &&
             main_track->identity_origin_frequency_hz < acquired_hz + 2.0 &&
             main_track->presentation_frequency_hz >= before_adjacent - 1.0,
         "adjacent and cumulative peaks cannot walk the identity origin or "
         "presentation cap");

  const double before_silence = main_track->presentation_frequency_hz;
  for (int index = 0; index < 120; ++index) step(false, 565.0);
  diagnostics = bank.allTrackDiagnostics();
  main_track = std::find_if(
      diagnostics.begin(), diagnostics.end(),
      [main_id](const auto& track) { return track.id == main_id; });
  expect(main_track != diagnostics.end() && !main_track->active &&
             !main_track->key_down && main_track->match_age_seconds > 0.75 &&
             std::abs(main_track->presentation_frequency_hz - before_silence) <
                 0.01,
         "silence exposes inactive match age without moving presentation");

  const double dsp_before_shift = main_track->frequency_hz;
  const double origin_before_shift = main_track->identity_origin_frequency_hz;
  const double presentation_before_shift =
      main_track->presentation_frequency_hz;
  bank.shiftTrackedFrequencies(200.0);
  diagnostics = bank.allTrackDiagnostics();
  main_track = std::find_if(
      diagnostics.begin(), diagnostics.end(),
      [main_id](const auto& track) { return track.id == main_id; });
  expect(main_track != diagnostics.end() &&
             std::abs(main_track->frequency_hz - (dsp_before_shift + 200.0)) <
                 0.01 &&
             std::abs(main_track->identity_origin_frequency_hz -
                      (origin_before_shift + 200.0)) < 0.01 &&
             std::abs(main_track->presentation_frequency_hz -
                      (presentation_before_shift + 200.0)) < 0.01,
         "known VFO retune shifts DSP, identity, and presentation exactly "
         "together");

  // Let the original track expire, then reacquire directly at the corrected
  // carrier. Its new association origin is about 58 Hz away from the old
  // biased origin, but the frozen color lease is tied to the robust verified
  // carrier center and must preserve the operator-facing color.
  // Outlast the verified track's 4 s decoded-signal retention explicitly. The
  // fixture previously relied on spectral persistence decaying faster than
  // retention, which only held because decay was counted in frames and this
  // fixture drives spectra at 100 Hz; persistence is now accrued in elapsed
  // time, so the expiry this case depends on is stated directly.
  feed(false, 4'400, carrier_hz + 200.0);
  for (int repeat = 0; repeat < 3; ++repeat) {
    letter("...", carrier_hz + 200.0);
    letter("---", carrier_hz + 200.0);
    letter("...", carrier_hz + 200.0);
  }
  diagnostics = bank.allTrackDiagnostics();
  const auto replacement = std::find_if(
      diagnostics.begin(), diagnostics.end(), [main_id](const auto& track) {
        return track.id != main_id &&
               track.verification_state == CwTrackState::Verified;
      });
  expect(replacement != diagnostics.end() && replacement->color_index == 0 &&
             std::abs(replacement->presentation_frequency_hz -
                      (carrier_hz + 200.0)) < 3.0,
         "reacquisition after a biased origin preserves the verified-carrier "
         "color lease");
}

void test_cw_channel_bank_implausible_character_distribution() {
  using cwassistant::core::isCharacterDistributionImplausible;

  // Real field data (a genuine debug capture of noise misclassified as CW)
  // showed E+T fractions of 0.59, 0.45, and 0.76 for false-positive tracks,
  // versus 0.27 for the most plausible real candidate and 0.25 for the
  // benchmark's own legitimate decoded text ("SOSCQTEST123") — the default
  // 0.35 threshold sits in the gap between those two groups.
  expect(!isCharacterDistributionImplausible("SOSCQTEST123", 10, 0.35F),
         "legitimate varied decoded text is never flagged implausible");
  expect(isCharacterDistributionImplausible("TETETETETETETETETET", 10, 0.35F),
         "a run of only E/T characters is flagged implausible");
  expect(!isCharacterDistributionImplausible("TETETETETETETETETET", 40, 0.35F),
         "the check does not fire before minimum_characters worth of text "
         "has accumulated, since the fraction is not yet meaningful");
  expect(!isCharacterDistributionImplausible("", 0, 0.35F),
         "empty decoded text is never flagged (no letters to judge)");
  expect(isCharacterDistributionImplausible("TE TE TE TE TE TE TE TE TE TE", 10,
                                            0.35F),
         "inter-character spaces are ignored when computing the fraction, "
         "so a spaced-out run of only E/T is still flagged");
  expect(!isCharacterDistributionImplausible("SOS DE W1AW K", 10, 0.60F),
         "raising the threshold config value relaxes the gate accordingly");

  using cwassistant::core::CwChannelBank;
  constexpr double sample_rate = 8'000.0;
  CwChannelBank recovery_bank({
      .minimum_verification_symbols = 20,
      .minimum_plausibility_check_characters = 6,
      .decoder_recovery_seconds = 0.5,
  });
  std::vector<float> bins(1'001, -110.0F);
  double phase = 0.0;
  std::uint64_t now = 0;
  const auto feed = [&](const bool keyed, const int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) {
      bins.assign(bins.size(), -110.0F);
      if (keyed) bins[500] = -65.0F;
      static_cast<void>(recovery_bank.updateSpectrum(now, 0.0, 1'000.0, bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = now;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            keyed ? 0.30F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
        phase += 2.0 * std::numbers::pi * 500.0 / sample_rate;
      }
      static_cast<void>(recovery_bank.processSamples(block));
      now += 10'000'000;
    }
  };
  for (int repetition = 0; repetition < 12; ++repetition) {
    feed(true, 60);  // E
    feed(false, 180);
    feed(true, 180);  // T
    feed(false, 180);
  }
  expect(recovery_bank.verificationDiagnostics().decoder_reacquisitions > 0 &&
             recovery_bank.allTrackDiagnostics().size() == 1,
         "a cadence-confirmed unverified decoder trapped in implausible "
         "single-element output reacquires while retaining its carrier");
}

void test_transmit_guard() {
  using cwassistant::core::CallsignPolicy;
  using cwassistant::core::TransmitGuard;
  CallsignPolicy policy;
  TransmitGuard guard(policy);
  expect(!guard.begin_transmission(), "cannot transmit while disarmed");
  expect(guard.arm(), "operator can arm TX");
  expect(guard.request_qso("i1abc"), "valid selected call requests QSO");
  expect(!guard.confirm("I1XYZ"), "confirmation must match selected call");
  expect(guard.confirm("I1ABC"), "operator confirms selected call");
  expect(!guard.stage_message("CQ <SCRIPT>"),
         "free text rejects unsupported or executable-looking syntax");
  // A bracketed token is admitted only when it names a real prosign, so the
  // check above cannot be satisfied by anything an operator might paste.
  expect(guard.stage_message("TU <SK>"),
         "a message closing with a prosign is accepted");
  expect(guard.stage_message("<SOS> <SOS>"),
         "a distress call can be staged for transmission");
  expect(!guard.stage_message("CQ <AR"),
         "an unterminated prosign is refused");
  expect(!guard.stage_message("CQ <>"),
         "an empty bracketed token is refused");
  expect(guard.stage_message("  de iu0lfq   pse k  "),
         "operator can stage bounded Morse free text");
  expect(guard.pending_message() == "DE IU0LFQ PSE K",
         "free text is normalized for an exact preview");
  expect(!guard.confirm_message("DE IU0LFQ K"),
         "changed preview cannot be confirmed");
  expect(!guard.begin_transmission(),
         "unconfirmed free text cannot reach transmission");
  expect(guard.confirm_message("de iu0lfq pse k"),
         "operator confirms the exact normalized preview");
  expect(guard.begin_transmission(), "confirmed QSO permits TX");
  expect(!guard.key_down(),
         "transmit intent does not claim that KEY is asserted");
  expect(guard.observe_key_state(true, 1'000'000'000ULL),
         "guard observes the first KEY assertion");
  expect(guard.key_down(), "an observed KEY assertion is exposed");
  expect(guard.observe_key_state(false, 1'100'000'000ULL),
         "KEY release clears the continuous-key timer");
  expect(!guard.key_down(), "an observed KEY release clears ON AIR state");
  expect(guard.finish_transmission(), "TX completes inside the confirmed QSO");
  expect(guard.state() == cwassistant::core::TransmitState::Confirmed,
         "QSO callsign confirmation survives between messages");
  expect(guard.end_qso(), "operator explicitly ends the confirmed QSO");
  expect(policy.add_ignored("w1aw"), "operator can ignore a callsign");
  expect(!guard.request_qso("W1AW"), "ignored callsign cannot request QSO");
  expect(guard.request_qso("K1ABC"), "another call can request QSO");
  expect(policy.add_ignored("K1ABC"), "pending call can become ignored");
  expect(!guard.confirm("K1ABC"), "new ignore rule cancels confirmation");
  expect(guard.state() == cwassistant::core::TransmitState::Armed,
         "ignore rule returns TX guard to armed state");
  guard.disarm();
  expect(!guard.observe_key_state(true, 2'000'000'000ULL) &&
             guard.state() == cwassistant::core::TransmitState::Fault,
         "KEY assertion outside guarded TX latches a fault");
  expect(guard.reset_fault(), "out-of-state KEY fault resets to disarmed");
  expect(guard.arm() && guard.request_qso("K2XYZ") && guard.confirm("K2XYZ") &&
             guard.stage_message("TEST") && guard.confirm_message("TEST") &&
             guard.begin_transmission(),
         "watchdog fixture reaches guarded transmission");
  expect(guard.observe_key_state(true, 3'000'000'000ULL) &&
             !guard.observe_key_state(
                 true, 3'000'000'000ULL +
                           TransmitGuard::kMaximumContinuousKeyDownNs + 1ULL) &&
             guard.state() == cwassistant::core::TransmitState::Fault,
         "continuous KEY beyond the deadline latches a fault");
  expect(guard.reset_fault(), "watchdog fault reset returns to disarmed");
  expect(guard.arm(), "guard can re-arm after an explicit fault reset");
  expect(guard.begin_tune(), "an armed operator can start TUNE");
  expect(guard.state() == cwassistant::core::TransmitState::Tuning &&
             guard.observe_key_state(true, 10'000'000'000ULL),
         "TUNE enters its distinct guarded KEY state");
  expect(guard.observe_key_state(
             true, 10'000'000'000ULL + TransmitGuard::kMaximumTuneKeyDownNs),
         "TUNE remains valid through its exact 15-second limit");
  expect(guard.finish_tune() &&
             guard.state() == cwassistant::core::TransmitState::Armed,
         "pressing TUNE again releases KEY and restores the armed state");
  expect(guard.begin_tune() &&
             guard.observe_key_state(true, 30'000'000'000ULL) &&
             !guard.observe_key_state(
                 true, 30'000'000'000ULL +
                           TransmitGuard::kMaximumTuneKeyDownNs + 1ULL) &&
             guard.state() == cwassistant::core::TransmitState::Fault,
         "TUNE beyond 15 seconds releases KEY and latches a fault");
  expect(guard.reset_fault() && guard.arm(),
         "TUNE watchdog fault requires reset before re-arming");
  guard.emergency_release();
  expect(guard.state() == cwassistant::core::TransmitState::Fault,
         "emergency release clears pending TX and latches a fault");
  guard.trip_fault();
  expect(!guard.arm(), "fault cannot be bypassed by arming");
  expect(guard.reset_fault(), "fault reset returns to disarmed");
}

void test_cw_transmit_encoder() {
  using cwassistant::core::CwTransmitEncoder;
  // A prosign is one symbol: its letters run together with no character gap,
  // which is the whole difference between <AR> and A R. <AR> is 9 units of
  // elements plus 4 internal element gaps; A R spelled out is the same
  // elements with a 3-unit character gap in the middle, so 2 units longer.
  const auto prosign = CwTransmitEncoder::encode("<AR>", 20U);
  const auto spelled = CwTransmitEncoder::encode("AR", 20U);
  expect(prosign.has_value() && spelled.has_value(),
         "a prosign and its spelled-out letters both encode");
  if (prosign.has_value() && spelled.has_value()) {
    const auto total = [](const auto& plan) {
      std::uint64_t units = 0U;
      for (const auto& span : plan->spans) units += span.duration_units;
      return units;
    };
    expect(total(prosign) + 2U == total(spelled),
           "a prosign is sent without the character gap that separates "
           "the same two letters");
    bool any_three_unit_gap = false;
    for (const auto& span : prosign->spans)
      if (!span.key_down && span.duration_units >= 3U) any_three_unit_gap = true;
    expect(!any_three_unit_gap,
           "a prosign contains no character gap at all");
  }
  // The owner's decision: a distress call is sendable. It passes the same
  // arming, confirmation, preview and explicit-send gates as any other
  // message, and the decoder can never initiate one.
  expect(CwTransmitEncoder::encode("<SOS>", 20U).has_value(),
         "a distress prosign can be transmitted");
  expect(!CwTransmitEncoder::encode("<XX>", 20U).has_value(),
         "an unknown prosign rejects the message instead of keying letters");
  expect(!CwTransmitEncoder::encode("<AR", 20U).has_value(),
         "an unterminated prosign rejects the message");
  expect(CwTransmitEncoder::encode("TU <SK>", 20U).has_value(),
         "a prosign closing an ordinary message encodes");
  const auto plan = CwTransmitEncoder::encode("SOS TEST", 20U);
  expect(plan.has_value(), "confirmed free text has a Morse timing plan");
  if (plan.has_value()) {
    expect(plan->dot_duration_ns == 60'000'000ULL,
           "PARIS timing gives a 60 ms dot at 20 WPM");
    expect(!plan->spans.empty() && plan->spans.front().key_down &&
               plan->spans.back().key_down,
           "timing plan starts and ends on keyed elements");
    std::uint64_t units = 0U;
    std::uint32_t longest_key_down = 0U;
    bool saw_word_gap = false;
    for (const auto& span : plan->spans) {
      units += span.duration_units;
      if (span.key_down)
        longest_key_down = std::max(longest_key_down, span.duration_units);
      else if (span.duration_units == 7U)
        saw_word_gap = true;
    }
    expect(units == 55U && plan->total_duration_ns == 3'300'000'000ULL,
           "SOS TEST uses exact standard Morse spacing");
    expect(longest_key_down == 3U && saw_word_gap,
           "plan distinguishes dashes and seven-unit word gaps");
  }
  expect(!CwTransmitEncoder::encode("SOS  TEST", 20U).has_value(),
         "encoder rejects non-normalized repeated spaces");
  expect(!CwTransmitEncoder::encode("sos", 20U).has_value(),
         "encoder accepts only the exact normalized preview");
  expect(!CwTransmitEncoder::encode("TEST", 4U).has_value() &&
             !CwTransmitEncoder::encode("TEST", 81U).has_value(),
         "encoder enforces the bounded operating-speed range");
  expect(CwTransmitEncoder::encode("CQ IU0LFQ/P?", 25U).has_value(),
         "portable callsigns and supported punctuation are encodable");
}

void test_selected_track_audio_monitor() {
  using cwassistant::core::CwChannelBank;
  using cwassistant::core::CwMonitorMode;
  using cwassistant::core::RealtimeSampleBlock;

  CwChannelBank bank({.minimum_verification_symbols = 0});
  std::vector<float> spectrum(291U, -100.0F);
  spectrum[90U] = -20.0F;  // 1000 Hz in the 100..3000 Hz test range.
  static_cast<void>(
      bank.updateSpectrum(1'000'000'000ULL, 100.0, 3'000.0, spectrum));
  const std::uint64_t selected = bank.selectFrequency(1'000.0);
  const std::uint64_t selected_second = bank.selectFrequency(1'500.0);
  expect(selected != 0U && selected_second != 0U && selected != selected_second,
         "monitor fixture creates two operator-selected lanes");

  constexpr double sample_rate = 48'000.0;
  RealtimeSampleBlock block;
  block.stream.sample_rate_hz = sample_rate;
  block.sample_count = block.samples.size();
  double target_phase = 0.0;
  double second_target_phase = 0.0;
  double interferer_phase = 0.0;
  const std::array selected_tracks{selected, selected_second, selected};
  bank.setMonitorTracks(CwMonitorMode::SelectedTrack, selected_tracks, 700.0);
  expect(bank.monitoredTrackIds().size() == 2U,
         "multi-stream monitor keeps a bounded unique track set");
  for (std::uint64_t block_index = 0; block_index < 24U; ++block_index) {
    block.timestamp_ns = 1'000'000'000ULL + block_index * 21'333'333ULL;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      target_phase += 2.0 * std::numbers::pi * 1'000.0 / sample_rate;
      second_target_phase += 2.0 * std::numbers::pi * 1'500.0 / sample_rate;
      interferer_phase += 2.0 * std::numbers::pi * 2'200.0 / sample_rate;
      block.samples[index] = {
          0.005F * static_cast<float>(std::sin(target_phase)) +
              0.005F * static_cast<float>(std::sin(second_target_phase)) +
              0.005F * static_cast<float>(std::sin(interferer_phase)),
          0.0F};
    }
    static_cast<void>(bank.processSamples(block));
  }
  const auto& isolated = bank.monitorAudio();
  expect(isolated.size() == block.sample_count,
         "selected monitor returns one audio sample per input sample");
  const auto magnitude_at = [&](const double frequency_hz) {
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t index = 0; index < isolated.size(); ++index) {
      const double phase = 2.0 * std::numbers::pi * frequency_hz *
                           static_cast<double>(index) / sample_rate;
      real += isolated[index] * std::cos(phase);
      imaginary -= isolated[index] * std::sin(phase);
    }
    return std::hypot(real, imaginary);
  };
  expect(magnitude_at(700.0) > 40.0 &&
             magnitude_at(700.0) > 8.0 * magnitude_at(1'000.0) &&
             magnitude_at(700.0) > 8.0 * magnitude_at(1'500.0) &&
             magnitude_at(700.0) > 8.0 * magnitude_at(2'200.0),
         "selected monitor normalizes weak selected carriers at one pitch and "
         "rejects unselected audio");

  bank.setMonitor(CwMonitorMode::FullReceiver);
  static_cast<void>(bank.processSamples(block));
  expect(bank.monitorAudio().size() == block.sample_count &&
             bank.monitorAudio().front() == block.samples[0].real(),
         "full receiver monitor preserves the complete input window");
  bank.setMonitor(CwMonitorMode::Off);
  static_cast<void>(bank.processSamples(block));
  expect(bank.monitorAudio().empty(), "monitor off emits no audio");
}

void test_presented_speed_requires_evidence() {
  using namespace cwassistant::core;
  // Below a few decoded symbols the timing bank has nothing to choose between
  // its hypotheses: the value is still the seeded default and whichever anchor
  // briefly leads can be far from the truth. Observed on a receiver capture, a
  // 27 WPM station read 20 WPM before anything was decoded and reached 40 WPM
  // on its fourth key transition. An unsupported speed is not presented, and
  // the display renders an absent speed as a dash.
  CwChannelBank bank({.minimum_spectral_observations = 1,
                      .minimum_verification_symbols = 0,
                      .verification_enter_seconds = 0.0,
                      .verification_exit_seconds = 0.0});
  std::vector<float> bins(201, -110.0F);
  std::uint64_t now = 0;
  double phase = 0.0;
  constexpr double sample_rate = 8'000.0;
  const auto feed = [&](const bool keyed, const int milliseconds) {
    for (int step = 0; step < milliseconds / 10; ++step) {
      bins.assign(bins.size(), -110.0F);
      if (keyed) bins[60] = -55.0F;
      static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));
      RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = now;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            keyed ? 0.35F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
        phase += 2.0 * std::numbers::pi * 300.0 / sample_rate;
      }
      static_cast<void>(bank.processSamples(block));
      now += 10'000'000;
    }
  };
  feed(false, 100);
  feed(true, 60);
  feed(false, 180);
  const auto& early = bank.channels();
  const bool early_speed_hidden = early.empty() || early.front().wpm == 0.0;
  expect(early_speed_hidden,
         "no speed is presented before enough symbols support one");
}

void test_callsign_policy_prosign_glue() {
  using cwassistant::core::CallsignPolicy;
  // A missing word gap glues the prosign onto the callsign after it, and the
  // glued token then collects the context credit the callsign earned. Observed
  // on a receiver capture: a station sending CQ CQ CQ DE SV7BIO SV7BIO decoded
  // as "CQ CQ DESV7BIO SV7BIO SV7BIO" and the stream was labelled DESV7BIO,
  // although the real callsign stood alone twice in the same text.
  const auto glued =
      CallsignPolicy::best_complete_in_text("Q CQ DESV7BIO SV7BIO SV7BI O + ");
  expect(glued.has_value() && *glued == "SV7BIO",
         "a prosign glued to the callsign after it does not become the "
         "station label");

  // The split may only happen where what follows the prosign is itself a
  // callsign. A genuine German DE-prefixed call must survive intact: removing
  // DE from DE1ABC leaves 1ABC, which is not a callsign, so the token stands.
  const auto german =
      CallsignPolicy::best_complete_in_text("CQ DE DE1ABC DE1ABC K ");
  expect(german.has_value() && *german == "DE1ABC",
         "a genuine DE-prefixed callsign is not split apart");
}

void test_callsign_policy() {
  using cwassistant::core::CallsignPolicy;
  CallsignPolicy policy;
  expect(policy.add_ignored("  i1abc/p  "), "ignore list normalizes callsign");
  expect(policy.is_ignored("I1ABC/P"), "ignore matching is case insensitive");
  expect(!policy.add_ignored("I1ABC/P"), "ignore list rejects duplicates");
  expect(!policy.add_ignored("NOT A CALL"), "ignore list rejects invalid text");
  expect(policy.remove_ignored("i1abc/p"), "ignored callsign can be restored");
  expect(!policy.is_ignored("I1ABC/P"), "removed call is no longer ignored");
  expect(CallsignPolicy::latest_in_text("CQ TEST DE iu0lfq/p K") ==
             std::optional<std::string>("IU0LFQ/P"),
         "latest decoded callsign extraction supports portable calls");
  expect(CallsignPolicy::latest_in_text("DE EA8/W1AW ") ==
             std::optional<std::string>("EA8/W1AW"),
         "latest decoded callsign extraction supports operating prefixes");
  expect(!CallsignPolicy::latest_in_text("CQ TEST 599 ?"),
         "reports and operating words are not mistaken for callsigns");
  expect(!CallsignPolicy::latest_complete_in_text("CQ IU0LF"),
         "an unfinished decoded callsign is not presented as confirmed");
  expect(CallsignPolicy::latest_complete_in_text("CQ IU0LFQ ") ==
             std::optional<std::string>("IU0LFQ"),
         "a stable word gap confirms a structurally valid decoded callsign");
  expect(CallsignPolicy::latest_complete_in_text("GW0KRL KN28N6 RANDOM7 ") ==
             std::optional<std::string>("GW0KRL"),
         "callsign extraction ignores noise tokens with trailing or embedded "
         "digits after the district numeral");
  expect(CallsignPolicy::best_complete_in_text("CQ IU0LFQ ") ==
             std::optional<std::string>("IU0LFQ"),
         "CQ context supplies enough evidence for an automatic call label");
  expect(CallsignPolicy::best_complete_in_text("CQ TEST IU0LFQ IU0LFQ 599 ") ==
             std::optional<std::string>("IU0LFQ"),
         "an exactly repeated decoded callsign supplies label evidence");
  expect(!CallsignPolicy::best_complete_in_text("QA1RRK 599 "),
         "a lone random call-shaped token is not promoted to a stream label");
  expect(CallsignPolicy::best_complete_in_text("CQ TEST IU0LFQ ") ==
             std::optional<std::string>("IU0LFQ"),
         "a runner callsign after a CQ qualifier is identified");
  expect(CallsignPolicy::best_complete_in_text("TU IK3EYN CQ ") ==
             std::optional<std::string>("IK3EYN"),
         "a runner callsign in the acknowledgement pattern is identified");
  expect(CallsignPolicy::best_complete_in_text("IK3EYN UP ") ==
             std::optional<std::string>("IK3EYN"),
         "a split runner callsign before UP is identified");
  expect(CallsignPolicy::best_complete_in_text("EM90ZMV PSE K ") ==
             std::optional<std::string>("EM90ZMV"),
         "an ordinary-QSO callsign before PSE K is identified");
  expect(CallsignPolicy::best_complete_in_text("K3YL K3YL AR ") ==
             std::optional<std::string>("K3YL"),
         "a repeated ordinary-QSO callsign before AR is identified");
  expect(CallsignPolicy::best_complete_in_text("W1AW W1AW ") ==
             std::optional<std::string>("W1AW"),
         "an exactly repeated standalone caller callsign is identified");
  expect(CallsignPolicy::best_complete_in_text("IZ3ERM IZ3ERM ") ==
             std::optional<std::string>("IZ3ERM"),
         "a repeated acoustic-consensus callsign is eligible for a stream "
         "label without decoded conversation context");
  expect(CallsignPolicy::best_complete_in_text("CQ SN100PKP ") ==
             std::optional<std::string>("SN100PKP"),
         "a special-event callsign with one multi-digit block is identified");
  expect(CallsignPolicy::best_complete_in_text("DE 3DA0RU ") ==
             std::optional<std::string>("3DA0RU"),
         "a valid numeric-leading international prefix is retained");
  expect(!CallsignPolicy::best_complete_in_text("EA7G2NX 599 P7FN "),
         "report context and random callsign-shaped fragments are not labels");

  const auto participants = CallsignPolicy::qso_participants_in_text(
      "FB CARO EMILIO IK1WJQ DE IU8NMZ + K ");
  expect(participants == std::vector<std::string>({"IK1WJQ", "IU8NMZ"}),
         "CALL1 DE CALL2 identifies both participants on one simplex carrier");
  expect(CallsignPolicy::qso_participants_in_text("IK1WJQ DE IU8NMZ").empty(),
         "an unfinished participant handover is not exposed");
  expect(
      CallsignPolicy::qso_participants_in_text("IK1WJQ DE IK1WJQ K ").empty(),
      "one repeated callsign is not presented as a two-party QSO");
  expect(CallsignPolicy::qso_participants_in_text("REPORT DE 599 K ").empty(),
         "ordinary DE text without two callsigns does not invent participants");
  expect(CallsignPolicy::strong_sender_in_text("IK1WJQ DE IU8NMZ K ") ==
             std::optional<std::string>("IU8NMZ"),
         "an explicit CALL1 DE CALL2 handover attributes the sender");
  expect(CallsignPolicy::strong_sender_in_text("CQ CQ DE SV7BIO K ") ==
             std::optional<std::string>("SV7BIO"),
         "CQ DE CALL explicitly attributes a calling station");
  expect(!CallsignPolicy::strong_sender_in_text("SV7BIO SV7BIO K "),
         "a repeated bare callsign does not guess the current sender");
  expect(!CallsignPolicy::strong_sender_in_text("DE SV7BIO K "),
         "an isolated DE fragment is insufficient sender evidence");
  expect(CallsignPolicy::best_complete_in_parallel_texts(
             "TEST SM5IMO TU DL1NKB DAN 1854 TU SM5E ",
             "TEST SM5IMO TU DL1NKB DAN 1854 TU SM5U ",
             cwassistant::core::CwOperatorRole::Monitor,
             {}) == std::optional<std::string>("DL1NKB"),
         "independent paths reinforce the shared contest callsign");
  expect(!CallsignPolicy::best_complete_in_parallel_texts(
             {}, "TTE C 5NE S TU5NEET6T E HI ",
             cwassistant::core::CwOperatorRole::Monitor, {}),
         "a call created only by splitting a glued TU cannot label a stream");
  expect(
      CallsignPolicy::best_complete_in_parallel_texts(
          {}, "G4LJU G4LJU COLIN ", cwassistant::core::CwOperatorRole::Monitor,
          {}) == std::optional<std::string>("G4LJU"),
      "an exact refined-only callsign remains usable");
}

namespace {

// Loads the dictionaries the application ships. Returns false if either file
// is missing or empty, which is itself a failure worth reporting: the decoder
// would otherwise fall back to the compiled-in copy, which is correct but
// would hide whether the shipped files themselves are usable.
bool loadShippedDictionaries() {
  const auto read = [](const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
  };
  const std::string directory = CWA_DICTIONARY_DIR;
  auto& vocabulary = cwassistant::core::cwSharedVocabulary();
  vocabulary.clear();
  const auto words = vocabulary.importExchangeWords(
      read(directory + "/cw-abbreviations.txt"));
  const auto prefixes = vocabulary.importWordGapPrefixes(
      read(directory + "/cw-word-gap-prefixes.txt"));
  const auto distinctive = vocabulary.importDistinctiveTokens(
      read(directory + "/cw-distinctive-tokens.txt"));
  return words.inserted_tokens > 0 && words.ignored_lines == 0 &&
      prefixes.inserted_tokens > 0 && prefixes.ignored_lines == 0 &&
      prefixes.duplicate_tokens == 0 && distinctive.inserted_tokens > 0 &&
      distinctive.ignored_lines == 0 && distinctive.duplicate_tokens == 0;
}

}  // namespace

// setenv and unsetenv are POSIX and do not exist under MSVC, which provides
// _putenv_s instead; there an empty value removes the variable. The core tests
// link no Qt, so qputenv is not available here.
void setDictionaryDirectoryEnvironment(const char* value) {
#if defined(_WIN32)
  static_cast<void>(
      _putenv_s("CWA_DICTIONARY_DIR", value == nullptr ? "" : value));
#else
  if (value == nullptr) {
    static_cast<void>(unsetenv("CWA_DICTIONARY_DIR"));
  } else {
    static_cast<void>(setenv("CWA_DICTIONARY_DIR", value, 1));
  }
#endif
}

void test_cw_morse_alphabet_survives_missing_files() {
  // The decoder must never be left without an alphabet. Making it data with no
  // fallback shipped an application that tracked signals and decoded nothing,
  // with no diagnostic, because a packaged build could not read the file. The
  // compiled-in copy is generated from that same file at build time, so this
  // guards availability without a second table to maintain.
  auto& alphabet = cwassistant::core::cwMutableSharedMorseAlphabet();
  alphabet.clear();
  const char* previous = std::getenv("CWA_DICTIONARY_DIR");
  const std::string saved = previous == nullptr ? std::string{} : previous;
  setDictionaryDirectoryEnvironment(nullptr);

  const auto& recovered = cwassistant::core::cwSharedMorseAlphabet();
  expect(recovered.size() >= 56U,
         "the alphabet recovers with no dictionary directory at all");
  expect(recovered.symbolFor(".-") == "A" &&
             recovered.symbolFor("...-.-") == "<SK>",
         "the recovered alphabet decodes letters and prosigns");
  expect(cwassistant::core::cwMorseAlphabetLoadedFromBuiltin(),
         "the recovery is reported as coming from the compiled-in copy");

  // The other half of that report, and the one that used to be wrong. The
  // flag was derived by comparing symbol counts, but the shipped file and the
  // compiled-in copy are generated from one source and hold exactly the same
  // entries, so a perfectly healthy file load described itself as the
  // fallback -- the opposite of the fault the flag exists to reveal. Load the
  // shipped file the way the application does and the report must follow.
  alphabet.clear();
  std::ifstream shipped_alphabet(
      std::string(CWA_DICTIONARY_DIR) + "/morse-alphabet.txt",
      std::ios::binary);
  std::ostringstream shipped_alphabet_text;
  shipped_alphabet_text << shipped_alphabet.rdbuf();
  static_cast<void>(alphabet.importText(shipped_alphabet_text.str()));
  expect(alphabet.size() >= 56U,
         "the shipped alphabet file loads through a host-style import");
  expect(!cwassistant::core::cwMorseAlphabetLoadedFromBuiltin(),
         "an alphabet a caller loaded from file is not reported as built-in");

  if (!saved.empty()) setDictionaryDirectoryEnvironment(saved.c_str());
  alphabet.clear();
  static_cast<void>(cwassistant::core::cwSharedMorseAlphabet());
}

void test_cw_morse_alphabet() {
  // An alphabet that reached the decoder empty would decode nothing at all,
  // at all. Assert it is present and complete rather than discovering that as
  // a wall of unknown symbols in some unrelated benchmark.
  const auto& alphabet = cwassistant::core::cwSharedMorseAlphabet();
  expect(alphabet.size() >= 56U,
         "the shipped Morse alphabet loads every symbol");
  expect(alphabet.symbolFor(".-") == "A" && alphabet.symbolFor("-...") == "B",
         "the alphabet maps letters");
  expect(alphabet.symbolFor("-----") == "0" && alphabet.symbolFor(".....") == "5",
         "the alphabet maps digits");
  expect(alphabet.symbolFor("...-.-") == "<SK>",
         "the alphabet maps a multi-character prosign");
  expect(alphabet.symbolFor("...---...") == "<SOS>",
         "a distress call can be read even though it cannot be sent");
  expect(alphabet.symbolFor(".-.-.-.-.-").empty(),
         "an unknown element pattern has no symbol");

  cwassistant::core::CwMorseAlphabet parsed;
  const auto result = parsed.importText(
      "# comment\n\n.-  A\n-... B\n.- DUPLICATE\nxyz Q\n..-\n");
  expect(result.inserted_symbols == 2U && result.duplicate_codes == 1U &&
             result.ignored_lines == 2U,
         "the alphabet parser counts duplicates and rejects malformed lines");
  expect(parsed.symbolFor(".-") == "A",
         "a duplicate code does not overwrite the first symbol");
}

namespace {

// Reads the prefix file the application ships, the way a host does.
std::string readShippedCallsignPrefixes() {
  std::ifstream input(std::string(CWA_DICTIONARY_DIR) + "/callsign-prefixes.txt",
                      std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

// Real callsigns, from the bands the operator works. Every one of them must be
// admitted: this table's whole purpose is to refuse tokens, and a refusal
// costs a real station every single time it is heard, where admitting a dead
// prefix costs nothing because something else still has to agree.
constexpr std::array<const char*, 34> kHeardCallsigns{
    "W0ZA",   "K1ABC",  "G4XYZ",  "I2ABC",  "IU0LFQ", "DD7CW",  "9A1AA",
    "3DA0AB", "2E0ABC", "VP8ABC", "T77XX",  "E77AA",  "T88AB",  "VU2NXG",
    "RN9RF",  "RA6CA",  "JA1XYZ", "VK3ABC", "ZL2ABC", "PY2ABC", "LU1ABC",
    "EA3ABC", "F5ABC",  "ON4ABC", "PA3ABC", "SM5ABC", "OH2ABC", "OK1ABC",
    "S51ABC", "4X4ABC", "JY1",    "ZS6ABC", "VE3ABC", "KH6ABC"};

}  // namespace

void test_cw_callsign_prefix_table() {
  cwassistant::core::CwCallsignPrefixTable shipped;
  const auto imported = shipped.importText(readShippedCallsignPrefixes());
  expect(imported.inserted_blocks == 308U && imported.ignored_lines == 0U &&
             imported.duplicate_blocks == 0U && shipped.size() == 308U,
         "the shipped prefix file parses to 308 blocks with no rejected and "
         "no duplicated line");

  for (const char* call : kHeardCallsigns) {
    expect(shipped.isAllocatedPrefix(call),
           std::string("the prefix table admits the real callsign ") + call +
               ", which it would otherwise refuse every time the operator "
               "hears that station");
  }

  // The fault this exists for. A prefix in no allocation names a country that
  // is not there, which is far better evidence of a misdecode than anything
  // the timing model can offer.
  expect(!shipped.isAllocatedPrefix("QQ1ABC") &&
             !shipped.isAllocatedPrefix("Q1ABC") &&
             !shipped.isAllocatedPrefix("0A1ABC") &&
             !shipped.isAllocatedPrefix("1B2ABC"),
         "the prefix table refuses a callsign whose country does not exist");

  // Longest first, so the most specific allocation applies. Trying the
  // shortest prefix first still admits all four of these, so the refusal
  // assertions above cannot catch it: it names the wrong country instead --
  // Turkiye for San Marino and Palau, Monaco for Eswatini, Spain for Bosnia.
  // That would be a country report an operator could not trust, and the
  // moment anything downstream compares it against a spot it becomes wrong
  // answers rather than merely useless ones.
  expect(shipped.countryFor("T77XX") == "San Marino",
         "T77XX reaches San Marino's T7 block rather than falling through to "
         "Turkiye's TAA-TCZ");
  expect(shipped.countryFor("3DA0AB") == "Eswatini",
         "3DA0AB reaches Eswatini rather than Fiji or Monaco");
  expect(shipped.countryFor("T88AB") == "Palau" &&
             shipped.countryFor("E77AA") == "Bosnia and Herzegovina",
         "a two-character allocation beats the one-character block it sits "
         "inside");

  // The padding, which is the half that would have quietly refused most of
  // the United States. A digit sorts below every letter, so W0ZA taken as W0Z
  // lies outside WAA-WZZ and a three-character comparison refuses it -- along
  // with every G4, K1, I2, F5 and JY1 call on the band.
  expect(shipped.countryFor("W0ZA") == "United States",
         "W0ZA is admitted as the United States, so a prefix shorter than "
         "three characters is padded rather than compared as it stands");
  expect(shipped.countryFor("K1ABC") == "United States" &&
             shipped.countryFor("G4XYZ") == "United Kingdom" &&
             shipped.countryFor("I2ABC") == "Italy" &&
             shipped.countryFor("JY1") == "Jordan",
         "a callsign whose second character is a digit is admitted");

  // Real callsigns arrive decorated. A portable element the table did not
  // understand would refuse exactly the DX an operator most wants to work.
  expect(shipped.countryFor("IU0LFQ/P") == "Italy" &&
             shipped.countryFor("IU0LFQ/QRP") == "Italy",
         "a portable suffix does not hide the callsign in front of it");
  expect(shipped.countryFor("DL/W1AW") == "Germany" &&
             shipped.countryFor("9A/IU0LFQ") == "Croatia",
         "a portable prefix names where the station is operating from, so the "
         "element before the slash wins when it is itself allocated");
  expect(shipped.countryFor("VE7CC-1") == "Canada",
         "a DX cluster node suffix is stripped before matching");
  expect(shipped.isAllocatedPrefix("W1AW/KH6") &&
             shipped.isAllocatedPrefix("DL/W1AW/P"),
         "a callsign carrying both a portable prefix and a suffix is still "
         "admitted");
  expect(shipped.isAllocatedPrefix("/P"),
         "a token whose leading element is not a prefix falls back to the "
         "element after the slash rather than being refused outright");

  // A file an operator edits is a file an operator can mistype. One bad line
  // must cost that line only: the alternative is a single typo refusing every
  // station on the band.
  cwassistant::core::CwCallsignPrefixTable parsed;
  const auto partial = parsed.importText(
      "# comment\n\nBAA BZZ China\nAB CD Two-character bounds\n"
      "NZZ NAA Backwards bounds\nZAA ZZZ\nBAA BZZ Duplicate start\nXAA\n");
  expect(partial.inserted_blocks == 2U && partial.ignored_lines == 3U &&
             partial.duplicate_blocks == 1U,
         "a malformed block line is skipped without taking the rest of the "
         "file with it");
  expect(parsed.countryFor("BY1ABC") == "China",
         "the blocks around a malformed line still load");
  expect(parsed.isAllocatedPrefix("ZS6ABC") &&
             parsed.countryFor("ZS6ABC").empty(),
         "a block that names no country still admits the stations it covers, "
         "because the names decide nothing and dropping the block would not");

  // The safe direction when there is no table at all. Answering no here would
  // refuse every station at once, which is a far larger fault than the
  // invented prefixes this table exists to catch.
  cwassistant::core::CwCallsignPrefixTable unloaded;
  expect(unloaded.isAllocatedPrefix("QQ1ABC") &&
             unloaded.countryFor("QQ1ABC").empty(),
         "a table nothing was imported into admits every callsign rather than "
         "refusing the whole band");
}

void test_cw_callsign_prefixes_survive_missing_files() {
  // Same failure class as the Morse alphabet, with the damage inverted: a
  // packaged build that cannot read the file would answer "no such country"
  // for every callsign, so the compiled-in copy is what keeps a missing file
  // from silently refusing every station heard.
  auto& table = cwassistant::core::cwMutableSharedCallsignPrefixes();
  table.clear();
  const char* previous = std::getenv("CWA_DICTIONARY_DIR");
  const std::string saved = previous == nullptr ? std::string{} : previous;
  setDictionaryDirectoryEnvironment(nullptr);

  const auto& recovered = cwassistant::core::cwSharedCallsignPrefixes();
  expect(recovered.size() >= 308U,
         "the prefix table recovers with no dictionary directory at all");
  expect(recovered.isAllocatedPrefix("IU0LFQ") &&
             !recovered.isAllocatedPrefix("QQ1ABC"),
         "the recovered table still separates a real prefix from an invented "
         "one");
  expect(cwassistant::core::cwCallsignPrefixesLoadedFromBuiltin(),
         "the recovery is reported as coming from the compiled-in copy");

  // The other half of that report. The shipped file and the compiled-in copy
  // are generated from one source and hold identical blocks, so nothing about
  // the contents can tell them apart afterwards and the flag has to be
  // recorded at import time.
  table.clear();
  static_cast<void>(table.importText(readShippedCallsignPrefixes()));
  expect(table.size() == 308U,
         "the shipped prefix file loads through a host-style import");
  expect(!cwassistant::core::cwCallsignPrefixesLoadedFromBuiltin(),
         "a table a caller loaded from file is not reported as built-in");

  if (!saved.empty()) setDictionaryDirectoryEnvironment(saved.c_str());
  table.clear();
  static_cast<void>(cwassistant::core::cwSharedCallsignPrefixes());
}

void test_cw_context_rescorer() {
  expect(loadShippedDictionaries(),
         "the shipped CW dictionaries load without a rejected line");
  expect(cwassistant::core::cwSharedVocabulary().exchangeWordCount() >= 60U,
         "the shipped abbreviation dictionary carries the expected vocabulary");
  // The verification gate stands in for stronger evidence, so it works only
  // while a match stays hard to counterfeit. Guard the property rather than
  // the contents: if this list ever grows toward the whole vocabulary, or
  // admits a single letter, the gate quietly stops meaning anything.
  const auto& shipped = cwassistant::core::cwSharedVocabulary();
  expect(shipped.isDistinctiveToken("CQ") && shipped.isDistinctiveToken("599"),
         "the distinctive-token gate recognises a calling station");
  expect(!shipped.isDistinctiveToken("K") && !shipped.isDistinctiveToken("R") &&
             !shipped.isDistinctiveToken("ES"),
         "the distinctive-token gate rejects tokens noise assembles easily");
  expect(!shipped.isDistinctiveToken("QSO"),
         "the distinctive-token gate is a subset, not the whole vocabulary");

  cwassistant::core::CwVocabulary subset;
  static_cast<void>(subset.importExchangeWords("CQ\nTU\n"));
  const auto refused = subset.importDistinctiveTokens("CQ\nNOTAWORD\nCQ\n");
  expect(refused.inserted_tokens == 1U && refused.duplicate_tokens == 2U,
         "a distinctive token must already be an exchange word");
  using cwassistant::core::CwContextAlternative;
  using cwassistant::core::selectCwContextAlternative;
  const std::array alternatives{
      CwContextAlternative{"CQDESV7BIO", 10.0},
      CwContextAlternative{"CQ DE SV7BIO", 10.18},
      CwContextAlternative{"CQ DE S?7BIO", 10.10},
  };
  const auto selected = selectCwContextAlternative(alternatives, 1.0);
  expect(selected.index == 1,
         "context resolves an acoustically competitive missing word gap");

  const std::array rejected{
      CwContextAlternative{"RAW", 2.0},
      CwContextAlternative{"CQ DE SV7BIO", 3.1},
  };
  expect(selectCwContextAlternative(rejected, 1.0).index == 0,
         "context cannot rescue a path outside the acoustic margin");
  const std::array character_change{
      CwContextAlternative{"CQ DE S?7BIO", 4.0},
      CwContextAlternative{"CQ DE SV7BIO", 4.05},
  };
  expect(selectCwContextAlternative(character_change, 1.0).index == 0,
         "context cannot change decoded characters inside the acoustic margin");
  const std::array unsorted{
      CwContextAlternative{"CQ DE SV7BIO", 5.15},
      CwContextAlternative{"CQDESV7BIO", 5.0},
      CwContextAlternative{"CQ DE SV7BIO",
                           std::numeric_limits<double>::quiet_NaN()},
  };
  expect(selectCwContextAlternative(unsorted, 1.0).index == 0,
         "context handles unsorted input and ignores a non-finite path");
  expect(cwassistant::core::reconstructCwWordGaps("CQDESV7BIO PSEK") ==
             "CQ DE SV7BIO PSE K",
         "known exchange words are separated around a plausible callsign");
  expect(
      cwassistant::core::reconstructCwWordGaps("CQ DE1ABC K") == "CQ DE1ABC K",
      "word-gap repair does not split a genuine DE-prefixed call");
  expect(cwassistant::core::reconstructCwWordGaps("RANDOMTEXT") == "RANDOMTEXT",
         "word-gap repair leaves unconstrained text unchanged");
}

void test_spectrum_settings() {
  cwassistant::core::SpectrumVisualizationSettings settings{
      .target_fps = 500,
      .waterfall_lines_per_second = 0,
      .lower_bound_db = -20.0F,
      .upper_bound_db = -19.0F,
      .averaging_frames = 100,
  };
  const auto safe = settings.sanitized();
  expect(safe.target_fps == 120, "spectrum FPS is bounded");
  expect(safe.waterfall_lines_per_second == 1,
         "waterfall speed is independently bounded");
  expect(safe.upper_bound_db - safe.lower_bound_db >= 10.0F,
         "manual range retains visible span");
  expect(safe.averaging_frames == 32, "averaging is bounded");
}

void test_wav_replay_source() {
  using namespace std::chrono_literals;
  using namespace cwassistant::core;
  const auto path = write_test_wav();
  WavReplaySource source;
  expect(source.open(path.string(), {}), "PCM16 stereo WAV opens for replay");
  expect(source.stream_descriptor().sample_rate_hz == 8'000.0 &&
             source.stream_descriptor().channel_count == 1,
         "WAV replay exposes deterministic mono output metadata");
  expect(source.total_frames() == 5'000 &&
             std::abs(source.duration_seconds() - 0.625) < 1.0e-9,
         "WAV replay reports exact frame count and duration");
  expect(source.start(), "WAV replay starts from frame zero");

  RealtimeSampleBlock first;
  expect(source.read(first, 0ms) && first.sample_count == 4'096 &&
             first.sequence == 0 && first.timestamp_ns == 0,
         "first WAV block has bounded size and deterministic origin");
  expect(std::abs(first.samples[0].real() - 0.375F) < 1.0e-6F,
         "stereo WAV channels downmix to normalized mono");

  RealtimeSampleBlock second;
  expect(source.read(second, 0ms) && second.sample_count == 904 &&
             second.sequence == 1 && second.timestamp_ns == 512'000'000,
         "second WAV block timestamp derives exactly from frame position");
  expect(!source.read(second, 0ms), "WAV replay stops cleanly at end of file");
  expect(source.start() && source.read(first, 0ms) && first.sequence == 0,
         "restarting WAV replay is deterministic");
  source.stop();
  std::error_code removal_error;
  std::filesystem::remove(path, removal_error);
}

void test_wav_writer() {
  using namespace std::chrono_literals;
  using namespace cwassistant::core;
  const auto path =
      std::filesystem::temp_directory_path() / "cwa_wav_writer_test.wav";
  {
    WavWriter writer;
    expect(writer.open(path.string(), 8'000.0),
           "wav writer opens a capture file");
    expect(writer.isOpen(), "wav writer reports open after open()");
    RealtimeSampleBlock block;
    block.stream.sample_rate_hz = 8'000.0;
    block.sample_count = 4;
    block.samples[0] = {0.5F, 0.0F};
    block.samples[1] = {-0.5F, 0.0F};
    block.samples[2] = {1.0F, 0.0F};
    block.samples[3] = {-1.0F, 0.0F};
    expect(writer.writeBlock(block) && writer.framesWritten() == 4,
           "wav writer accepts a sample block and counts written frames");
    writer.close();
    expect(!writer.isOpen(), "wav writer reports closed after close()");
  }

  WavReplaySource reader;
  expect(reader.open(path.string(), {}) &&
             reader.stream_descriptor().sample_rate_hz == 8'000.0 &&
             reader.total_frames() == 4,
         "a captured file round-trips through the WAV reader with exact "
         "sample rate and frame count");
  expect(reader.start(), "round-tripped capture starts playback");
  RealtimeSampleBlock read_block;
  expect(reader.read(read_block, 0ms) && read_block.sample_count == 4,
         "round-tripped capture reports the exact frame count written");
  if (read_block.sample_count == 4) {
    expect(std::abs(read_block.samples[0].real() - 0.5F) < 0.001F &&
               std::abs(read_block.samples[1].real() + 0.5F) < 0.001F &&
               std::abs(read_block.samples[2].real() - 1.0F) < 0.001F &&
               std::abs(read_block.samples[3].real() + 1.0F) < 0.001F,
           "round-tripped capture preserves sample values within PCM16 "
           "quantization precision");
  }
  reader.stop();
  std::error_code removal_error;
  std::filesystem::remove(path, removal_error);
}

void test_spectrum_analyzer() {
  using namespace cwassistant::core;
  constexpr std::size_t fft_size = 1'024;
  constexpr std::size_t tone_bin = 75;
  RealtimeSampleBlock block;
  block.stream = {.kind = StreamKind::Audio,
                  .sample_rate_hz = 48'000.0,
                  .center_frequency_hz = 0.0,
                  .channel_count = 1};
  block.sample_count = fft_size;
  for (std::size_t index = 0; index < fft_size; ++index) {
    const float phase = 2.0F * std::numbers::pi_v<float> *
                        static_cast<float>(tone_bin * index) /
                        static_cast<float>(fft_size);
    block.samples[index] = {std::sin(phase), 0.0F};
  }

  SpectrumAnalyzer analyzer({.fft_size = fft_size, .averaging_frames = 1});
  const auto snapshots = analyzer.process(block);
  expect(snapshots.size() == 1 && snapshots[0].bins_dbfs.size() == 513 &&
             snapshots[0].instantaneous_bins_dbfs.size() == 513,
         "audio FFT emits averaged and instantaneous one-sided bins including "
         "Nyquist");
  const auto peak = static_cast<std::size_t>(
      std::distance(snapshots[0].bins_dbfs.begin(),
                    std::max_element(snapshots[0].bins_dbfs.begin(),
                                     snapshots[0].bins_dbfs.end())));
  expect(peak == tone_bin, "windowed FFT locates a bin-centered CW tone");
  expect(std::abs(snapshots[0].bins_dbfs[peak]) < 0.05F,
         "window coherent-gain normalization reports a full-scale tone near 0 "
         "dBFS");
  expect(
      std::abs(snapshots[0].instantaneous_bins_dbfs[peak] -
               snapshots[0].bins_dbfs[peak]) < 1.0e-6F,
      "one-frame averaging preserves the instantaneous CW-symbol raster bins");
  expect(snapshots[0].lower_frequency_hz == 0.0 &&
             snapshots[0].upper_frequency_hz == 24'000.0 &&
             std::abs(snapshots[0].bin_width_hz - 46.875) < 1.0e-9,
         "audio FFT publishes exact frequency coordinates");

  SpectrumAnalyzer conditioned({.fft_size = fft_size,
                                .averaging_frames = 1,
                                .audio_dc_rejection = true,
                                .audio_automatic_gain = true,
                                .audio_gain_db = 0.0F,
                                .audio_automatic_gain_target_dbfs = -6.0F,
                                .audio_automatic_bandwidth = false,
                                .audio_lower_frequency_hz = 300.0,
                                .audio_upper_frequency_hz = 3'000.0});
  constexpr std::size_t conditioned_tone_bin = 16;
  for (std::size_t index = 0; index < fft_size; ++index) {
    const float phase = 2.0F * std::numbers::pi_v<float> *
                        static_cast<float>(conditioned_tone_bin * index) /
                        static_cast<float>(fft_size);
    block.samples[index] = {0.25F + 0.25F * std::sin(phase), 0.0F};
  }
  const auto conditioned_snapshots = conditioned.process(block);
  expect(conditioned_snapshots.size() == 1 &&
             conditioned_snapshots[0].bins_dbfs.size() == 58 &&
             conditioned_snapshots[0].lower_frequency_hz == 328.125 &&
             conditioned_snapshots[0].upper_frequency_hz == 3'000.0,
         "manual audio bandwidth crops bins and reports exact displayed "
         "coordinates");
  const auto conditioned_peak =
      std::max_element(conditioned_snapshots[0].bins_dbfs.begin(),
                       conditioned_snapshots[0].bins_dbfs.end());
  expect(conditioned_peak != conditioned_snapshots[0].bins_dbfs.end() &&
             std::abs(*conditioned_peak + 6.0F) < 0.1F,
         "automatic input gain reaches its configured dBFS target after DC "
         "rejection");

  SpectrumAnalyzer dc_rejected({.fft_size = fft_size,
                                .averaging_frames = 1,
                                .audio_dc_rejection = true});
  const auto dc_rejected_snapshots = dc_rejected.process(block);
  expect(dc_rejected_snapshots.size() == 1 &&
             dc_rejected_snapshots[0].bins_dbfs.front() < -100.0F,
         "audio DC rejection removes a constant left-edge spectral peak");

  SpectrumAnalyzer automatic_bandwidth(
      {.fft_size = fft_size,
       .averaging_frames = 1,
       .audio_dc_rejection = true,
       .audio_automatic_gain = false,
       .audio_gain_db = 0.0F,
       .audio_automatic_gain_target_dbfs = -12.0F,
       .audio_automatic_bandwidth = true});
  const auto automatic_snapshots = automatic_bandwidth.process(block);
  expect(
      automatic_snapshots.size() == 1 &&
          automatic_snapshots[0].lower_frequency_hz == 140.625 &&
          automatic_snapshots[0].upper_frequency_hz == 3'000.0,
      "automatic audio bandwidth derives a CW-oriented view from sample rate");

  expect(!analyzer.configure({.fft_size = 1'000, .averaging_frames = 1}),
         "spectrum analyzer rejects a non-radix-two transform");

  RealtimeSampleBlock overlap_block = block;
  overlap_block.sample_count = 2'048;
  for (std::size_t index = 0; index < overlap_block.sample_count; ++index) {
    const float phase = 2.0F * std::numbers::pi_v<float> *
                        static_cast<float>(tone_bin * index) /
                        static_cast<float>(fft_size);
    overlap_block.samples[index] = {std::sin(phase), 0.0F};
  }
  SpectrumAnalyzer overlapping(
      {.fft_size = fft_size, .averaging_frames = 1, .frame_rate_hz = 120});
  const auto overlap_snapshots = overlapping.process(overlap_block);
  expect(overlap_snapshots.size() == 3 &&
             overlap_snapshots[1].timestamp_ns == 8'333'333,
         "overlapping FFT hops add genuine high-rate waterfall timing frames");
  overlap_block.sample_count = fft_size;
  overlap_block.timestamp_ns = 2'000'000'000;
  const auto after_gap = overlapping.process(overlap_block);
  expect(after_gap.size() == 1 &&
             after_gap.front().timestamp_ns == overlap_block.timestamp_ns,
         "analysis resets overlap at a capture gap so waterfall time is not "
         "compressed");

  SpectrumAnalyzer wide_rate_limited(
      {.fft_size = 1'024, .averaging_frames = 1, .frame_rate_hz = 60});
  RealtimeSampleBlock wide_block;
  wide_block.stream = {.kind = StreamKind::ComplexIq,
                       .sample_rate_hz = 240'000.0,
                       .center_frequency_hz = 14'050'000.0,
                       .channel_count = 2};
  wide_block.sample_count = wide_block.samples.size();
  for (std::size_t index = 0; index < wide_block.sample_count; ++index) {
    const float phase = 2.0F * std::numbers::pi_v<float> * 2'000.0F *
                        static_cast<float>(index) / 240'000.0F;
    wide_block.samples[index] = {std::cos(phase), std::sin(phase)};
  }
  const auto limited_snapshots = wide_rate_limited.process(wide_block);
  expect(limited_snapshots.size() == 1,
         "wide-IQ FFT skips complete input intervals to honor its configured "
         "frame rate");
  expect(limited_snapshots.size() < wide_block.sample_count / 1'024,
         "wide-IQ FFT does not emit one frame per transform length at high "
         "sample rates");

  SpectrumAnalyzer multi_mhz_limited(
      {.fft_size = 1'024, .averaging_frames = 1, .frame_rate_hz = 60});
  wide_block.stream.sample_rate_hz = 8'000'000.0;
  std::size_t multi_mhz_frames = 0;
  std::uint64_t sample_offset = 0;
  std::uint64_t last_frame_timestamp = 0;
  for (std::uint64_t sequence = 0; sequence < 100; ++sequence) {
    wide_block.sequence = sequence;
    wide_block.timestamp_ns = static_cast<std::uint64_t>(
        static_cast<long double>(sample_offset) * 1'000'000'000.0L /
        wide_block.stream.sample_rate_hz);
    const auto produced = multi_mhz_limited.process(wide_block);
    for (const auto& snapshot : produced) {
      if (multi_mhz_frames > 0) {
        expect(snapshot.timestamp_ns > last_frame_timestamp + 16'600'000 &&
                   snapshot.timestamp_ns < last_frame_timestamp + 16'800'000,
               "multi-MHz overview frames retain the configured wall-clock "
               "cadence");
      }
      last_frame_timestamp = snapshot.timestamp_ns;
      ++multi_mhz_frames;
    }
    sample_offset += wide_block.sample_count;
  }
  expect(multi_mhz_frames == 4,
         "an 8 MHz overview emits about 60 frames per "
         "second instead of one per FFT");

  SpectrumAnalyzer discontinuous_average(
      {.fft_size = 1'024, .averaging_frames = 4, .frame_rate_hz = 60});
  wide_block.stream.sample_rate_hz = 240'000.0;
  wide_block.timestamp_ns = 0;
  wide_block.sequence = 0;
  std::fill_n(wide_block.samples.begin(), wide_block.sample_count,
              std::complex<float>{1.0F, 0.0F});
  static_cast<void>(discontinuous_average.process(wide_block));
  wide_block.timestamp_ns = 1'000'000'000;
  wide_block.sequence = 2;
  std::fill_n(wide_block.samples.begin(), wide_block.sample_count,
              std::complex<float>{0.0F, 0.0F});
  const auto after_discontinuity = discontinuous_average.process(wide_block);
  expect(!after_discontinuity.empty() &&
             after_discontinuity.front().bins_dbfs ==
                 after_discontinuity.front().instantaneous_bins_dbfs,
         "a capture discontinuity clears spectral averaging instead of "
         "ghosting old RF");
}

void test_remote_control_lease() {
  using namespace std::chrono_literals;
  using cwassistant::core::ControlLeaseManager;
  ControlLeaseManager leases;
  const auto start = ControlLeaseManager::TimePoint{};

  expect(leases.acquire("client-a", "rig-1", start, 10s),
         "first remote client acquires rig lease");
  expect(!leases.acquire("client-b", "rig-1", start, 10s),
         "second client cannot steal active rig lease");
  expect(leases.acquire("client-b", "rig-2", start, 10s),
         "different rigs have independent leases");
  expect(leases.owns("client-a", "rig-1", start + 1s),
         "lease owner is recognized");
  expect(leases.renew("client-a", "rig-1", start + 1s, 20s),
         "lease owner can renew heartbeat");
  expect(leases.owns("client-a", "rig-1", start + 15s),
         "renewed lease remains active");
  expect(!leases.owns("client-a", "rig-1", start + 22s),
         "lease expires without heartbeat");
  expect(leases.acquire("client-b", "rig-1", start + 22s, 1ms),
         "new client can acquire expired lease");
  expect(leases.owns("client-b", "rig-1", start + 23s),
         "minimum TTL clamp prevents unsafe instant expiry");
  expect(!leases.release("client-a", "rig-1"),
         "non-owner cannot release another lease");
  expect(leases.release("client-b", "rig-1"),
         "owner can explicitly release lease");
}

void test_adif() {
  cwassistant::core::QsoRecord qso{};
  qso.callsign = "I1ABC";
  qso.qso_date = "20260830";
  qso.time_on = "143512";
  qso.band = "20M";
  qso.mode = "CW";
  qso.frequency_mhz = "14.025000";
  qso.rst_sent = "599";
  qso.rst_received = "579";
  qso.station_callsign = "IU0XYZ";
  const auto adif = cwassistant::core::to_adif(qso);
  expect(adif.find("<CALL:5>I1ABC") != std::string::npos,
         "ADIF encodes field length");
  expect(adif.ends_with("<EOR>"), "ADIF terminates the record");
}

void test_split_transverter_and_satellite_adif() {
  using namespace cwassistant::core;
  const VfoFrequencyPlan plan{
      .rx_dial_hz = 29'900'000,
      .tx_dial_hz = 28'300'000,
      .split_enabled = true,
  };
  const TransverterOffsets offsets{
      .rx_offset_hz = 116'000'000,
      .tx_offset_hz = 407'000'000,
  };
  const auto resolved = resolve_frequencies(plan, offsets);
  expect(resolved.has_value(), "split transverter frequencies resolve");
  expect(resolved && resolved->rx_rf_hz == 145'900'000,
         "positive RX transverter offset produces actual downlink RF");
  expect(resolved && resolved->tx_rf_hz == 435'300'000,
         "independent positive TX offset produces actual uplink RF");

  QsoRecord qso{};
  qso.callsign = "I1ABC";
  qso.qso_date = "20260830";
  qso.time_on = "143512";
  qso.mode = "CW";
  qso.rst_sent = "599";
  qso.rst_received = "579";
  qso.station_callsign = "IU0XYZ";
  const SatelliteQsoDetails satellite{.name = "AO-7", .mode = "U/V"};
  expect(populate_qso_frequencies(qso, plan, offsets, &satellite),
         "satellite QSO receives calculated RF fields");
  expect(qso.band == "70CM" && qso.band_rx == "2M",
         "ADIF TX and RX bands derive from actual RF frequencies");
  expect(
      qso.frequency_mhz == "435.300000" && qso.frequency_rx_mhz == "145.900000",
      "ADIF keeps exact TX and RX frequencies to one hertz");

  const auto adif = to_adif(qso);
  expect(adif.find("<BAND:4>70CM") != std::string::npos,
         "ADIF exports transmit band");
  expect(adif.find("<BAND_RX:2>2M") != std::string::npos,
         "ADIF exports receive band");
  expect(adif.find("<FREQ:10>435.300000") != std::string::npos,
         "ADIF exports actual transmit frequency");
  expect(adif.find("<FREQ_RX:10>145.900000") != std::string::npos,
         "ADIF exports actual receive frequency");
  expect(adif.find("<PROP_MODE:3>SAT") != std::string::npos &&
             adif.find("<SAT_NAME:4>AO-7") != std::string::npos &&
             adif.find("<SAT_MODE:3>U/V") != std::string::npos,
         "ADIF exports satellite propagation, name, and mode");
}

void test_negative_transverter_offset_and_invalid_frequency() {
  using namespace cwassistant::core;
  const auto resolved = resolve_frequencies(
      {.rx_dial_hz = 145'900'000, .split_enabled = false},
      {.rx_offset_hz = -116'000'000, .tx_offset_hz = -116'000'000});
  expect(resolved && resolved->rx_rf_hz == 29'900'000 &&
             resolved->tx_rf_hz == 29'900'000,
         "negative transverter offsets are supported for RX and TX");
  expect(adif_band_from_frequency(29'900'000).empty(),
         "out-of-band frequency is not mislabeled in ADIF");
  expect(
      !resolve_frequencies({.rx_dial_hz = 10'000'000, .split_enabled = false},
                           {.rx_offset_hz = -10'000'000, .tx_offset_hz = 0}),
      "offset calculation rejects zero or underflowed actual RF");
  expect(resolve_audio_tone_rf(14'074'700, 725.0, 700.0, true) ==
             std::optional<std::uint64_t>(14'074'725),
         "CW-U audio offset maps upward from the actual-RF reference");
  expect(resolve_audio_tone_rf(14'074'700, 725.0, 700.0, false) ==
             std::optional<std::uint64_t>(14'074'675),
         "CW-L audio offset maps downward from the actual-RF reference");
  expect(!resolve_audio_tone_rf(10, 800.0, 700.0, false),
         "audio-to-RF mapping rejects an underflow below zero hertz");
  expect(resolve_dial_frequency(145'900'000, 116'000'000) ==
             std::optional<std::uint64_t>(29'900'000),
         "actual RF is converted back to a positive-offset radio dial value");
  expect(resolve_dial_frequency(14'000'000, -116'000'000) ==
             std::optional<std::uint64_t>(130'000'000),
         "actual RF is converted back through a negative transverter offset");
  expect(!resolve_dial_frequency(116'000'000, 116'000'000),
         "inverse offset rejects a zero dial frequency");
  expect(!resolve_dial_frequency(std::numeric_limits<std::uint64_t>::max(), -1),
         "inverse offset rejects unsigned overflow");
  expect(resolve_dial_frequency(1, std::numeric_limits<std::int64_t>::min()) ==
                 std::optional<std::uint64_t>(9'223'372'036'854'775'809ULL) &&
             resolve_dial_frequency(9'223'372'036'854'775'808ULL,
                                    std::numeric_limits<std::int64_t>::max()) ==
                 std::optional<std::uint64_t>(1),
         "inverse offset handles both signed limits without overflow");
  expect(parse_frequency_value("14040.49", 1'000) ==
                 std::optional<std::uint64_t>(14'040'490) &&
             parse_frequency_value(" 7010,5 ", 1'000) ==
                 std::optional<std::uint64_t>(7'010'500) &&
             parse_frequency_value("1", 1'000) ==
                 std::optional<std::uint64_t>(1'000),
         "operator kHz entry accepts exact dot/comma fractional values");
  expect(parse_frequency_value("144.300025", 1'000'000) ==
             std::optional<std::uint64_t>(144'300'025),
         "operator MHz entry preserves exact integer-hertz precision");
  expect(!parse_frequency_value("14,040.5", 1'000) &&
             !parse_frequency_value("14040.1234", 1'000) &&
             !parse_frequency_value("0", 1'000) &&
             !parse_frequency_value("14 MHz", 1'000) &&
             !parse_frequency_value("14.1", 60),
         "operator kHz entry rejects grouping, excessive precision, zero, and "
         "units");

  using cwassistant::core::omni_rig_conventional_vfo_target;
  // A radio that publishes no VFO identity still has VFOs.
  //
  // Every VFO role was derived from OmniRig's `Vfo` parameter, so a radio whose
  // profile omits it was treated as having no transmit VFO at all and pointing
  // the transmit frequency was refused -- even where the parameter mask said
  // plainly that FreqB was writable. The FT-450D is such a radio and has FA and
  // FB in its CAT set, so the control it demonstrably has was unreachable.
  {
    constexpr std::uint32_t kWritableBoth = 0x04U | 0x08U;
    expect(omni_rig_conventional_vfo_target(false, true, true, kWritableBoth) ==
                   OmniRigRxFrequencyTarget::FrequencyB &&
               omni_rig_conventional_vfo_target(false, true, false,
                                                kWritableBoth) ==
                   OmniRigRxFrequencyTarget::FrequencyA,
           "with split on and no VFO identity published, A receives and B "
           "transmits -- the convention every transceiver shares");
    expect(omni_rig_conventional_vfo_target(true, true, true, kWritableBoth) ==
               OmniRigRxFrequencyTarget::None,
           "a radio that names its VFOs is read from what it says, never from "
           "a convention");
    expect(omni_rig_conventional_vfo_target(false, false, true,
                                            kWritableBoth) ==
               OmniRigRxFrequencyTarget::None,
           "simplex has one VFO and so no roles to assign");
    // The guard that keeps this a reading of the radio rather than a guess
    // about it: a capability is never claimed that the radio has not itself
    // published as writable.
    expect(omni_rig_conventional_vfo_target(false, true, true, 0x04U) ==
                   OmniRigRxFrequencyTarget::None &&
               omni_rig_conventional_vfo_target(false, true, false, 0x08U) ==
                   OmniRigRxFrequencyTarget::None,
           "the convention is claimed only where the parameter mask agrees the "
           "property can be written");
  }

  using cwassistant::core::omni_rig_active_vfo_is_receive_frequency;
  // OmniRig's `Freq` is the SELECTED VFO, not the receive VFO.
  //
  // A rig that publishes FreqA and FreqB never needs it. Rigs that publish
  // neither -- the FT-450D among them -- had their receive frequency read from
  // `Freq` unconditionally, so with split on, selecting the transmit VFO to
  // set it dragged the receive frequency along: moving VFO B moved VFO A, and
  // with it the RF axis, the spot band filter and the decoder's frequency
  // mapping. The reading is trustworthy exactly when one VFO is in play.
  expect(!omni_rig_active_vfo_is_receive_frequency(true, false) &&
             !omni_rig_active_vfo_is_receive_frequency(true, true),
         "a radio naming its VFOs is read per VFO and never from the selected "
         "one");
  expect(omni_rig_active_vfo_is_receive_frequency(false, false),
         "in simplex the selected VFO is the receive VFO, so the active "
         "frequency is the receive frequency");
  expect(!omni_rig_active_vfo_is_receive_frequency(false, true),
         "with split enabled and no way to tell which VFO is selected, no "
         "reading beats one that is wrong exactly when the operator is "
         "working the transmit VFO");

  using Target = OmniRigRxFrequencyTarget;
  expect(select_omnirig_rx_frequency_target(true, true, 0x04, 0x80) ==
                 Target::FrequencyA &&
             select_omnirig_rx_frequency_target(true, true, 0x04, 0x100) ==
                 Target::FrequencyA &&
             select_omnirig_rx_frequency_target(true, true, 0x04, 0x800) ==
                 Target::FrequencyA,
         "OmniRig A/AA/AB receive states select a writable FreqA property");
  expect(select_omnirig_rx_frequency_target(true, true, 0x08, 0x200) ==
                 Target::FrequencyB &&
             select_omnirig_rx_frequency_target(true, true, 0x08, 0x400) ==
                 Target::FrequencyB &&
             select_omnirig_rx_frequency_target(true, true, 0x08, 0x1000) ==
                 Target::FrequencyB,
         "OmniRig B/BA/BB receive states select a writable FreqB property");
  expect(select_omnirig_rx_frequency_target(true, true, 0x02, 0x80) ==
                 Target::Frequency &&
             select_omnirig_rx_frequency_target(false, true, 0x0e, 0x80) ==
                 Target::None &&
             select_omnirig_rx_frequency_target(true, false, 0x0e, 0x80) ==
                 Target::None &&
             select_omnirig_rx_frequency_target(true, true, 0, 0x80) ==
                 Target::None,
         "OmniRig uses generic Freq only when writable and blocks offline, TX, "
         "and read-only states");

  const auto first_step = step_rx_frequency(14'040'000, std::nullopt, 1'000, 1);
  const auto second_step = step_rx_frequency(14'040'000, first_step, 1'000, 1);
  expect(first_step == std::optional<std::uint64_t>(14'041'000) &&
             second_step == std::optional<std::uint64_t>(14'042'000),
         "accepted RX steps accumulate while provider readback is pending");
  expect(!step_rx_frequency(500, std::nullopt, 1'000, -1) &&
             !step_rx_frequency(14'040'000, std::nullopt, 1'000, 0),
         "RX stepping rejects underflow and invalid direction");
}

void test_band_selected_station_equipment_adif() {
  using namespace cwassistant::core;
  const std::vector<StationEquipmentRule> rules{
      {
          .bands = {"6M", "10M", "12M", "15M", "17M", "20M"},
          .equipment =
              {
                  .radio = "Yaesu FT-450D",
                  .transverter = {},
                  .antenna = "Dipole",
              },
      },
      {
          .bands = {"13CM"},
          .equipment =
              {
                  .radio = "Microwave IF radio",
                  .transverter = "DXPatrol Transverter",
                  .antenna = "Offset parabolic dish",
              },
      },
  };
  const ResolvedFrequencies hf{
      .rx_rf_hz = 14'025'000,
      .tx_rf_hz = 14'025'000,
  };
  const auto hf_equipment = resolve_station_equipment(hf, rules);
  expect(hf_equipment && describe_station_rig(*hf_equipment) == "Yaesu FT-450D",
         "HF band rule selects its configured radio");
  expect(hf_equipment && describe_station_antenna(*hf_equipment) == "Dipole",
         "HF band rule selects its configured antenna");

  const ResolvedFrequencies cross_band{
      .rx_rf_hz = 14'025'000,
      .tx_rf_hz = 2'320'100'000,
      .split_enabled = true,
  };
  QsoRecord qso;
  expect(populate_qso_station_equipment(qso, cross_band, rules),
         "cross-band equipment chains resolve from actual RF bands");
  expect(qso.station_rig ==
             "TX: Microwave IF radio + DXPatrol Transverter; RX: Yaesu FT-450D",
         "different TX/RX radio chains are explicit");
  expect(qso.station_antenna == "TX: Offset parabolic dish; RX: Dipole",
         "different TX/RX antennas are explicit");
  const auto adif = to_adif(qso);
  expect(adif.find("<MY_RIG:") != std::string::npos &&
             adif.find("<MY_ANTENNA:") != std::string::npos,
         "ADIF exports logging-station rig and antenna fields");
}

void test_reference_rig_profiles() {
  using namespace cwassistant::core;
  expect(kSupportedSerialBaudRates ==
             std::array<std::uint32_t, 8>{1'200, 2'400, 4'800, 9'600, 19'200,
                                          38'400, 57'600, 115'200},
         "direct serial CAT exposes only conventional supported baud rates");
  expect(is_supported_serial_baud_rate(57'600) &&
             !is_supported_serial_baud_rate(57'601),
         "serial baud validation rejects one-unit arbitrary values");
  expect(nearest_supported_serial_baud_rate(57'601) == 57'600 &&
             nearest_supported_serial_baud_rate(200'000) == 115'200,
         "legacy arbitrary baud values migrate to the nearest supported rate");
  const auto profiles = reference_rig_profiles();
  expect(profiles.size() == 2, "two Yaesu reference profiles are available");

  const auto* ft450d = find_reference_rig_profile("yaesu-ft-450d");
  expect(ft450d != nullptr, "FT-450D profile is selectable");
  expect(ft450d != nullptr && ft450d->cat.baud_rate == 4'800 &&
             ft450d->cat.data_bits == 8 && ft450d->cat.stop_bits == 1,
         "FT-450D starts with documented 4800 8-N-1 CAT framing");
  expect(ft450d != nullptr && ft450d->omnirig_rig_type == "FT-450",
         "FT-450D maps to the OmniRig FT-450 command description");

  const auto* ft818 = find_reference_rig_profile("yaesu-ft-818");
  expect(ft818 != nullptr, "FT-818 profile is selectable");
  expect(ft818 != nullptr && ft818->cat.baud_rate == 4'800 &&
             ft818->cat.data_bits == 8 && ft818->cat.stop_bits == 2,
         "FT-818 starts with documented 4800 8-N-2 CAT framing");
  expect(ft818 != nullptr && ft818->omnirig_rig_type == "FT-817",
         "FT-818 uses the compatible OmniRig FT-817 command description");

  expect(ft450d != nullptr && ft450d->cat.port.empty() &&
             ft450d->keying.port.empty(),
         "reference profiles never guess physical COM ports");
  expect(ft450d != nullptr && ft450d->ptt_line != ft450d->key_line,
         "direct keying defaults PTT and KEY to different lines");
}

void test_cat4om_protocol_contract() {
  using namespace cwassistant::core;
  expect(cat4om_protocol_compatible("1.0.0") &&
             cat4om_protocol_compatible("1.99.3"),
         "CAT4OM accepts additive changes within protocol major 1");
  expect(!cat4om_protocol_compatible("2.0.0") &&
             !cat4om_protocol_compatible("invalid"),
         "CAT4OM rejects incompatible or malformed protocol versions");
  expect(cat4om_role_from_string("master") == Cat4OmRole::Master &&
             cat4om_role_from_string("new-role") == Cat4OmRole::Unknown,
         "CAT4OM roles degrade safely when a future value is unknown");

  const Cat4OmRadioState simplex{
      .radio_id = "run",
      .connection_status = "connected",
      .active_vfo = "MAIN",
      .tx_vfo = "SUB",
      .split = false,
      .vfos = {{.id = "MAIN", .frequency_hz = 14'025'000, .mode = "USB"},
               {.id = "SUB", .frequency_hz = 7'010'000, .mode = "CW"}},
      .available_commands = {"SetFrequency", "SetMode", "SetSplit"},
  };
  const auto simplex_plan = cat4om_frequency_plan(simplex);
  expect(simplex_plan && simplex_plan->rx_dial_hz == 14'025'000 &&
             simplex_plan->tx_dial_hz == 14'025'000 &&
             !simplex_plan->split_enabled,
         "CAT4OM simplex state uses the active VFO for RX and TX");
  expect(cat4om_has_command(simplex, "setfrequency"),
         "CAT4OM command capability matching tolerates case only");
  const auto simplex_radio = cat4om_radio_state(simplex, true);
  expect(simplex_radio.rx_mode.mode == RadioMode::UpperSideband &&
             simplex_radio.tx_mode.mode == RadioMode::Cw &&
             simplex_radio.tx_frequency.hz == 7'010'000 &&
             simplex_radio.tx_vfo.identifier == "SUB" &&
             simplex_radio.split.split == RadioSplit::Disabled,
         "CAT4OM keeps the standby TX VFO independent in simplex state");

  auto split = simplex;
  split.split = true;
  const auto split_plan = cat4om_frequency_plan(split);
  expect(split_plan && split_plan->rx_dial_hz == 14'025'000 &&
             split_plan->tx_dial_hz == 7'010'000 && split_plan->split_enabled,
         "CAT4OM split state preserves independent opaque VFO names");
  const auto split_radio = cat4om_radio_state(split, true);
  expect(split_radio.tx_frequency.hz == 7'010'000 &&
             split_radio.tx_mode.mode == RadioMode::Cw &&
             radio_has_capability(split_radio.capabilities,
                                  RadioCapability::SetTxFrequency),
         "CAT4OM preserves independent TX VFO frequency and mode");
  split.tx_vfo = "missing";
  expect(!cat4om_frequency_plan(split),
         "CAT4OM refuses an incomplete split frequency snapshot");
}

}  // namespace

// Display settings must never change decoding. Spectrum averaging is a
// presentation control, and a display line rate that is a multiple of the
// detector's own cadence must supply the detector with the same evidence.
// Both were operator-visible defects: raising averaging changed candidate
// churn by an order of magnitude, and changing the line rate could change the
// callsign shown for a signal.
void test_decoder_display_setting_invariance() {
  using namespace cwassistant::core;

  constexpr double sample_rate = 48'000.0;
  constexpr double tone_hz = 700.0;
  constexpr double dot_seconds = 0.06;  // 20 WPM
  std::vector<float> audio;
  audio.reserve(static_cast<std::size_t>(sample_rate * 8.0));
  double phase = 0.0;
  std::uint32_t noise_state = 0x1234'5678U;
  const auto emit = [&](const double seconds, const bool keyed) {
    const auto count = static_cast<std::size_t>(seconds * sample_rate);
    for (std::size_t index = 0; index < count; ++index) {
      noise_state = noise_state * 1'103'515'245U + 12'345U;
      const float noise =
          0.002F *
          (static_cast<float>((noise_state >> 16U) & 0x7FFFU) / 16'384.0F -
           1.0F);
      phase += 2.0 * std::numbers::pi * tone_hz / sample_rate;
      audio.push_back(
          keyed ? 0.20F * static_cast<float>(std::sin(phase)) + noise : noise);
    }
  };
  const auto send = [&](const std::string_view elements) {
    for (std::size_t index = 0; index < elements.size(); ++index) {
      emit(elements[index] == '-' ? 3.0 * dot_seconds : dot_seconds, true);
      emit(dot_seconds, false);
    }
    emit(2.0 * dot_seconds, false);
  };
  emit(0.3, false);
  for (int repeat = 0; repeat < 6; ++repeat) {
    send("...");  // S
    send("---");  // O
    send("...");  // S
    emit(4.0 * dot_seconds, false);
  }

  struct Outcome {
    std::string text;
    std::size_t published{0};
    std::uint64_t verified{0};
  };
  const auto replay = [&](const int averaging_frames, const int frame_rate_hz) {
    SpectrumAnalyzer analyzer(
        {.averaging_frames = static_cast<std::uint8_t>(averaging_frames),
         .frame_rate_hz = static_cast<std::uint16_t>(frame_rate_hz),
         .audio_upper_frequency_hz = 3'000.0});
    CwChannelBank bank;
    RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    Outcome outcome;
    std::vector<std::uint64_t> ids;
    std::size_t position = 0;
    std::uint64_t now = 0;
    while (position < audio.size()) {
      const std::size_t take =
          std::min<std::size_t>(1'024, audio.size() - position);
      block.sample_count = take;
      block.timestamp_ns = now;
      for (std::size_t index = 0; index < take; ++index)
        block.samples[index] = {audio[position + index], 0.0F};
      for (const auto& snapshot : analyzer.process(block)) {
        // Production supplies the unaveraged bins; the detector owns its own
        // smoothing so display averaging cannot reach it.
        static_cast<void>(bank.updateSpectrum(
            snapshot.timestamp_ns, snapshot.lower_frequency_hz,
            snapshot.upper_frequency_hz, snapshot.instantaneous_bins_dbfs));
      }
      for (const auto& channel : bank.processSamples(block)) {
        if (std::find(ids.begin(), ids.end(), channel.id) == ids.end())
          ids.push_back(channel.id);
        if (channel.text.size() > outcome.text.size())
          outcome.text = channel.text;
      }
      position += take;
      now += static_cast<std::uint64_t>(static_cast<long double>(take) *
                                        1'000'000'000.0L / sample_rate);
    }
    outcome.published = ids.size();
    outcome.verified = bank.verificationDiagnostics().verified_transitions;
    return outcome;
  };

  const Outcome reference = replay(3, 60);
  expect(!reference.text.empty() && reference.published > 0,
         "display-invariance fixture decodes a signal at the reference "
         "averaging and line rate");

  for (const int averaging_frames : {1, 8, 32}) {
    const Outcome measured = replay(averaging_frames, 60);
    expect(measured.text == reference.text &&
               measured.published == reference.published &&
               measured.verified == reference.verified,
           "spectrum averaging is a display control and cannot change the "
           "decoded result");
  }

  const Outcome doubled_rate = replay(3, 120);
  expect(doubled_rate.text == reference.text &&
             doubled_rate.published == reference.published &&
             doubled_rate.verified == reference.verified,
         "a display line rate above the detector cadence supplies the same "
         "evidence and cannot change the decoded result");
}

void test_soft_decision_keying_evidence() {
  using namespace cwassistant::core;

  constexpr double sample_rate = 48'000.0;
  constexpr double tone_hz = 700.0;
  // The keying decision is a likelihood ratio between a mark level and a space
  // level, each carrying its own measured scatter. Unequal scatter moves the
  // decision off half amplitude, which is wanted: a mark carries signal plus
  // noise and a space carries noise alone, so the boundary sits nearer the
  // mark and noise excursions stop producing marks. It may only ever move that
  // way. A keying edge sweeps through both levels and can transiently inflate
  // the space estimate past the mark's; if that is allowed to stand, the
  // boundary shifts the wrong way and every element is mistimed. On a signal
  // with no noise at all -- where the true scatter of both levels is nil and
  // the transient is the only thing either estimate ever sees -- that failure
  // is total, and the decoder emits nothing at all.
  const auto decode = [&](const double dot_seconds,
                          const float noise_amplitude) {
    std::vector<float> audio;
    audio.reserve(static_cast<std::size_t>(sample_rate * 12.0));
    double phase = 0.0;
    std::uint32_t noise_state = 0x2468'ACE0U;
    const auto emit = [&](const double seconds, const bool keyed) {
      const auto count = static_cast<std::size_t>(seconds * sample_rate);
      for (std::size_t index = 0; index < count; ++index) {
        noise_state = noise_state * 1'103'515'245U + 12'345U;
        const float noise =
            noise_amplitude *
            (static_cast<float>((noise_state >> 16U) & 0x7FFFU) / 16'384.0F -
             1.0F);
        phase += 2.0 * std::numbers::pi * tone_hz / sample_rate;
        audio.push_back(keyed ? 0.20F * static_cast<float>(std::sin(phase)) +
                                    noise
                              : noise);
      }
    };
    const auto send = [&](const std::string_view elements) {
      for (std::size_t index = 0; index < elements.size(); ++index) {
        emit(elements[index] == '-' ? 3.0 * dot_seconds : dot_seconds, true);
        emit(dot_seconds, false);
      }
      emit(2.0 * dot_seconds, false);
    };
    emit(0.3, false);
    for (int repeat = 0; repeat < 8; ++repeat) {
      send("...");  // S
      send("---");  // O
      send("...");  // S
      emit(4.0 * dot_seconds, false);
    }

    SpectrumAnalyzer analyzer({.audio_upper_frequency_hz = 3'000.0});
    CwChannelBank bank;
    RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    std::string text;
    std::size_t position = 0;
    std::uint64_t now = 0;
    while (position < audio.size()) {
      const std::size_t take =
          std::min<std::size_t>(1'024, audio.size() - position);
      block.sample_count = take;
      block.timestamp_ns = now;
      for (std::size_t index = 0; index < take; ++index)
        block.samples[index] = {audio[position + index], 0.0F};
      for (const auto& snapshot : analyzer.process(block)) {
        static_cast<void>(bank.updateSpectrum(
            snapshot.timestamp_ns, snapshot.lower_frequency_hz,
            snapshot.upper_frequency_hz, snapshot.instantaneous_bins_dbfs));
      }
      for (const auto& channel : bank.processSamples(block)) {
        if (!channel.text.empty()) text = channel.text;
      }
      position += take;
      now += static_cast<std::uint64_t>(static_cast<long double>(take) *
                                        1'000'000'000.0L / sample_rate);
    }
    return text;
  };

  const std::string clean = decode(0.06, 0.0F);  // 20 WPM, no noise
  expect(clean.find("SOS") != std::string::npos,
         "a perfectly keyed signal carrying no noise decodes");

  // Fast keying with receiver noise present is where the ordering matters
  // most: the elements are short enough that edge samples are a large share
  // of every run, so an unconstrained space estimate overtakes the mark's and
  // the boundary shifts away from the mark. Every element then runs together
  // and the message collapses into a string of single marks.
  const std::string fast = decode(0.03, 0.002F);  // 40 WPM with noise
  expect(fast.find("SOS") != std::string::npos,
         "fast keying survives, so a keying edge cannot invert the two "
         "levels' scatter and shift the decision away from the mark");
}

// Decoding must not get more expensive the longer a station has been left
// running. An operator does not restart the application between overs, so a
// per-block cost that grows with the transcript turns a session that started
// responsive into one that has to be killed -- which is exactly what was
// reported, and exactly what a processor meter fails to show, because the
// growth is on one thread while the machine as a whole stays idle.
//
// This measures the shape of the cost rather than its size: the median block
// of the last fifth of a run against the median block of the first fifth, on
// the same machine, in the same process. A ratio cannot be made to fail by a
// slow or a busy builder, which an absolute time bound would be.
//
// Against the reconstruction being redone from scratch on every block, the
// median rises about sixteenfold across two minutes of decoding. With it
// remembered, it is flat -- about 1.3. Four is far above the one and far
// below the other.
void test_decoding_cost_does_not_grow_with_session_length() {
  using namespace cwassistant::core;
  using Clock = std::chrono::steady_clock;
  constexpr double sample_rate = 8'000.0;
  constexpr std::size_t bin_count = 2'048;
  constexpr double tone_hz = 700.0;
  constexpr int blocks = 12'000;  // 10 ms each: two minutes of decoding.

  CwChannelBank bank({.empty_track_retention_seconds = 6.0,
                      .decoded_track_retention_seconds = 45.0,
                      .detector_frame_interval_seconds = 0.0,
                      .minimum_spectral_observations = 1,
                      .minimum_verification_symbols = 0,
                      .verification_enter_seconds = 0.0,
                      .verification_exit_seconds = 0.0});
  std::vector<float> spectrum(bin_count, -110.0F);
  std::vector<double> costs_us;
  costs_us.reserve(static_cast<std::size_t>(blocks));
  std::uint64_t now = 0;
  double phase = 0.0;
  bool keyed = false;
  int remaining = 0;
  // A fixed sequence, so the measurement is the same on every machine and
  // every run. The exact keying does not matter; that it keeps decoding for
  // two minutes does.
  std::mt19937 rng(12'345);
  std::uniform_int_distribution<int> element(0, 2);

  for (int step = 0; step < blocks; ++step) {
    if (remaining <= 0) {
      keyed = !keyed;
      remaining = keyed && element(rng) == 0 ? 18 : 6;
    }
    --remaining;
    std::fill(spectrum.begin(), spectrum.end(), -110.0F);
    if (keyed) {
      const auto bin = static_cast<std::size_t>(
          tone_hz / (sample_rate * 0.5) * static_cast<double>(bin_count));
      if (bin < bin_count) spectrum[bin] = -55.0F;
    }
    static_cast<void>(
        bank.updateSpectrum(now, 0.0, sample_rate * 0.5, spectrum, false));
    RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = now;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          keyed ? 0.40F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
      phase += 2.0 * std::numbers::pi * tone_hz / sample_rate;
    }
    const auto started = Clock::now();
    static_cast<void>(bank.processSamples(block));
    costs_us.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - started)
            .count());
    now += 10'000'000;
  }

  const auto median_of = [&costs_us](const std::size_t from,
                                     const std::size_t to) {
    std::vector<double> slice(costs_us.begin() +
                                  static_cast<std::ptrdiff_t>(from),
                              costs_us.begin() +
                                  static_cast<std::ptrdiff_t>(to));
    std::sort(slice.begin(), slice.end());
    return slice[slice.size() / 2];
  };
  const std::size_t fifth = costs_us.size() / 5;
  const double early = median_of(0, fifth);
  const double late = median_of(costs_us.size() - fifth, costs_us.size());
  // A machine fast enough to measure the early median as zero cannot produce
  // a ratio at all; treat the finest measurable interval as the floor.
  const double ratio = late / std::max(early, 1.0);
  expect(ratio < 4.0,
         "decoding a long session costs no more per block at the end than at "
         "the start (median " + std::to_string(early) + " us -> " +
         std::to_string(late) + " us, ratio " + std::to_string(ratio) + ")");
}

// A stream that decoded and named its station keeps its region on the display
// about twice as long as one that never identified.
//
// The two fixtures key identically: CQ DE DK7SS and CQ DE QK7SS are the same
// elements bar one, decode equally cleanly and both verify. They differ only
// in whether the decoded call names a country that exists, so the only thing
// under test here is identification, not decode quality.
//
// The longer hold lives in the display's retained observation, not in track
// expiry. A track owns a decoder, a slot in the bounded bank and a frequency
// cell that a genuinely new station must be able to take over; holding an
// identified track alive twice as long stops that takeover and breaks the
// identity inheritance asserted in the replacement fixture above. The region
// the operator asked to keep already outlives the track by design, so that is
// where the extra time belongs.
void test_identified_stream_holds_its_region_longer() {
  using cwassistant::core::CwChannelBank;
  constexpr double sample_rate = 8'000.0;
  constexpr double retention_seconds = 10.0;

  struct Region {
    std::size_t published_while_sending{0};
    std::string live_callsign;
    std::size_t published_after_silence{0};
    std::string callsign_after_silence;
  };

  const auto region_after_silence =
      [&](const std::string_view first_call_character,
          const double silence_seconds) -> Region {
    CwChannelBank bank({.empty_track_retention_seconds = 0.5,
                        .decoded_track_retention_seconds = retention_seconds,
                        // Fabricated spectra are driven faster than the
                        // detector cadence, and the exact lifecycle is what is
                        // being asserted, so cadence decimation is disabled.
                        .detector_frame_interval_seconds = 0.0,
                        .minimum_spectral_observations = 1,
                        .minimum_verification_symbols = 0,
                        .verification_enter_seconds = 0.0,
                        .verification_exit_seconds = 0.0});
    std::vector<float> bins(201, -110.0F);
    std::uint64_t now = 0;
    double phase = 0.0;
    const auto step = [&](const bool keyed) {
      bins.assign(bins.size(), -110.0F);
      if (keyed) bins[60] = -55.0F;
      static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));
      cwassistant::core::RealtimeSampleBlock block;
      block.stream.sample_rate_hz = sample_rate;
      block.timestamp_ns = now;
      block.sample_count = 80;
      for (std::size_t index = 0; index < block.sample_count; ++index) {
        block.samples[index] = {
            keyed ? 0.40F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
        phase += 2.0 * std::numbers::pi * 300.0 / sample_rate;
      }
      static_cast<void>(bank.processSamples(block));
      now += 10'000'000;
    };
    const auto gap = [&](const int steps) {
      for (int index = 0; index < steps; ++index) step(false);
    };
    const auto character = [&](const std::string_view elements) {
      for (std::size_t index = 0; index < elements.size(); ++index) {
        for (int repeat = 0; repeat < (elements[index] == '.' ? 6 : 18);
             ++repeat) {
          step(true);
        }
        if (index + 1U < elements.size()) gap(6);
      }
      gap(18);
    };
    const auto word_gap = [&] { gap(24); };
    character("-.-.");  // C
    character("--.-");  // Q
    word_gap();
    character("-..");  // D
    character(".");    // E
    word_gap();
    character(first_call_character);  // D (allocated) or Q (no such country)
    character("-.-");                 // K
    character("--...");               // 7
    character("...");                 // S
    character("...");                 // S
    word_gap();
    character(".");  // Close the preceding callsign token.

    Region region;
    const auto live = bank.channels();
    region.published_while_sending = live.size();
    if (!live.empty()) region.live_callsign = live.front().callsign;

    // The carrier stops. One frame at the far end of the silence is enough:
    // expiry reads the elapsed time, not the number of frames, and a flat
    // spectrum offers no peak that could start a new track.
    bins.assign(bins.size(), -110.0F);
    const auto silent_now =
        now + static_cast<std::uint64_t>(silence_seconds * 1'000'000'000.0);
    static_cast<void>(bank.updateSpectrum(silent_now, 0.0, 1'000.0, bins));
    const auto& remaining = bank.channels();
    region.published_after_silence = remaining.size();
    if (!remaining.empty())
      region.callsign_after_silence = remaining.front().callsign;
    return region;
  };

  const Region named_early =
      region_after_silence("-..", retention_seconds * 0.6);
  const Region anonymous_early =
      region_after_silence("--.-", retention_seconds * 0.6);
  expect(named_early.published_while_sending == 1 &&
             named_early.live_callsign == "DK7SS",
         "the identified fixture names its station while it is sending");
  expect(anonymous_early.published_while_sending == 1 &&
             anonymous_early.live_callsign.empty(),
         "the control fixture decodes and publishes but never names a station");
  expect(named_early.published_after_silence == 1 &&
             anonymous_early.published_after_silence == 1,
         "both streams keep their region for the standard retention");

  // Past the standard retention, only the identified stream is still there,
  // and it is still wearing the label that makes it worth keeping.
  const Region named_late =
      region_after_silence("-..", retention_seconds * 1.5);
  const Region anonymous_late =
      region_after_silence("--.-", retention_seconds * 1.5);
  expect(anonymous_late.published_after_silence == 0,
         "an unidentified stream gives up its region on the standard "
         "retention");
  expect(named_late.published_after_silence == 1 &&
             named_late.callsign_after_silence == "DK7SS",
         "an identified stream keeps its labelled region past the standard "
         "retention");

  // Longer, not permanent.
  const Region named_expired =
      region_after_silence("-..", retention_seconds * 2.5);
  expect(named_expired.published_after_silence == 0,
         "the longer hold is bounded: an identified stream gives up its "
         "region once the extended retention passes");
}

// A station that has identified more than once keeps its name through a spell
// of poor copy, and gives it up only to a rival that identifies as often.
//
// The fault this covers was reported from a live station: track 3090 on
// 7,015,005 Hz read EH3ST cleanly eight times in a minute, and EH3S, EH3SN,
// EHSMST and EG7S once each in between. The stream's name followed every one
// of them, because it was recomputed from the current text window on every
// snapshot and written straight over whatever had been established. Neither
// the transcript nor the callsign scoring changes here; only which of the
// readings gets to name the stream.
void test_stream_label_hysteresis() {
  using cwassistant::core::CwStreamLabel;
  using cwassistant::core::cwApplyStreamLabelReading;
  using cwassistant::core::cwCountCallsignOccurrences;

  expect(cwCountCallsignOccurrences("CQ DE EH3ST EH3ST K", "EH3ST") == 2,
         "a call sent twice counts as two readings");
  expect(cwCountCallsignOccurrences("VEH3STK", "EH3ST") == 0 &&
             cwCountCallsignOccurrences("EH3ST/P", "EH3ST") == 0,
         "a call embedded in a longer token is a different decode, not "
         "another reading of this one");

  {
    // Below the bar the name still follows the newest reading. An operator
    // wants the call the moment it is first copied; it is keeping the name
    // that has to be earned.
    CwStreamLabel provisional;
    provisional = cwApplyStreamLabelReading(std::move(provisional), "EH3ST",
                                            "CQ DE EH3ST ", "");
    expect(provisional.callsign == "EH3ST" && provisional.support == 1,
           "one clean identification names the stream immediately");
    provisional = cwApplyStreamLabelReading(std::move(provisional), "EG7S",
                                            "CQ DE EG7S ", "");
    expect(provisional.callsign == "EG7S",
           "a provisional name follows the newest reading");
  }

  {
    // The literal and the refined path are two readings of one transmission.
    // Counting both would let a single identification establish a name and
    // freeze the first thing copied, however wrong.
    CwStreamLabel parallel;
    parallel = cwApplyStreamLabelReading(std::move(parallel), "EH3ST",
                                         "CQ DE EH3ST ", "CQ DE EH3ST ");
    expect(parallel.support == 1,
           "the two decoded paths corroborate a name once between them, not "
           "once each");
  }

  CwStreamLabel label;
  label = cwApplyStreamLabelReading(std::move(label), "EH3ST",
                                    "EH3ST CQ CQ EH3ST ", "");
  expect(label.callsign == "EH3ST" && label.support >= 2,
         "a call read twice establishes the stream's name");

  label = cwApplyStreamLabelReading(std::move(label), std::string_view{}, "",
                                    "");
  expect(label.callsign == "EH3ST",
         "silence is not evidence that a different station has arrived");

  label = cwApplyStreamLabelReading(std::move(label), "EG7S",
                                    "EH3ST CQ CQ EH3ST E EG7S ", "");
  expect(label.callsign == "EH3ST" && label.challenger == "EG7S" &&
             label.challenger_support == 1,
         "a single divergent reading challenges an established name instead "
         "of replacing it");

  label = cwApplyStreamLabelReading(std::move(label), "EH3ST",
                                    "EH3ST CQ CQ EH3ST E EG7S EH3ST ", "");
  expect(label.challenger.empty() && label.challenger_support == 0,
         "the station identifying again answers the challenge against it");

  label = cwApplyStreamLabelReading(std::move(label), "EG7S", "EG7S EG7S ", "");
  expect(label.callsign == "EG7S" && label.challenger.empty(),
         "a rival read as often as the incumbent takes the stream over");

  // The same rule, driven by real keyed audio through the whole bank.
  //
  // The fixture keys DK7SS bare twice, which is how a station answering a call
  // identifies, and then a single CQ DE DL7SS UP. DL7SS is one element away
  // from DK7SS, opens on an allocated prefix, and its split-runner context
  // outscores bare repetition outright, so the callsign scoring prefers it on
  // that one reading: exactly the single-reading misdecode that used to
  // rename the stream.
  using cwassistant::core::CwChannelBank;
  constexpr double sample_rate = 8'000.0;
  CwChannelBank bank({.empty_track_retention_seconds = 0.5,
                      .decoded_track_retention_seconds = 10.0,
                      // Fabricated spectra are driven faster than the detector
                      // cadence and the exact per-frame label is the subject,
                      // so cadence decimation is disabled.
                      .detector_frame_interval_seconds = 0.0,
                      .minimum_spectral_observations = 1,
                      .minimum_verification_symbols = 0,
                      .verification_enter_seconds = 0.0,
                      .verification_exit_seconds = 0.0});
  std::vector<float> bins(201, -110.0F);
  std::uint64_t now = 0;
  double phase = 0.0;
  const auto step = [&](const bool keyed) {
    bins.assign(bins.size(), -110.0F);
    if (keyed) bins[60] = -55.0F;
    static_cast<void>(bank.updateSpectrum(now, 0.0, 1'000.0, bins));
    cwassistant::core::RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = now;
    block.sample_count = 80;
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      block.samples[index] = {
          keyed ? 0.40F * static_cast<float>(std::sin(phase)) : 0.0F, 0.0F};
      phase += 2.0 * std::numbers::pi * 300.0 / sample_rate;
    }
    static_cast<void>(bank.processSamples(block));
    now += 10'000'000;
  };
  const auto gap = [&](const int steps) {
    for (int index = 0; index < steps; ++index) step(false);
  };
  const auto character = [&](const std::string_view elements) {
    for (std::size_t index = 0; index < elements.size(); ++index) {
      for (int repeat = 0; repeat < (elements[index] == '.' ? 6 : 18); ++repeat)
        step(true);
      if (index + 1U < elements.size()) gap(6);
    }
    gap(18);
  };
  const auto word_gap = [&] { gap(24); };
  const auto label_now = [&]() -> std::string {
    const auto channels = bank.channels();
    return channels.empty() ? std::string{} : channels.front().callsign;
  };
  const auto text_now = [&]() -> std::string {
    const auto channels = bank.channels();
    return channels.empty() ? std::string{} : channels.front().text;
  };
  const auto send_dk7ss = [&] {
    character("-..");    // D
    character("-.-");    // K
    character("--...");  // 7
    character("...");    // S
    character("...");    // S
    word_gap();
  };
  const auto send_dl7ss = [&] {
    character("-..");    // D
    character(".-..");   // L
    character("--...");  // 7
    character("...");    // S
    character("...");    // S
    word_gap();
  };

  send_dk7ss();
  send_dk7ss();
  const std::string established = label_now();
  expect(established == "DK7SS",
         "the fixture establishes DK7SS from two bare identifications");

  character("-.-.");  // C
  character("--.-");  // Q
  word_gap();
  character("-..");  // D
  character(".");    // E
  word_gap();
  send_dl7ss();
  character("..-");   // U
  character(".--.");  // P
  word_gap();
  const std::string after_rival = label_now();
  const std::string transcript = text_now();
  expect(after_rival == "DK7SS",
         "one better-placed reading of a different callsign does not rename "
         "an established stream");
  expect(transcript.find("DL7SS") != std::string::npos,
         "refusing the rename leaves the decoded transcript untouched");

  // Changeable, not frozen. A rival that identifies as often as the incumbent
  // did is a station, not a misdecode, and takes the stream over.
  send_dl7ss();
  expect(label_now() == "DL7SS",
         "a rival that identifies twice takes an established stream over");
}


// ---------------------------------------------------------------------------
// One keyer or several? A channel that holds several publishes occupancy.
// ---------------------------------------------------------------------------

const char* keyerFixtureMorse(const char symbol) {
  switch (symbol) {
    case 'A': return ".-";    case 'B': return "-...";  case 'C': return "-.-.";
    case 'D': return "-..";   case 'E': return ".";     case 'F': return "..-.";
    case 'G': return "--.";   case 'H': return "....";  case 'I': return "..";
    case 'J': return ".---";  case 'K': return "-.-";   case 'L': return ".-..";
    case 'M': return "--";    case 'N': return "-.";    case 'O': return "---";
    case 'P': return ".--.";  case 'Q': return "--.-";  case 'R': return ".-.";
    case 'S': return "...";   case 'T': return "-";     case 'U': return "..-";
    case 'V': return "...-";  case 'W': return ".--";   case 'X': return "-..-";
    case 'Y': return "-.--";  case 'Z': return "--..";  case '0': return "-----";
    case '1': return ".----"; case '2': return "..---"; case '3': return "...--";
    case '4': return "....-"; case '5': return "....."; case '6': return "-....";
    case '7': return "--..."; case '8': return "---.."; case '9': return "----.";
    default: return "";
  }
}

// One station's keying as (seconds, key-down) runs.
std::vector<std::pair<double, bool>> keyerFixtureRuns(
    const std::string_view message, const double wpm) {
  const double dot = 1.2 / wpm;
  std::vector<std::pair<double, bool>> runs;
  for (const char symbol : message) {
    if (symbol == ' ') {
      if (!runs.empty()) runs.back().first += 4.0 * dot;
      continue;
    }
    for (const char* element = keyerFixtureMorse(symbol); *element != '\0';
         ++element) {
      runs.push_back({*element == '-' ? 3.0 * dot : dot, true});
      runs.push_back({dot, false});
    }
    if (!runs.empty()) runs.back().first = 3.0 * dot;
  }
  return runs;
}

// Adds one keyed carrier, repeating its message for the whole buffer.
// `start_fraction` rotates the message so two stations sharing a fixture are
// not keyed in lockstep, which is what a real pileup is not.
void addKeyerFixtureCarrier(std::vector<float>& audio, const double sample_rate,
                            const double tone_hz, const double wpm,
                            const float amplitude,
                            const std::string_view message,
                            const double start_fraction) {
  const auto runs = keyerFixtureRuns(message, wpm);
  if (runs.empty()) return;
  constexpr double rise = 0.004;
  double phase = 0.0;
  std::size_t run =
      static_cast<std::size_t>(start_fraction *
                               static_cast<double>(runs.size())) %
      runs.size();
  std::size_t index = 0;
  while (index < audio.size()) {
    const auto length = static_cast<std::size_t>(runs[run].first * sample_rate);
    for (std::size_t step = 0; step < length && index < audio.size();
         ++step, ++index) {
      double envelope = 0.0;
      if (runs[run].second) {
        const double into = static_cast<double>(step) / sample_rate;
        const double from_end =
            (static_cast<double>(length) / sample_rate) - into;
        const double attack =
            into < rise ? 0.5 - 0.5 * std::cos(std::numbers::pi * into / rise)
                        : 1.0;
        const double decay =
            from_end < rise
                ? 0.5 - 0.5 * std::cos(std::numbers::pi * from_end / rise)
                : 1.0;
        envelope = std::min(attack, decay);
      }
      phase += 2.0 * std::numbers::pi * tone_hz / sample_rate;
      audio[index] +=
          static_cast<float>(amplitude * envelope * std::sin(phase));
    }
    run = (run + 1U) % runs.size();
  }
}

// Deterministic across standard libraries: normal_distribution's mapping is
// not standardized, a sum of twelve uniform draws is.
void addKeyerFixtureNoise(std::vector<float>& audio, const float level,
                          const unsigned seed) {
  std::mt19937 generator(seed);
  for (float& sample : audio) {
    double sum = 0.0;
    for (int draw = 0; draw < 12; ++draw)
      sum += static_cast<double>(generator()) / 4'294'967'296.0;
    sample += level * static_cast<float>(sum - 6.0);
  }
}

// What the bank made of one band of the fixture. The window matters: two
// carriers 60 Hz apart are two tracks, either of which may be the one the
// bank keeps live, so every assertion is made about the window rather than
// about one nominal frequency.
struct KeyerFixtureOutcome {
  bool tracked{false};
  // Strongest divergence between the wide- and narrow-filter speed estimates
  // seen on any live track in the window.
  float speed_ratio{0.0F};
  bool unresolved{false};
  // A live, currently matched carrier in the window that is published with its
  // frequency and key state but without text.
  bool occupancy_published{false};
  double occupancy_frequency_hz{0.0};
  // Any channel in the window still publishing open-ended text.
  bool text_published{false};
  std::string text;
  std::string callsign;
  std::size_t unresolved_track_count{0};
  std::size_t separation_candidate_count{0};
  std::uint8_t neighbours{0};
  // Block indices, for the hysteresis assertion: when the measured ratio first
  // exceeded the configured limit, and when the standing verdict first
  // followed it.
  std::size_t first_ratio_over_limit_block{0};
  std::size_t first_verdict_block{0};
};

KeyerFixtureOutcome runKeyerFixture(
    const std::vector<float>& audio, const double sample_rate,
    const double window_center_hz, const double window_hz,
    const cwassistant::core::CwChannelBankConfig& config) {
  using namespace cwassistant::core;
  SpectrumAnalyzer analyzer({.audio_upper_frequency_hz = 3'000.0});
  CwChannelBank bank(config);
  RealtimeSampleBlock block;
  block.stream.sample_rate_hz = sample_rate;
  KeyerFixtureOutcome outcome;
  std::size_t position = 0;
  std::size_t blocks = 0;
  std::uint64_t now = 0;
  while (position < audio.size()) {
    const std::size_t take =
        std::min<std::size_t>(1'024, audio.size() - position);
    block.sample_count = take;
    block.timestamp_ns = now;
    for (std::size_t index = 0; index < take; ++index)
      block.samples[index] = {audio[position + index], 0.0F};
    for (const auto& frame : analyzer.process(block)) {
      static_cast<void>(bank.updateSpectrum(
          frame.timestamp_ns, frame.lower_frequency_hz,
          frame.upper_frequency_hz, frame.instantaneous_bins_dbfs, false));
    }
    static_cast<void>(bank.processSamples(block));
    ++blocks;
    for (const auto& diagnostic : bank.allTrackDiagnostics()) {
      if (std::abs(diagnostic.frequency_hz - window_center_hz) > window_hz)
        continue;
      outcome.tracked = true;
      outcome.speed_ratio =
          std::max(outcome.speed_ratio, diagnostic.keyer_speed_ratio);
      outcome.unresolved =
          outcome.unresolved || diagnostic.keyer_overlap_unresolved;
      outcome.neighbours =
          std::max(outcome.neighbours, diagnostic.overlap_neighbour_count);
      if (outcome.first_ratio_over_limit_block == 0 &&
          diagnostic.keyer_speed_ratio >
              config.maximum_single_keyer_speed_ratio) {
        outcome.first_ratio_over_limit_block = blocks;
      }
      if (outcome.first_verdict_block == 0 &&
          diagnostic.keyer_overlap_unresolved) {
        outcome.first_verdict_block = blocks;
      }
    }
    position += take;
    now += static_cast<std::uint64_t>(static_cast<long double>(take) *
                                      1'000'000'000.0L / sample_rate);
  }
  const auto diagnostics = bank.verificationDiagnostics();
  outcome.unresolved_track_count = diagnostics.unresolved_overlap_tracks;
  outcome.separation_candidate_count = diagnostics.joint_separation_candidates;
  for (const auto& channel : bank.channels()) {
    if (std::abs(channel.frequency_hz - window_center_hz) > window_hz) continue;
    if (!channel.text.empty()) {
      outcome.text_published = true;
      outcome.text = channel.text;
    }
    if (!channel.callsign.empty()) outcome.callsign = channel.callsign;
    if (channel.active &&
        channel.verification_state ==
            cwassistant::core::CwTrackState::Verified &&
        channel.verification_reason ==
            cwassistant::core::CwVerificationReason::UnresolvedKeyerOverlap &&
        channel.text.empty()) {
      outcome.occupancy_published = true;
      outcome.occupancy_frequency_hz = channel.frequency_hz;
    }
  }
  return outcome;
}

void test_overlapping_keyers_publish_occupancy_not_text() {
  using namespace cwassistant::core;
  constexpr double sample_rate = 8'000.0;
  constexpr double carrier_hz = 700.0;
  // 60 Hz, the closest spacing the operator's pileup capture contains and
  // narrower than any filter that still passes a keyed carrier.
  constexpr double neighbour_hz = 760.0;
  constexpr double wpm = 20.0;
  constexpr double seconds = 30.0;
  const auto count = static_cast<std::size_t>(seconds * sample_rate);
  constexpr float noise = 0.02F;
  // 20 dB in the 120 Hz reference bandwidth the bank measures against.
  const float amplitude =
      noise * std::sqrt(std::pow(10.0F, 2.0F) /
                        static_cast<float>(sample_rate / 2.0 / 120.0));

  std::vector<float> alone(count, 0.0F);
  addKeyerFixtureCarrier(alone, sample_rate, carrier_hz, wpm, amplitude,
                         "CQ CQ DE IU0LFQ IU0LFQ K ", 0.0);
  addKeyerFixtureNoise(alone, noise, 11U);
  const auto single = runKeyerFixture(alone, sample_rate, carrier_hz, 25.0, {});

  expect(single.tracked && single.speed_ratio > 0.0F &&
             single.speed_ratio <= 1.5F,
         "one keyer reads nearly the same speed at the narrowest and the "
         "widest analysis filter");
  expect(!single.unresolved && single.text_published,
         "a channel holding one keyer keeps publishing its decoded text");

  // The same station with one neighbour 60 Hz away at a similar level. Their
  // envelopes sum; the sum is not Morse.
  std::vector<float> crowded(count, 0.0F);
  addKeyerFixtureCarrier(crowded, sample_rate, carrier_hz, wpm, amplitude,
                         "CQ CQ DE IU0LFQ IU0LFQ K ", 0.0);
  addKeyerFixtureCarrier(crowded, sample_rate, neighbour_hz, wpm * 1.07,
                         amplitude * 0.9F, "TEST DE DL2ABC PSE K ", 0.37);
  addKeyerFixtureNoise(crowded, noise, 11U);
  // One window covering both carriers. Which of two stations 60 Hz apart stays
  // the live track is a matter of which ridge wins, and the requirement is
  // about the crowd rather than about either of them by name.
  const auto crowded_outcome =
      runKeyerFixture(crowded, sample_rate, 0.5 * (carrier_hz + neighbour_hz),
                      60.0, {});

  expect(crowded_outcome.tracked && crowded_outcome.speed_ratio > 1.5F,
         "several superimposed keyers read far faster through a wide filter "
         "than through a narrow one");
  expect(crowded_outcome.unresolved,
         "a channel that reads as several keyers is marked unresolved");

  // Silenced, not deleted. Everything an operator tunes by survives: the
  // marker, its frequency, its verified carrier state and its key activity.
  expect(crowded_outcome.occupancy_published &&
             crowded_outcome.occupancy_frequency_hz >= carrier_hz - 25.0 &&
             crowded_outcome.occupancy_frequency_hz <= neighbour_hz + 25.0,
         "an unresolved track stays published, active and verified at its own "
         "frequency, naming the overlap as the reason its text is withheld");
  expect(!crowded_outcome.text_published && crowded_outcome.callsign.empty(),
         "no channel in a crowded window publishes open-ended text or a "
         "station name");
  expect(crowded_outcome.unresolved_track_count >= 1,
         "the bank counts unresolved tracks so an operator-facing diagnostic "
         "can say how much of a window is occupancy rather than copy");
  // The evidence a later, more expensive separation stage needs to choose
  // which refused tracks are worth attempting. Two carriers is a problem such
  // a stage can condition; the dense centre of a real pileup is not.
  expect(crowded_outcome.separation_candidate_count >= 1 &&
             crowded_outcome.neighbours >= 1,
         "an unresolved track records a sparse enough neighbourhood to be "
         "offered to a later separation stage");

  // The gate, and nothing else, is what withheld the text: the same audio
  // through a bank that does not run the test decodes as before.
  const auto ungated =
      runKeyerFixture(crowded, sample_rate, 0.5 * (carrier_hz + neighbour_hz),
                      60.0, {.resolve_overlapping_keyers = false});
  expect(!ungated.unresolved && ungated.text_published,
         "disabling the resolution test restores the open-ended transcript "
         "the gate was withholding");

  // Hysteresis. The verdict follows sustained evidence, never one
  // measurement: a pileup thinning for a word, or a DX pausing, must not flip
  // a stream between publishing text and withholding it.
  expect(crowded_outcome.first_ratio_over_limit_block > 0 &&
             crowded_outcome.first_verdict_block >
                 crowded_outcome.first_ratio_over_limit_block + 5,
         "the standing verdict lags the first measurement that crosses the "
         "limit by several further measurements");
}

// The bank used to hold 24 carriers, and the 24 was not a decision: the
// display had 24 hand-written colors, the color-lease array was sized to the
// palette, and the track cap was sized to the leases. An operator capture of a
// real DX pileup holds about 50 stations inside a 6 kHz window at a median
// spacing of 70 Hz, all of them further apart than `minimum_separation_hz`, so
// half of that band was carriers this bank could resolve and had no room for.
// The cap falsified its own diagnostics while it held, too: every live record
// read `tracks: 24` exactly, which reads as a busy band and was saturation.
void test_channel_bank_follows_a_pileup() {
  using namespace cwassistant::core;
  constexpr double sample_rate = 12'000.0;
  constexpr double upper_hz = 6'000.0;
  constexpr std::size_t bin_count = 601;  // 10 Hz per bin across the window
  constexpr std::size_t carriers = 40;
  constexpr double first_carrier_hz = 400.0;
  // Well clear of `minimum_separation_hz`, so every carrier here is one the
  // detector is meant to resolve and the only thing that can lose them is the
  // cap itself.
  constexpr double spacing_hz = 120.0;
  static_assert(first_carrier_hz + spacing_hz * (carriers - 1) < upper_hz);
  static_assert(carriers > 24,
                "the point of the fixture is to exceed the old cap");

  // Only the evidence thresholds are relaxed, so a deterministic test reaches
  // a verified verdict in seconds of audio rather than a minute of it. The
  // track cap is deliberately left at its default: that is what is under test.
  CwChannelBank bank({.minimum_spectral_observations = 1,
                      .minimum_verification_symbols = 0});

  std::vector<float> bins(bin_count, -110.0F);
  std::array<double, carriers> phase{};
  std::array<int, carriers> remaining{};
  std::array<bool, carriers> keyed{};
  std::uint64_t now = 0;
  std::uint32_t random_state = 12'345U;
  // A plain linear congruential step rather than <random>, so the fixture is
  // the same sequence on every standard library the project builds against.
  const auto next_element = [&random_state]() {
    random_state = random_state * 1'664'525U + 1'013'904'223U;
    return (random_state >> 16) % 3U;
  };
  const auto carrier_hz = [](const std::size_t index) {
    return first_carrier_hz + spacing_hz * static_cast<double>(index);
  };
  for (int step = 0; step < 900; ++step) {
    bins.assign(bin_count, -110.0F);
    for (std::size_t index = 0; index < carriers; ++index) {
      if (remaining[index] <= 0) {
        keyed[index] = !keyed[index];
        // Dits, dahs and the gaps between them, at independent phases per
        // carrier, so the bank sees forty separately keyed envelopes.
        remaining[index] = keyed[index] && next_element() == 0U ? 18 : 6;
      }
      --remaining[index];
      if (!keyed[index]) continue;
      bins[static_cast<std::size_t>(carrier_hz(index) / upper_hz *
                                    static_cast<double>(bin_count - 1))] =
          -55.0F;
    }
    static_cast<void>(bank.updateSpectrum(now, 0.0, upper_hz, bins));
    RealtimeSampleBlock block;
    block.stream.sample_rate_hz = sample_rate;
    block.timestamp_ns = now;
    block.sample_count = 120;
    for (std::size_t sample = 0; sample < block.sample_count; ++sample) {
      float value = 0.0F;
      for (std::size_t index = 0; index < carriers; ++index) {
        if (keyed[index]) {
          value += 0.10F * static_cast<float>(std::sin(phase[index]));
        }
        phase[index] +=
            2.0 * std::numbers::pi * carrier_hz(index) / sample_rate;
      }
      block.samples[sample] = {value, 0.0F};
    }
    static_cast<void>(bank.processSamples(block));
    now += 10'000'000ULL;
  }

  const auto tracked = bank.allTrackDiagnostics();
  expect(tracked.size() > 24,
         "the channel bank follows more simultaneous carriers than the old "
         "24-color display palette allowed");
  const auto& published = bank.channels();
  expect(published.size() > 24,
         "every carrier the bank follows past the old cap is reported, so a "
         "diagnostic reading of the track count is the band and not the cap");

  // The lease table has to have grown with the cap, not stayed at the palette
  // size. If it had not, every lease would be taken as soon as the bank filled
  // and `assignOrRefreshColor` would fall through to its modulo fallback,
  // handing carriers 120 Hz apart the same color.
  std::vector<std::uint8_t> colors;
  colors.reserve(published.size());
  for (const auto& channel : published) colors.push_back(channel.color_index);
  std::sort(colors.begin(), colors.end());
  expect(std::adjacent_find(colors.begin(), colors.end()) == colors.end(),
         "every concurrently published track holds its own color index past "
         "the old 24-lease limit");
}

int main() {
  test_ring_buffer();
  test_scheduler();
  test_cw_timing_decoder();
  test_cw_channel_bank();
  test_established_cw_track_reserves_its_carrier_ridge();
  test_cw_channel_bank_state_reason_consistency();
  test_overlapping_keyers_publish_occupancy_not_text();
  test_channel_bank_follows_a_pileup();
  test_operator_selected_cw_probe();
  test_cw_channel_presentation_frequency_model();
  test_cw_channel_bank_implausible_character_distribution();
  test_decoder_display_setting_invariance();
  test_soft_decision_keying_evidence();
  test_callsign_policy();
  test_callsign_policy_prosign_glue();
  test_cw_morse_alphabet_survives_missing_files();
  test_cw_morse_alphabet();
  test_cw_callsign_prefixes_survive_missing_files();
  test_cw_callsign_prefix_table();
  test_cw_context_rescorer();
  test_decoding_cost_does_not_grow_with_session_length();
  test_identified_stream_holds_its_region_longer();
  test_stream_label_hysteresis();
  test_presented_speed_requires_evidence();
  test_spectrum_settings();
  test_wav_replay_source();
  test_wav_writer();
  test_spectrum_analyzer();
  test_remote_control_lease();
  test_transmit_guard();
  test_cw_transmit_encoder();
  test_selected_track_audio_monitor();
  test_adif();
  test_split_transverter_and_satellite_adif();
  test_negative_transverter_offset_and_invalid_frequency();
  test_band_selected_station_equipment_adif();
  test_reference_rig_profiles();
  test_cat4om_protocol_contract();
  if (failures == 0) {
    std::cout << "All core tests passed\n";
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
