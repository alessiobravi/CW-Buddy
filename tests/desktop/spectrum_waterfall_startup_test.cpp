#include <QColor>
#include <QGuiApplication>
#include <QSGNode>
#include <QTemporaryFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "cwassistant/core/iq_receive.hpp"
#include "cwassistant/core/sample_block.hpp"

#include "decoder/local_character_decoder.hpp"
#include "replay/decoder_channel_model.hpp"
#include "replay/replay_controller.hpp"
#include "visualization/spectrum_waterfall_item.hpp"
#include "visualization/waterfall_conditioner.hpp"

namespace {

class TestableSpectrumWaterfallItem final
    : public cwassistant::desktop::SpectrumWaterfallItem {
 public:
  using SpectrumWaterfallItem::updatePaintNode;
};

using CharacterWindow =
    cwassistant::desktop::CwCharacterFeatureWindowPtr;

std::vector<CharacterWindow> feedFrontend(
    cwassistant::desktop::LocalCharacterFrontendBank& bank,
    const std::span<const cwassistant::core::CwCharacterTrackSnapshot> channels,
    const std::span<const double> tones_hz, const std::size_t block_count,
    std::uint64_t& sequence, std::uint64_t& timestamp_ns,
    std::uint64_t& sample_cursor) {
  constexpr double sample_rate_hz = 3'200.0;
  constexpr std::size_t samples_per_block = 3'200U;
  std::vector<CharacterWindow> windows;
  for (std::size_t block_index = 0; block_index < block_count;
       ++block_index) {
    cwassistant::core::RealtimeSampleBlock block;
    block.stream = {.kind = cwassistant::core::StreamKind::Audio,
                    .sample_rate_hz = sample_rate_hz,
                    .center_frequency_hz = 0.0,
                    .channel_count = 1};
    block.sequence = sequence++;
    block.timestamp_ns = timestamp_ns;
    block.sample_count = samples_per_block;
    for (std::size_t sample = 0; sample < samples_per_block; ++sample) {
      double value = 0.0;
      for (const double tone_hz : tones_hz) {
        value += std::sin(2.0 * std::numbers::pi * tone_hz *
                          static_cast<double>(sample_cursor + sample) /
                          sample_rate_hz);
      }
      const double scale = tones_hz.empty()
          ? 0.0 : 0.4 / static_cast<double>(tones_hz.size());
      block.samples[sample] = {
          static_cast<float>(value * scale), 0.0F};
    }
    sample_cursor += samples_per_block;
    timestamp_ns += 1'000'000'000ULL;
    auto produced = bank.process(block, channels);
    windows.insert(windows.end(),
                   std::make_move_iterator(produced.begin()),
                   std::make_move_iterator(produced.end()));
  }
  return windows;
}

bool testLocalCharacterFrontendBank() {
  std::uint64_t sequence = 1U;
  std::uint64_t timestamp_ns = 1'000'000'000ULL;
  std::uint64_t sample_cursor = 0U;
  const std::array<double, 5> tones{600.0, 700.0, 800.0, 900.0, 1'000.0};
  std::array<cwassistant::core::CwCharacterTrackSnapshot, 5> channels{};
  channels[0] = {.id = 101, .frequency_hz = 600.0, .snr_db = 2.0F,
                 .verification_state = cwassistant::core::CwTrackState::Verified,
                 .active = true};
  channels[1] = {.id = 102, .frequency_hz = 700.0, .snr_db = 12.0F,
                 .active = true, .operator_selected = true};
  channels[2] = {.id = 103, .frequency_hz = 800.0, .snr_db = 30.0F,
                 .active = true};
  channels[3] = {.id = 104, .frequency_hz = 900.0, .snr_db = 30.0F,
                 .verification_state = cwassistant::core::CwTrackState::Verified,
                 .active = false};
  channels[4] = {.id = 105, .frequency_hz = 1'000.0, .snr_db = 5.0F,
                 .verification_state =
                     cwassistant::core::CwTrackState::MorseLikely,
                 .active = true};

  cwassistant::desktop::LocalCharacterFrontendBank bank{2U};
  if (!feedFrontend(bank, channels, tones, 9U, sequence, timestamp_ns,
                    sample_cursor).empty()) {
    return false;
  }
  bank.setEnabled(true);

  cwassistant::desktop::LocalCharacterFrontendBank ineligible_bank{2U};
  ineligible_bank.setEnabled(true);
  std::array<cwassistant::core::CwCharacterTrackSnapshot, 2> ineligible{
      channels[2], channels[3]};
  if (!feedFrontend(ineligible_bank, ineligible, tones, 9U, sequence,
                    timestamp_ns, sample_cursor).empty()) {
    return false;
  }
  ineligible[0].operator_selected = true;
  if (!feedFrontend(ineligible_bank, ineligible, tones, 1U, sequence,
                    timestamp_ns, sample_cursor).empty()) {
    return false;
  }

  const auto first = feedFrontend(bank, channels, tones, 9U, sequence,
                                  timestamp_ns, sample_cursor);
  if (first.size() != 2U) return false;
  const auto find_track = [](const auto& windows, const std::uint64_t id) {
    return std::find_if(windows.begin(), windows.end(),
                        [id](const CharacterWindow& window) {
                          return window && window->track.track_id == id;
                        });
  };
  const auto first_verified = find_track(first, 101U);
  const auto first_selected = find_track(first, 102U);
  if (first_verified == first.end() || first_selected == first.end() ||
      find_track(first, 103U) != first.end() ||
      find_track(first, 104U) != first.end() ||
      find_track(first, 105U) != first.end()) {
    return false;
  }
  const auto first_key = (*first_verified)->track;

  auto silent_channels = channels;
  for (auto& channel : silent_channels) channel.active = false;
  const auto slow_word_gap = feedFrontend(
      bank, silent_channels, tones, 1U, sequence, timestamp_ns, sample_cursor);
  const auto gap_verified = find_track(slow_word_gap, 101U);
  if (gap_verified == slow_word_gap.end() ||
      (*gap_verified)->track != first_key) {
    return false;
  }

  cwassistant::core::RealtimeSampleBlock empty_block;
  for (std::size_t index = 0; index < 501U; ++index) {
    empty_block.sequence = sequence++;
    empty_block.timestamp_ns = timestamp_ns;
    timestamp_ns += 1'000'000ULL;
    if (!bank.process(empty_block, {}).empty()) return false;
  }
  const auto reacquired = feedFrontend(bank, channels, tones, 9U, sequence,
                                       timestamp_ns, sample_cursor);
  const auto reacquired_verified = find_track(reacquired, 101U);
  if (reacquired_verified == reacquired.end() ||
      (*reacquired_verified)->track.track_generation !=
          first_key.track_generation ||
      (*reacquired_verified)->track.frontend_generation ==
          first_key.frontend_generation) {
    return false;
  }

  bank.reset();
  const auto reset_windows = feedFrontend(bank, channels, tones, 9U, sequence,
                                          timestamp_ns, sample_cursor);
  const auto reset_verified = find_track(reset_windows, 101U);
  if (reset_verified == reset_windows.end() ||
      (*reset_verified)->track.track_generation == first_key.track_generation) {
    return false;
  }

  cwassistant::desktop::LocalCharacterFrontendBank likely_bank{1U};
  likely_bank.setEnabled(true);
  const std::array<cwassistant::core::CwCharacterTrackSnapshot, 1> likely_track{
      channels[4]};
  const std::array<double, 1> likely_tone{1'000.0};
  const auto likely_windows = feedFrontend(
      likely_bank, likely_track, likely_tone, 9U, sequence, timestamp_ns,
      sample_cursor);
  if (likely_windows.size() != 1U || !likely_windows.front() ||
      likely_windows.front()->track.track_id != 105U) {
    return false;
  }

  cwassistant::desktop::LocalCharacterFrontendBank centered_bank{1U};
  centered_bank.setEnabled(true);
  auto centered_track = channels[2];
  centered_track.id = 106U;
  centered_track.frequency_hz = 834.0;
  centered_track.presentation_frequency_hz = 800.0;
  centered_track.verification_state =
      cwassistant::core::CwTrackState::MorseLikely;
  const std::array<cwassistant::core::CwCharacterTrackSnapshot, 1> centered_tracks{
      centered_track};
  const std::array<double, 1> centered_tone{800.0};
  const auto centered_windows = feedFrontend(
      centered_bank, centered_tracks, centered_tone, 9U, sequence,
      timestamp_ns, sample_cursor);
  if (centered_windows.size() != 1U || !centered_windows.front()) return false;
  const auto peak = *std::max_element(centered_windows.front()->features.cbegin(),
                                      centered_windows.front()->features.cend());
  return std::isfinite(peak) && peak > -4.0F;
}

}  // namespace

// A zoom must survive the ordinary frames that follow it. Preservation across a
// retune is predicated on the source bounds changing, so testing that condition
// alone in acceptFrame reset the view on every steady-state frame and a zoom
// lasted until the next frame arrived. Returns a nonzero code on failure.
int testZoomSurvivesFrames() {
  constexpr double kLowerHz = 7'000'000.0;
  constexpr double kUpperHz = 7'100'000.0;
  cwassistant::desktop::SpectrumWaterfallItem item;
  cwassistant::desktop::SpectrumFrame frame;
  frame.bins_dbfs = QVector<float>(1024, -90.0F);
  frame.lower_frequency_hz = kLowerHz;
  frame.upper_frequency_hz = kUpperHz;
  frame.sequence = 1;
  item.acceptFrame(frame);
  if (std::abs(item.lowerFrequencyHz() - kLowerHz) > 0.5 ||
      std::abs(item.upperFrequencyHz() - kUpperHz) > 0.5) {
    return 55;
  }

  item.zoomAt(0.5 * (kLowerHz + kUpperHz), 0.25);
  const double zoomed_lower = item.lowerFrequencyHz();
  const double zoomed_upper = item.upperFrequencyHz();
  const double zoomed_span = zoomed_upper - zoomed_lower;
  if (zoomed_span >= (kUpperHz - kLowerHz) - 0.5 || zoomed_span <= 0.0) {
    return 56;
  }

  // The regression: further frames on an unchanged source must not move it.
  for (quint64 sequence = 2; sequence <= 5; ++sequence) {
    frame.sequence = sequence;
    item.acceptFrame(frame);
    if (std::abs(item.lowerFrequencyHz() - zoomed_lower) > 0.5 ||
        std::abs(item.upperFrequencyHz() - zoomed_upper) > 0.5) {
      return 57;
    }
  }

  // Retuning slides the waterfall rather than discarding it. Clearing the
  // history on every frequency change wiped the display at each click of the
  // dial on a receiver whose frames carry absolute radio frequency, which is
  // not what the same action does on audio.
  // Rows are emitted against elapsed time, so the scene has to advance.
  for (quint64 sequence = 10; sequence <= 60; ++sequence) {
    frame.sequence = sequence;
    frame.timestamp_ns = sequence * 100'000'000ULL;
    item.acceptFrame(frame);
  }
  const int rows_before_retune = item.waterfallRowCount();
  if (rows_before_retune < 5) return 59;
  frame.sequence = 61;
  frame.timestamp_ns = 61 * 100'000'000ULL;
  frame.lower_frequency_hz = kLowerHz + 20'000.0;
  frame.upper_frequency_hz = kUpperHz + 20'000.0;
  item.acceptFrame(frame);
  if (item.waterfallRowCount() < rows_before_retune) return 60;

  // A span change cannot be slid, because the bins stop meaning the same
  // width, so that history is dropped.
  frame.sequence = 62;
  frame.timestamp_ns = 62 * 100'000'000ULL;
  frame.lower_frequency_hz = kLowerHz;
  frame.upper_frequency_hz = kLowerHz + 400'000.0;
  item.acceptFrame(frame);
  if (item.waterfallRowCount() > 1) return 61;

  // Restore the retune-preserving case for the checks below.
  frame.sequence = 63;
  frame.timestamp_ns = 63 * 100'000'000ULL;
  frame.lower_frequency_hz = kLowerHz;
  frame.upper_frequency_hz = kUpperHz;
  item.acceptFrame(frame);
  item.zoomAt(0.5 * (kLowerHz + kUpperHz), 0.25);
  const double retuned_lower = item.lowerFrequencyHz();
  const double retuned_span = item.upperFrequencyHz() - retuned_lower;

  // A retune keeps the zoom width and carries it with the receiver.
  constexpr double kShiftHz = 50'000.0;
  frame.sequence = 6;
  frame.lower_frequency_hz = kLowerHz + kShiftHz;
  frame.upper_frequency_hz = kUpperHz + kShiftHz;
  item.acceptFrame(frame);
  if (std::abs((item.upperFrequencyHz() - item.lowerFrequencyHz()) -
               retuned_span) > 0.5 ||
      std::abs(item.lowerFrequencyHz() - (retuned_lower + kShiftHz)) > 0.5) {
    return 58;
  }
  return 0;
}

// A VFO move must carry the tracked signals with it. The decoder shifts every
// track by the amount the dial moved so an identified signal keeps its
// identity, and the channel bank is tested for that directly. Nothing tested
// that the controller actually asks for the shift, which is the half that
// reaches an operator: without the request the tracks stay at the old audio
// frequency while the signal moves, and an identified stream is lost and then
// re-acquired as a new one.
int testVfoMoveShiftsTrackedSignals() {
  cwassistant::desktop::ReplayController controller;
  controller.setSourceMode(0);
  std::vector<double> shifts;
  QObject::connect(
      &controller, &cwassistant::desktop::ReplayController::
                       liveFrequencyShiftRequested,
      &controller, [&shifts](const double delta) { shifts.push_back(delta); });

  // The first report only establishes the context; there is no previous
  // frequency to have moved from.
  controller.setRadioFrequencyContext(true, 7'020'000ULL, 7'020'000ULL, false,
                                      0, 700.0);
  if (!shifts.empty()) return 70;

  // Upper sideband: tuning the dial up moves a received signal down in audio
  // by the same amount, so the shift is the negative of the dial movement.
  controller.setRadioFrequencyContext(true, 7'020'300ULL, 7'020'000ULL, false,
                                      0, 700.0);
  if (shifts.size() != 1U || std::abs(shifts.back() + 300.0) > 0.001) {
    return 71;
  }

  // Lower sideband moves the other way.
  controller.setRadioFrequencyContext(true, 7'020'000ULL, 7'020'000ULL, false,
                                      1, 700.0);
  if (shifts.size() != 2U || std::abs(shifts.back() - (-300.0)) > 0.001) {
    // sideband 1 with a -300 Hz dial move gives -300 Hz of audio shift
    return 72;
  }

  // A report that merely repeats the frequency must not shift anything, or a
  // polling radio would drag the tracks on every update.
  controller.setRadioFrequencyContext(true, 7'020'000ULL, 7'020'000ULL, false,
                                      1, 700.0);
  if (shifts.size() != 2U) return 73;

  // A momentary loss of radio availability must not silently swallow the next
  // move. If it does, the tracks stay where they were while the signal moves
  // and the identified stream is lost.
  controller.setRadioFrequencyContext(false, 7'020'000ULL, 7'020'000ULL, false,
                                      1, 700.0);
  controller.setRadioFrequencyContext(true, 7'020'500ULL, 7'020'000ULL, false,
                                      1, 700.0);
  if (shifts.size() != 3U) return 74;
  return 0;
}


// Starting SDR reception must hand the decoder a window inside the passband.
//
// IqSubbandDecimator::process() refuses a block outright when
//   |decoder centre - capture centre| + bandwidth / 2 >= sample rate / 2,
// and LiveAudioDspWorker::drain() publishes the wide overview spectrum before
// it ever consults the decimator. The two together produce a fault that looks
// like anything but its cause: the spectrum and waterfall paint signals
// perfectly while not one sample reaches detection, so no track is ever
// created and nothing is marked.
//
// The stored decoder window defaults to 14.050 MHz and used to move only when
// the operator dragged a selection in the zoomed view, so a receiver started
// on any other band decoded nothing at all -- and a receiver retuned away from
// 20 m lost every track the moment the passband slid out from under the
// window.
int testSdrCaptureKeepsDecoderWindowInsidePassband() {
  cwassistant::desktop::ReplayController controller;
  std::vector<std::pair<double, double>> windows;
  QObject::connect(
      &controller,
      &cwassistant::desktop::ReplayController::liveSdrDecoderWindowRequested,
      &controller, [&windows](const double center, const double bandwidth) {
        windows.emplace_back(center, bandwidth);
      });

  constexpr double kCaptureCenterHz = 7'030'000.0;
  constexpr double kSampleRateHz = 2'000'000.0;
  // The stale 20 m window, exactly as a fresh installation carries it.
  controller.setSdrInputSelection(
      QStringLiteral("driver=test"), QStringLiteral("Test SDR"),
      static_cast<qulonglong>(kCaptureCenterHz),
      static_cast<int>(kSampleRateHz), 192'000, QStringLiteral("RX"), true,
      0.0, 14'050'000ULL, 24'000);
  controller.startLiveSdr();
  if (windows.empty()) return 75;

  const auto [center_hz, bandwidth_hz] = windows.back();
  if (bandwidth_hz <= 0.0) return 76;
  if (std::abs(center_hz - kCaptureCenterHz) + bandwidth_hz * 0.5 >=
      kSampleRateHz * 0.5) {
    // The published window cannot be admitted: every IQ block would be
    // refused and the decoder would receive nothing while the spectrum ran.
    return 77;
  }

  // Close the loop on the real component rather than on arithmetic that
  // merely mirrors it, so the test fails if the decimator's own acceptance
  // rule ever moves away from what the controller assumes.
  cwassistant::core::IqSubbandDecimator decimator;
  if (!decimator.configure({.center_frequency_hz = center_hz,
                            .bandwidth_hz = bandwidth_hz,
                            .maximum_output_sample_rate_hz = std::clamp(
                                bandwidth_hz * 2.5, 48'000.0, 192'000.0)})) {
    return 78;
  }
  cwassistant::core::RealtimeSampleBlock block;
  block.stream = {.kind = cwassistant::core::StreamKind::ComplexIq,
                  .sample_rate_hz = kSampleRateHz,
                  .center_frequency_hz = kCaptureCenterHz,
                  .channel_count = 1};
  block.sample_count = 2'048;
  for (std::size_t index = 0; index < block.sample_count; ++index) {
    block.samples[index] = {0.01F, 0.0F};
  }
  cwassistant::core::RealtimeSampleBlock decoded;
  const auto status = decimator.process(block, decoded);
  if (status != cwassistant::core::IqBlockStatus::Accepted &&
      status !=
          cwassistant::core::IqBlockStatus::AcceptedAfterDiscontinuity) {
    return 79;
  }

  // Retuning the receiver drags the passband away from the window. The
  // decoder has to be moved with it, or the tracks vanish on the first click
  // of the dial while the spectrum carries on unchanged.
  const std::size_t before_retune = windows.size();
  controller.setSdrInputSelection(
      QStringLiteral("driver=test"), QStringLiteral("Test SDR"),
      21'030'000ULL, static_cast<int>(kSampleRateHz), 192'000,
      QStringLiteral("RX"), true, 0.0,
      static_cast<qulonglong>(std::llround(center_hz)), 24'000);
  if (windows.size() <= before_retune) return 80;
  const auto [retuned_center_hz, retuned_bandwidth_hz] = windows.back();
  if (std::abs(retuned_center_hz - 21'030'000.0) +
          retuned_bandwidth_hz * 0.5 >=
      kSampleRateHz * 0.5) {
    return 81;
  }
  return 0;
}


// A configured span opens the view at that width, not at whatever the radio
// delivered.
//
// SoapySDR devices routinely refuse the requested sample rate and run at the
// nearest one they support, so a 2 MHz selection can arrive as an 8 MHz frame.
// Opening at the frame span then buries the entire CW segment in a handful of
// pixels and the operator has to zoom in by hand before anything is legible.
int testPreferredSpanOpensTheConfiguredWidth() {
  constexpr double kCenterHz = 14'050'000.0;
  constexpr double kDeliveredSpanHz = 8'000'000.0;
  constexpr double kPreferredSpanHz = 2'000'000.0;
  cwassistant::desktop::SpectrumWaterfallItem item;
  item.setPreferredSpanHz(kPreferredSpanHz);

  cwassistant::desktop::SpectrumFrame frame;
  frame.bins_dbfs = QVector<float>(1024, -90.0F);
  frame.lower_frequency_hz = kCenterHz - kDeliveredSpanHz * 0.5;
  frame.upper_frequency_hz = kCenterHz + kDeliveredSpanHz * 0.5;
  frame.sequence = 1;
  item.acceptFrame(frame);

  const double span_hz = item.upperFrequencyHz() - item.lowerFrequencyHz();
  if (std::abs(span_hz - kPreferredSpanHz) > 1.0) return 82;
  const double view_center_hz =
      0.5 * (item.lowerFrequencyHz() + item.upperFrequencyHz());
  if (std::abs(view_center_hz - kCenterHz) > 1.0) return 83;

  // A preference wider than what arrived cannot be honoured and must not
  // manufacture spectrum that was never received.
  cwassistant::desktop::SpectrumWaterfallItem narrow_item;
  narrow_item.setPreferredSpanHz(4'000'000.0);
  cwassistant::desktop::SpectrumFrame narrow_frame;
  narrow_frame.bins_dbfs = QVector<float>(1024, -90.0F);
  narrow_frame.lower_frequency_hz = 14'000'000.0;
  narrow_frame.upper_frequency_hz = 14'096'000.0;
  narrow_frame.sequence = 1;
  narrow_item.acceptFrame(narrow_frame);
  if (std::abs(narrow_item.lowerFrequencyHz() - 14'000'000.0) > 1.0 ||
      std::abs(narrow_item.upperFrequencyHz() - 14'096'000.0) > 1.0) {
    return 84;
  }

  // No preference leaves the previous behaviour exactly as it was, so an
  // audio card still opens on its whole axis.
  cwassistant::desktop::SpectrumWaterfallItem plain_item;
  plain_item.acceptFrame(frame);
  if (std::abs(plain_item.upperFrequencyHz() - plain_item.lowerFrequencyHz() -
               kDeliveredSpanHz) > 1.0) {
    return 85;
  }
  return 0;
}

// A frame refused for backpressure must not be drawn as a break in reception.
//
// The waterfall pads intervals it received nothing for with blank rows, so that
// a real input stall reads as a gap and the time axis stays honest. Once frames
// began being dropped to bound memory, that padding had no way to tell "nothing
// arrived" from "we could not draw what arrived", and the display filled with
// black stripes -- reporting a fault in reception that had not happened, across
// a continuous band of signal.
int testDroppedFramesDoNotPadTheWaterfall() {
  constexpr double kLowerHz = 14'000'000.0;
  constexpr double kUpperHz = 14'048'000.0;
  cwassistant::desktop::SpectrumWaterfallItem item;
  item.setWaterfallRate(20);

  cwassistant::desktop::SpectrumFrame frame;
  frame.bins_dbfs = QVector<float>(512, -90.0F);
  frame.instantaneous_bins_dbfs = frame.bins_dbfs;
  frame.lower_frequency_hz = kLowerHz;
  frame.upper_frequency_hz = kUpperHz;
  frame.sequence = 1;
  frame.timestamp_ns = 0;
  item.acceptFrame(frame);
  const int after_first = item.waterfallRowCount();

  // A whole second later at twenty rows a second: nineteen intervals with no
  // frame. Reported as dropped, so the receiver was fine.
  frame.sequence = 2;
  frame.timestamp_ns = 1'000'000'000ULL;
  frame.dropped_before = 19;
  item.acceptFrame(frame);
  const int after_dropped = item.waterfallRowCount();
  if (after_dropped != after_first + 1) {
    // Padding a display-side drop is what produced the stripes.
    return 90;
  }

  // The same gap without that report is a genuine break in reception and must
  // still be padded, or a real input stall would be drawn as continuous signal
  // and the time axis would lie.
  frame.sequence = 3;
  frame.timestamp_ns = 2'000'000'000ULL;
  frame.dropped_before = 0;
  item.acceptFrame(frame);
  if (item.waterfallRowCount() <= after_dropped + 1) return 91;
  return 0;
}

namespace {

struct LabColor {
  double lightness{0.0};
  double a{0.0};
  double b{0.0};
  // Relative luminance, which is what decides whether a marker is visible
  // against a near-black spectrum at all.
  double luminance{0.0};
};

// sRGB to CIE L*a*b* (D65). The test measures in L*a*b* rather than comparing
// hex strings because the requirement is perceptual: two colors are far enough
// apart when an operator can tell them apart, and the distance that answers
// that question is not a distance between byte triples.
[[nodiscard]] LabColor labOf(const QColor& color) {
  const auto expand = [](const double channel) {
    return channel <= 0.04045 ? channel / 12.92
                              : std::pow((channel + 0.055) / 1.055, 2.4);
  };
  const double red = expand(color.redF());
  const double green = expand(color.greenF());
  const double blue = expand(color.blueF());
  const double x =
      (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047;
  const double y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue;
  const double z =
      (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883;
  const auto compand = [](const double value) {
    return value > 216.0 / 24'389.0 ? std::cbrt(value)
                                    : (841.0 / 108.0) * value + 4.0 / 29.0;
  };
  const double fx = compand(x);
  const double fy = compand(y);
  const double fz = compand(z);
  return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz), y};
}

[[nodiscard]] double perceptualDistance(const LabColor& left,
                                        const LabColor& right) {
  const double lightness = left.lightness - right.lightness;
  const double a = left.a - right.a;
  const double b = left.b - right.b;
  return std::sqrt(lightness * lightness + a * a + b * b);
}

}  // namespace

// The generated track palette, which replaced 24 hand-written hex strings that
// were setting the decoder's track capacity between them.
//
// Everything here has to hold for an arbitrary seed, not for one inspected
// table, because the seed is drawn afresh at every launch. That is a stronger
// contract than the fixed palette could offer: a hand-written table is checked
// once by eye, while a generator has to be right for every palette it can
// produce. Thresholds below are set from the measured worst case over the seeds
// tried, with margin.
int testGeneratedChannelPalette() {
  using cwassistant::desktop::ChannelColorPalette;
  constexpr std::uint32_t capacity =
      static_cast<std::uint32_t>(cwassistant::core::kCwMaximumTrackCapacity);
  // The default track cap. Separation has to be comfortable at the number of
  // tracks an operator will actually see, and merely adequate at the ceiling.
  constexpr std::uint32_t working_set = 64;

  double worst_distance_working_set = 1e9;
  double worst_distance_capacity = 1e9;
  double dimmest = 1e9;
  double palest = 1e9;
  double brightest = 0.0;

  for (std::uint32_t trial = 0; trial < 32U; ++trial) {
    // Spread over the 64-bit seed space, including zero, which is the one seed
    // a weak mixer is most likely to degenerate on.
    const std::uint64_t seed =
        trial == 0U ? 0U : 0x9e37'79b9'7f4a'7c15ULL * trial;
    const ChannelColorPalette palette{seed};
    if (palette.seed() != seed) return 100;

    // Stable for the life of the run, and reconstructible from the seed alone.
    // `color_identity_retention_seconds` holds a color against a frequency for
    // five minutes so a station heard again is recognisable; a color that
    // changed under a live track would break that, and would make one track
    // look like a new station on every redraw.
    const ChannelColorPalette twin{seed};
    std::vector<LabColor> measured;
    measured.reserve(capacity);
    for (std::uint32_t index = 0; index < capacity; ++index) {
      const QString color = palette.color(index);
      if (color != palette.color(index)) return 101;
      if (color != twin.color(index)) return 102;
      // The format the QML reads. Anything else silently draws nothing.
      if (color.size() != 7 || !color.startsWith(QLatin1Char('#')) ||
          !QColor::isValidColorName(color)) {
        return 103;
      }
      const LabColor lab = labOf(QColor(color));
      dimmest = std::min(dimmest, lab.luminance);
      brightest = std::max(brightest, lab.luminance);
      palest = std::min(palest, std::hypot(lab.a, lab.b));
      measured.push_back(lab);
    }

    for (std::uint32_t first = 0; first < capacity; ++first) {
      for (std::uint32_t second = first + 1; second < capacity; ++second) {
        const double distance =
            perceptualDistance(measured[first], measured[second]);
        worst_distance_capacity = std::min(worst_distance_capacity, distance);
        if (first < working_set && second < working_set) {
          worst_distance_working_set =
              std::min(worst_distance_working_set, distance);
        }
      }
    }
  }

  // Readable over a dark waterfall. A just-noticeable difference in this metric
  // is about 2.3, so both bounds are several times a difference an operator
  // could only just see.
  if (worst_distance_working_set < 5.0) return 104;
  if (worst_distance_capacity < 3.0) return 105;
  // Not too dark to see against a near-black spectrum, and not so bright it
  // reads as the white grid. Relative luminance, so this is a statement about
  // light reaching the eye rather than about a channel byte.
  if (dimmest < 0.20 || brightest > 0.92) return 106;
  // Not washed out: a generated color that lost all its chroma would be one of
  // the greys the unverified marker already uses.
  if (palest < 15.0) return 107;

  // A different seed is a different palette. Without this the whole generator
  // could be seed-independent and every other assertion here would still pass.
  const ChannelColorPalette first_palette{1U};
  const ChannelColorPalette second_palette{2U};
  std::uint32_t differing = 0;
  for (std::uint32_t index = 0; index < working_set; ++index) {
    if (first_palette.color(index) != second_palette.color(index)) ++differing;
  }
  if (differing < working_set / 2U) return 108;

  // The color a track is actually drawn in comes from the palette it is given,
  // with no table wrap in between: index 200 is a color, not index 200 modulo
  // a table length. That wrap is what used to tie capacity to the palette.
  cwassistant::core::CwChannelSnapshot snapshot{
      .id = 7,
      .color_index = 200,
      .frequency_hz = 900.0,
      .presentation_frequency_hz = 900.0,
      .verified_cw = true,
  };
  const auto model_of = [&snapshot](const ChannelColorPalette& used) {
    return cwassistant::desktop::decoderChannelModel(
               std::span<const cwassistant::core::CwChannelSnapshot>{&snapshot,
                                                                     1},
               {}, used)
        .front()
        .toMap()
        .value(QStringLiteral("color"))
        .toString();
  };
  if (model_of(first_palette) != first_palette.color(200)) return 109;
  if (model_of(first_palette) == model_of(second_palette)) return 110;

  // An unverified track stays neutral whatever the palette says, because it is
  // a carrier the decoder has not vouched for and a color would present it as a
  // station.
  snapshot.verified_cw = false;
  if (model_of(first_palette) != QStringLiteral("#8d9aaa")) return 111;

  return 0;
}

int main(int argc, char* argv[]) {
  QGuiApplication application(argc, argv);
  if (const int palette_failure = testGeneratedChannelPalette();
      palette_failure != 0) {
    return palette_failure;
  }
  if (const int dropped_pad_failure = testDroppedFramesDoNotPadTheWaterfall();
      dropped_pad_failure != 0) {
    return dropped_pad_failure;
  }
  if (const int preferred_span_failure =
          testPreferredSpanOpensTheConfiguredWidth();
      preferred_span_failure != 0) {
    return preferred_span_failure;
  }
  if (const int sdr_window_failure =
          testSdrCaptureKeepsDecoderWindowInsidePassband();
      sdr_window_failure != 0) {
    return sdr_window_failure;
  }
  if (const int zoom_failure = testZoomSurvivesFrames();
      zoom_failure != 0) {
    return zoom_failure;
  }
  if (const int vfo_failure = testVfoMoveShiftsTrackedSignals();
      vfo_failure != 0) {
    return vfo_failure;
  }
  if (!testLocalCharacterFrontendBank()) return 21;
  cwassistant::desktop::ReplayController frequency_mapping;
  frequency_mapping.setSourceMode(0);
  frequency_mapping.setRadioFrequencyContext(
      true, 7'020'000ULL, 7'021'300ULL, true, 0, 700.0);
  if (std::abs(frequency_mapping.rfFrequencyToDisplayHz(7'021'300ULL) -
               2'000.0) > 0.01 ||
      frequency_mapping.displayFrequencyToRfHz(2'000.0) != 7'021'300ULL) {
    return 51;
  }
  frequency_mapping.setRadioFrequencyContext(
      true, 7'020'000ULL, 7'018'700ULL, true, 1, 700.0);
  if (std::abs(frequency_mapping.rfFrequencyToDisplayHz(7'018'700ULL) -
               2'000.0) > 0.01 ||
      frequency_mapping.displayFrequencyToRfHz(2'000.0) != 7'018'700ULL) {
    return 52;
  }
  frequency_mapping.setSourceMode(2);
  if (frequency_mapping.rfFrequencyToDisplayHz(7'021'430ULL) != 7'021'430.0 ||
      frequency_mapping.displayFrequencyToRfHz(7'021'430.4) != 7'021'430ULL) {
    return 53;
  }
  frequency_mapping.setSourceMode(1);
  if (std::isfinite(
          frequency_mapping.rfFrequencyToDisplayHz(7'021'430ULL)) ||
      frequency_mapping.displayFrequencyToRfHz(700.0) != 0U) {
    return 54;
  }
  cwassistant::core::OfflineCallsignDatabase callsign_database;
  const auto callsign_import =
      callsign_database.importText("EM90ZMV\nEA1EYL\nSV2HQL\nNV2HQD\n");
  QVariantMap acoustic_channel{
      {QStringLiteral("verifiedCw"), true},
      {QStringLiteral("callsign"), QString{}},
      {QStringLiteral("refinedText"), QStringLiteral("CQ DE EM?0ZMV ")},
      {QStringLiteral("text"), QString{}},
      {QStringLiteral("acousticAlternatives"),
       QVariantList{
           QVariantMap{{QStringLiteral("text"),
                       QStringLiteral("CQ DE EM90ZMV")},
                       {QStringLiteral("cost"), 1.0},
                       {QStringLiteral("confidence"), 0.68},
                       {QStringLiteral("firstObservationId"),
                        QVariant::fromValue<qulonglong>(10)},
                       {QStringLiteral("lastObservationId"),
                        QVariant::fromValue<qulonglong>(30)}},
           QVariantMap{{QStringLiteral("text"),
                        QStringLiteral("DE EM90ZMV")},
                       {QStringLiteral("cost"), 1.2},
                       {QStringLiteral("confidence"), 0.61},
                       {QStringLiteral("firstObservationId"),
                        QVariant::fromValue<qulonglong>(10)},
                       {QStringLiteral("lastObservationId"),
                        QVariant::fromValue<qulonglong>(30)}}}},
  };
  QString suggestion_diagnostic;
  const auto offline_suggestion =
      cwassistant::desktop::advisoryCallsignPresentation(
          acoustic_channel, callsign_database, &suggestion_diagnostic);
  if (!callsign_import.accepted || callsign_import.inserted_records != 4U ||
      !offline_suggestion ||
      offline_suggestion->callsign != QStringLiteral("EM90ZMV") ||
      offline_suggestion->raw_span != QStringLiteral("EM?0ZMV") ||
      !offline_suggestion->database_match ||
      offline_suggestion->agreeing_alternatives != 2 ||
      suggestion_diagnostic != QStringLiteral("suggested-database")) {
    return 23;
  }
  if (acoustic_channel.value(QStringLiteral("refinedText")).toString() !=
          QStringLiteral("CQ DE EM?0ZMV ") ||
      !acoustic_channel.value(QStringLiteral("callsign")).toString().isEmpty() ||
      !acoustic_channel.value(QStringLiteral("verifiedCw")).toBool()) {
    return 24;
  }
  QVariantMap ineligible_channel = acoustic_channel;
  ineligible_channel.insert(QStringLiteral("verifiedCw"), false);
  if (cwassistant::desktop::advisoryCallsignPresentation(
          ineligible_channel, callsign_database)) {
    return 25;
  }
  ineligible_channel = acoustic_channel;
  ineligible_channel.insert(QStringLiteral("callsign"),
                            QStringLiteral("EM90ZMV"));
  if (cwassistant::desktop::advisoryCallsignPresentation(
          ineligible_channel, callsign_database)) {
    return 26;
  }
  QVariantMap stronger_unknown = acoustic_channel;
  stronger_unknown.insert(
      QStringLiteral("acousticAlternatives"),
      QVariantList{
          QVariantMap{{QStringLiteral("text"), QStringLiteral("EM80ZMV")},
                      {QStringLiteral("cost"), 0.0},
                      {QStringLiteral("confidence"), 0.92},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(31)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(50)}},
          QVariantMap{{QStringLiteral("text"), QStringLiteral("EM80ZMV")},
                      {QStringLiteral("cost"), 0.1},
                      {QStringLiteral("confidence"), 0.88},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(31)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(50)}},
          QVariantMap{{QStringLiteral("text"), QStringLiteral("EM90ZMV")},
                      {QStringLiteral("cost"), 0.4},
                      {QStringLiteral("confidence"), 0.55},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(31)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(50)}},
          QVariantMap{{QStringLiteral("text"), QStringLiteral("EM90ZMV")},
                      {QStringLiteral("cost"), 0.5},
                      {QStringLiteral("confidence"), 0.52},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(31)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(50)}}});
  const auto stronger_acoustic =
      cwassistant::desktop::advisoryCallsignPresentation(
          stronger_unknown, callsign_database);
  if (!stronger_acoustic ||
      stronger_acoustic->callsign != QStringLiteral("EM80ZMV") ||
      stronger_acoustic->database_match) {
    return 27;
  }
  // The same stream with directory correction enabled. The operator opts into
  // substituting the single listed entry within two edits of the acoustic
  // winner, which is what recovers a callsign whose opening characters were
  // lost, and is why it is off by default: EM80ZMV and EM90ZMV are both
  // plausible stations one character apart.
  QString corrected_reason;
  const auto stronger_corrected =
      cwassistant::desktop::advisoryCallsignPresentation(
          stronger_unknown, callsign_database, &corrected_reason, true);
  if (!stronger_corrected ||
      stronger_corrected->callsign != QStringLiteral("EM90ZMV") ||
      !stronger_corrected->database_match ||
      !stronger_corrected->database_corrected ||
      corrected_reason != QStringLiteral("suggested-database-corrected")) {
    return 41;
  }
  // An acoustic winner that is already listed is never substituted, whether or
  // not correction is enabled.
  QVariantMap listed_unknown = stronger_unknown;
  listed_unknown.insert(QStringLiteral("refinedText"),
                        QStringLiteral("CQ DE EM90ZMV "));
  const auto listed_exact =
      cwassistant::desktop::advisoryCallsignPresentation(
          listed_unknown, callsign_database, nullptr, true);
  if (listed_exact && listed_exact->database_corrected) return 42;

  QVariantMap corrected_fixed_character = acoustic_channel;
  corrected_fixed_character.insert(QStringLiteral("refinedText"),
                                   QStringLiteral("CQ DE SV2?L? "));
  corrected_fixed_character.insert(
      QStringLiteral("acousticAlternatives"),
      QVariantList{
          QVariantMap{{QStringLiteral("text"), QStringLiteral("SV2HQL AIPX")},
                      {QStringLiteral("cost"), 38.68},
                      {QStringLiteral("confidence"), 0.482},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(61)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(80)}},
          QVariantMap{{QStringLiteral("text"), QStringLiteral("DV2HQL AIPX")},
                      {QStringLiteral("cost"), 38.91},
                      {QStringLiteral("confidence"), 0.482},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(61)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(80)}},
          QVariantMap{{QStringLiteral("text"),
                       QStringLiteral("SV2HQL AIPX NV?HQD")},
                      {QStringLiteral("cost"), 38.99},
                      {QStringLiteral("confidence"), 0.482},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(61)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(80)}},
          QVariantMap{{QStringLiteral("text"),
                       QStringLiteral("SV2HQLAIPX NV2HQD")},
                      {QStringLiteral("cost"), 39.21},
                      {QStringLiteral("confidence"), 0.482},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(61)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(80)}}});
  const auto corrected_fixed_suggestion =
      cwassistant::desktop::advisoryCallsignPresentation(
          corrected_fixed_character, callsign_database);
  if (!corrected_fixed_suggestion ||
      corrected_fixed_suggestion->callsign != QStringLiteral("SV2HQL") ||
      corrected_fixed_suggestion->raw_span != QStringLiteral("SV2?L?") ||
      !corrected_fixed_suggestion->database_match) {
    return 33;
  }
  cwassistant::core::OfflineCallsignDatabase empty_callsign_database;
  suggestion_diagnostic.clear();
  const auto acoustic_only_suggestion =
      cwassistant::desktop::advisoryCallsignPresentation(
          corrected_fixed_character, empty_callsign_database,
          &suggestion_diagnostic);
  if (!acoustic_only_suggestion ||
      acoustic_only_suggestion->callsign != QStringLiteral("SV2HQL") ||
      acoustic_only_suggestion->database_match ||
      suggestion_diagnostic != QStringLiteral("suggested-acoustic")) {
    return 35;
  }
  QVariantMap recovered_boundary = corrected_fixed_character;
  recovered_boundary.insert(QStringLiteral("refinedText"),
                            QStringLiteral("CQ NV?D "));
  QVariantList boundary_alternatives =
      recovered_boundary.value(QStringLiteral("acousticAlternatives")).toList();
  for (QVariant& value : boundary_alternatives) {
    QVariantMap alternative = value.toMap();
    alternative.insert(QStringLiteral("text"), QStringLiteral("NV2HQD"));
    value = alternative;
  }
  recovered_boundary.insert(QStringLiteral("acousticAlternatives"),
                            boundary_alternatives);
  const auto boundary_suggestion =
      cwassistant::desktop::advisoryCallsignPresentation(
          recovered_boundary, callsign_database);
  if (!boundary_suggestion ||
      boundary_suggestion->callsign != QStringLiteral("NV2HQD") ||
      boundary_suggestion->raw_span != QStringLiteral("NV?D")) {
    return 34;
  }
  QVariantMap old_span = acoustic_channel;
  old_span.insert(QStringLiteral("refinedText"),
                  QStringLiteral("EA?EYL EM?0ZMV "));
  old_span.insert(
      QStringLiteral("acousticAlternatives"),
      QVariantList{
          QVariantMap{{QStringLiteral("text"), QStringLiteral("EA1EYL")},
                      {QStringLiteral("cost"), 0.0},
                      {QStringLiteral("confidence"), 0.9},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(51)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(60)}},
          QVariantMap{{QStringLiteral("text"), QStringLiteral("EA1EYL")},
                      {QStringLiteral("cost"), 0.1},
                      {QStringLiteral("confidence"), 0.85},
                      {QStringLiteral("firstObservationId"),
                       QVariant::fromValue<qulonglong>(51)},
                      {QStringLiteral("lastObservationId"),
                       QVariant::fromValue<qulonglong>(60)}}});
  if (cwassistant::desktop::advisoryCallsignPresentation(
          old_span, callsign_database)) {
    return 29;
  }
  acoustic_channel.insert(QStringLiteral("acousticAlternatives"),
                          QVariantList{});
  suggestion_diagnostic.clear();
  if (cwassistant::desktop::advisoryCallsignPresentation(
          acoustic_channel, callsign_database, &suggestion_diagnostic) ||
      suggestion_diagnostic !=
          QStringLiteral("fewer-than-two-acoustic-alternatives")) {
    return 28;
  }
  QTemporaryFile local_callsign_file;
  if (!local_callsign_file.open() ||
      local_callsign_file.write("EM90ZMV\nEA1EYL\n") <= 0 ||
      !local_callsign_file.flush()) {
    return 30;
  }
  cwassistant::desktop::ReplayController controller;
  int inactive_presentation_publications = 0;
  QObject::connect(
      &controller,
      &cwassistant::desktop::ReplayController::livePresentationDiagnosticsRequested,
      &controller, [&inactive_presentation_publications](const QVariantMap&) {
        ++inactive_presentation_publications;
      });
  controller.configureOfflineCallsignDatabase(
      true, local_callsign_file.fileName());
  if (controller.offlineCallsignDatabaseState() != QStringLiteral("ready") ||
      controller.offlineCallsignDatabaseEntries() != 2) {
    return 31;
  }

  // Rapid decoder updates must not destroy and recreate a card delegate. In
  // particular, a low-level tracker reacquisition at the same reserved colour
  // and frequency is still the same operator-visible session; resetting the
  // list in the middle of a pointer gesture used to lose monitor clicks.
  cwassistant::desktop::DecoderSessionListModel session_model;
  int session_model_resets = 0;
  int session_model_updates = 0;
  QObject::connect(&session_model, &QAbstractItemModel::modelReset,
                   &session_model,
                   [&session_model_resets] { ++session_model_resets; });
  QObject::connect(
      &session_model, &QAbstractItemModel::dataChanged, &session_model,
      [&session_model_updates](const QModelIndex&, const QModelIndex&,
                               const QList<int>&) {
        ++session_model_updates;
      });
  QVariantMap stable_session{
      {QStringLiteral("id"), QVariant::fromValue<qulonglong>(700)},
      {QStringLiteral("color"), QStringLiteral("#4dd0e1")},
      {QStringLiteral("presentationFrequencyHz"), 702.0},
      {QStringLiteral("callsign"), QStringLiteral("AM42SDC")}};
  session_model.replace(QVariantList{stable_session});
  if (session_model_resets != 1 || session_model.rowCount() != 1) return 43;
  stable_session.insert(QStringLiteral("active"), true);
  session_model.replace(QVariantList{stable_session});
  stable_session.insert(QStringLiteral("id"),
                        QVariant::fromValue<qulonglong>(701));
  stable_session.insert(QStringLiteral("presentationFrequencyHz"), 718.0);
  session_model.replace(QVariantList{stable_session});
  if (session_model_resets != 1 || session_model_updates != 2 ||
      session_model.data(session_model.index(0, 0), Qt::DisplayRole)
              .toMap()
              .value(QStringLiteral("id"))
              .toULongLong() != 701) {
    return 44;
  }
  stable_session.insert(QStringLiteral("id"),
                        QVariant::fromValue<qulonglong>(702));
  stable_session.insert(QStringLiteral("color"), QStringLiteral("#ffb74d"));
  session_model.replace(QVariantList{stable_session});
  if (session_model_resets != 2) return 45;

  controller.configureOfflineCallsignDatabase(false, QString{});
  if (controller.offlineCallsignDatabaseState() !=
          QStringLiteral("disabled") ||
      controller.offlineCallsignDatabaseEntries() != 0 ||
      inactive_presentation_publications != 0) {
    return 32;
  }
  using cwassistant::desktop::freshCharacterRefinementCallEvidence;
  if (freshCharacterRefinementCallEvidence("NOISE", 0U) ||
      !freshCharacterRefinementCallEvidence("4X5L", 0U) ||
      freshCharacterRefinementCallEvidence("4X5LL ", 5U) ||
      freshCharacterRefinementCallEvidence("4X5LL", 4U) ||
      !freshCharacterRefinementCallEvidence("4X5L", 3U) ||
      freshCharacterRefinementCallEvidence("4X5LL HEL", 6U) ||
      freshCharacterRefinementCallEvidence("CQ 4X5LL A", 8U) ||
      !freshCharacterRefinementCallEvidence("4X5LL 4X5L", 6U) ||
      freshCharacterRefinementCallEvidence("4X5LL", 5U)) {
    return 22;
  }
  cwassistant::core::CwChannelSnapshot selected_snapshot{
      .id = 5,
      .frequency_hz = 850.0,
      .presentation_frequency_hz = 850.0,
      .verified_cw = false,
      .operator_selected = true,
      .characters = {{.symbol = "A", .confidence = 0.8F,
                      .timing_quality = 0.8F, .known = true}},
      .text = "UNVERIFIED",
      .refined_text = "EA1EYL ",
      .provisional_text = "NO",
      .pending_elements = ".-",
      .callsign = "IU0LFQ",
  };
  QVariantMap selected_model =
      cwassistant::desktop::decoderChannelModel(
          std::span<const cwassistant::core::CwChannelSnapshot>{
              &selected_snapshot, 1})
          .front().toMap();
  if (!selected_model.value(QStringLiteral("operatorSelected")).toBool() ||
      selected_model.value(QStringLiteral("verifiedCw")).toBool() ||
      !selected_model.value(QStringLiteral("text")).toString().isEmpty() ||
      !selected_model.value(QStringLiteral("refinedText")).toString().isEmpty() ||
      !selected_model.value(QStringLiteral("acousticAlternatives")).toList().isEmpty() ||
      !selected_model.value(QStringLiteral("provisionalText")).toString().isEmpty() ||
      !selected_model.value(QStringLiteral("elements")).toString().isEmpty() ||
      !selected_model.value(QStringLiteral("callsign")).toString().isEmpty() ||
      !selected_model.value(QStringLiteral("localModelText")).toString().isEmpty() ||
      !selected_model.value(QStringLiteral("localModelCallsign")).toString().isEmpty() ||
      selected_model.value(QStringLiteral("localModelState")).toString() !=
          QStringLiteral("unavailable") ||
      !selected_model.value(QStringLiteral("characterEvidence")).toList().isEmpty() ||
      selected_model.value(QStringLiteral("color")).toString() !=
          QStringLiteral("#8d9aaa")) {
    return 18;
  }
  const cwassistant::desktop::LocalDecoderChannelPresentation local_decoder{
      .channel_id = 5,
      .state = cwassistant::desktop::LocalDecoderPresentationState::Ready,
      .stable_text = QStringLiteral("CQ TEST "),
      .status = QStringLiteral("Stable local transcript"),
  };
  selected_model =
      cwassistant::desktop::decoderChannelModel(
          std::span<const cwassistant::core::CwChannelSnapshot>{
              &selected_snapshot, 1},
          std::span<const cwassistant::desktop::LocalDecoderChannelPresentation>{
              &local_decoder, 1})
          .front().toMap();
  if (!selected_model.value(QStringLiteral("localModelText")).toString().isEmpty() ||
      !selected_model.value(QStringLiteral("localModelCallsign")).toString().isEmpty() ||
      selected_model.value(QStringLiteral("localModelState")).toString() !=
          QStringLiteral("ready")) {
    return 20;
  }
  selected_snapshot.verified_cw = true;
  selected_model =
      cwassistant::desktop::decoderChannelModel(
          std::span<const cwassistant::core::CwChannelSnapshot>{
              &selected_snapshot, 1},
          std::span<const cwassistant::desktop::LocalDecoderChannelPresentation>{
              &local_decoder, 1})
          .front().toMap();
  if (selected_model.value(QStringLiteral("text")).toString() !=
          QStringLiteral("UNVERIFIED") ||
      selected_model.value(QStringLiteral("refinedText")).toString() !=
          QStringLiteral("EA1EYL ") ||
      selected_model.value(QStringLiteral("callsign")).toString() !=
          QStringLiteral("IU0LFQ") ||
      selected_model.value(QStringLiteral("localModelText")).toString() !=
          QStringLiteral("CQ TEST ") ||
      selected_model.value(QStringLiteral("localModelState")).toString() !=
          QStringLiteral("ready") ||
      selected_model.value(QStringLiteral("localModelStatus")).toString() !=
          QStringLiteral("Stable local transcript") ||
      selected_model.value(QStringLiteral("characterEvidence")).toList().size() != 1 ||
      selected_model.value(QStringLiteral("color")).toString() ==
          QStringLiteral("#8d9aaa")) {
    return 19;
  }
  // A measurement that moves below what the card can display must leave the
  // row byte-identical, so the session list reports no change and the view
  // does not rebuild a decoded card -- transcript text layout included -- for
  // a signal-to-noise reading that wandered a thousandth of a decibel.
  //
  // This is the whole of the "jerky with several streams" fault. Every one of
  // these fields moves on every update, a row compares unequal if any single
  // field differs, so before this rounding the model reported all two dozen
  // rows changed on all two dozen publications a second and the drawing
  // thread never stopped rebuilding cards -- while the processor sat idle,
  // which is why it never looked like a load problem. Measured on a running
  // station: narrowband coherence, keying-level separation and explained
  // variation each differed on 100% of consecutive samples.
  //
  // Base values are exact multiples of their rounding step and the drifts are
  // a small fraction of one step, so a row that still differs means some
  // field reached the model unrounded.
  const auto quantisation_snapshot =
      [](const double drift) -> cwassistant::core::CwChannelSnapshot {
    const auto coarse = static_cast<float>(drift);       // 0.1 steps
    const auto fine = static_cast<float>(drift / 10.0);  // 0.01 steps
    return cwassistant::core::CwChannelSnapshot{
        .id = 900,
        .frequency_hz = 800.0 + drift,
        .presentation_frequency_hz = 800.0 + drift,
        .drift_hz_per_second = 1.0 + drift,
        .filter_width_hz = 120.0 + 10.0 * drift,
        .snr_db = 12.0F + coarse,
        .wpm = 22.0 + drift,
        .acoustic_wpm = 22.0 + drift,
        .acoustic_cadence_confidence = 0.5F + fine,
        .confidence = 0.5F + fine,
        .key_down_probability = 0.5F + fine,
        .verified_cw = true,
        .verification_confidence = 0.5F + fine,
        .verification_cadence_quality = 0.5F + fine,
        .verification_timing_quality = 0.5F + fine,
        .verification_character_confidence = 0.5F + fine,
        .cadence_quality = 0.5F + fine,
        .mean_character_confidence = 0.5F + fine,
        .narrowband_coherence = 0.5F + fine,
        .characters = {{.symbol = "A",
                        .confidence = 0.5F + fine,
                        .timing_quality = 0.5F + fine,
                        .known = true}},
        .text = "CQ",
        .refined_text = "CQ ",
        .acoustic_alternatives = {{.text = "CQ",
                                   .provisional_elements = ".-",
                                   .wpm = 22.0 + drift,
                                   .acoustic_cost = 3.0 + drift,
                                   .evidence_confidence = 0.5F + fine,
                                   .first_observation_id = 1,
                                   .last_observation_id = 2}},
        .provisional_text = "C",
        .pending_elements = ".-",
        .transmissions = {{.sequence = 1,
                           .text = "CQ",
                           .sender_callsign = "IU0LFQ",
                           .wpm = 22.0 + drift,
                           .cadence_confidence = 0.5F + fine}},
        .sender_cadences = {{.callsign = "IU0LFQ",
                             .wpm = 22.0 + drift,
                             .confidence = 0.5F + fine,
                             .observed_turns = 2}},
        .active_transmission_sequence = 1,
        .current_sender_callsign = "IU0LFQ",
        .current_sender_wpm = 22.0 + drift,
    };
  };
  const auto quantised_row =
      [&quantisation_snapshot](const double drift) -> QVariantMap {
    const cwassistant::core::CwChannelSnapshot snapshot =
        quantisation_snapshot(drift);
    return cwassistant::desktop::decoderChannelModel(
               std::span<const cwassistant::core::CwChannelSnapshot>{
                   &snapshot, 1})
        .front()
        .toMap();
  };
  const QVariantMap settled_row = quantised_row(0.0);
  if (quantised_row(0.004) != settled_row ||
      quantised_row(-0.004) != settled_row) {
    return 46;
  }
  // The rounding must not be so coarse that a change the operator would see
  // is swallowed. Half a decibel, half a word per minute and five hundredths
  // of confidence all still report.
  if (quantised_row(0.5) == settled_row) return 47;
  {
    cwassistant::core::CwChannelSnapshot louder = quantisation_snapshot(0.0);
    louder.snr_db = 12.5F;
    const QVariantMap louder_row =
        cwassistant::desktop::decoderChannelModel(
            std::span<const cwassistant::core::CwChannelSnapshot>{&louder, 1})
            .front()
            .toMap();
    if (louder_row == settled_row) return 48;
    cwassistant::core::CwChannelSnapshot surer = quantisation_snapshot(0.0);
    surer.confidence = 0.55F;
    const QVariantMap surer_row =
        cwassistant::desktop::decoderChannelModel(
            std::span<const cwassistant::core::CwChannelSnapshot>{&surer, 1})
            .front()
            .toMap();
    if (surer_row == settled_row) return 49;
  }

  const QVariantMap previous_session{
      {QStringLiteral("id"), QVariant::fromValue<qulonglong>(7)},
      {QStringLiteral("color"), QStringLiteral("#4dd0e1")},
      {QStringLiteral("audioFrequencyHz"), 700.0},
      {QStringLiteral("presentationFrequencyHz"), 700.0},
  };
  const QVariantMap reacquired_channel{
      {QStringLiteral("id"), QVariant::fromValue<qulonglong>(19)},
      {QStringLiteral("color"), QStringLiteral("#4dd0e1")},
      {QStringLiteral("audioFrequencyHz"), 742.0},
      {QStringLiteral("presentationFrequencyHz"), 706.0},
  };
  const QList<qulonglong> reconciled =
      cwassistant::desktop::reconcileDecoderSessionOrder(
          QList<qulonglong>{7}, QVariantList{previous_session},
          QVariantList{reacquired_channel});
  if (reconciled != QList<qulonglong>{19}) return 15;
  const QList<qulonglong> dismissed =
      cwassistant::desktop::reconcileDecoderSessionOrder(
          {}, QVariantList{previous_session}, QVariantList{reacquired_channel});
  if (!dismissed.isEmpty()) return 17;

  cwassistant::desktop::WaterfallConditioner conditioner;
  QVector<float> shaped_noise(128);
  for (qsizetype index = 0; index < shaped_noise.size(); ++index)
    shaped_noise[index] = -92.0F + 0.08F * static_cast<float>(index);
  static_cast<void>(conditioner.process(
      shaped_noise, true, 6.0, -110.0, -40.0, 3.0, -90.0));
  QVector<float> keyed = shaped_noise;
  for (qsizetype index = 62; index <= 66; ++index)
    keyed[index] += 24.0F;
  const QVector<float> isolated = conditioner.process(
      keyed, true, 6.0, -110.0, -40.0, 3.0, -90.0);
  int bright_bins = 0;
  for (const float value : isolated) {
    if (value > -75.0F) ++bright_bins;
  }
  if (bright_bins < 5 || bright_bins > 9 || isolated[64] < -60.0F ||
      isolated[20] > -100.0F) {
    return 11;
  }
  QVariantMap keyed_channel{
      {QStringLiteral("verifiedCw"), true},
      {QStringLiteral("active"), true},
      {QStringLiteral("keyDown"), true},
      {QStringLiteral("frequencyHz"), 700.0},
  };
  const QVector<float> symbol_row = cwassistant::desktop::cwSymbolRow(
      QVariantList{keyed_channel}, 101, 200.0, 1'200.0, -110.0, -40.0);
  int symbol_bins = 0;
  for (const float value : symbol_row) {
    if (value > -50.0F) ++symbol_bins;
  }
  if (symbol_bins != 3 || symbol_row[50] < -50.0F ||
      symbol_row[20] > -100.0F) {
    return 13;
  }
  keyed_channel.insert(QStringLiteral("keyDown"), false);
  const QVector<float> gap_row = cwassistant::desktop::cwSymbolRow(
      QVariantList{keyed_channel}, 101, 200.0, 1'200.0, -110.0, -40.0);
  if (std::any_of(gap_row.cbegin(), gap_row.cend(),
                  [](const float value) { return value > -100.0F; })) {
    return 14;
  }
  keyed_channel.insert(QStringLiteral("keyDown"), true);
  keyed_channel.insert(QStringLiteral("active"), false);
  const QVector<float> retained_noise_row =
      cwassistant::desktop::cwSymbolRow(
          QVariantList{keyed_channel}, 101, 200.0, 1'200.0, -110.0, -40.0);
  if (std::any_of(retained_noise_row.cbegin(), retained_noise_row.cend(),
                  [](const float value) { return value > -100.0F; })) {
    return 16;
  }
  TestableSpectrumWaterfallItem item;
  item.setWidth(960.0);
  item.setHeight(540.0);
  item.setAutomaticRangeSpanDb(60.0);
  item.setNoiseSuppression(true);
  item.setNoiseMarginDb(6.0);
  item.setWaterfallRate(30);
  item.setWaterfallTimeSpanSeconds(15);
  if (item.waterfallRowCapacity() != 450 ||
      item.waterfallTimeSpanSeconds() != 15) {
    return 2;
  }

  cwassistant::desktop::SpectrumFrame noise_frame{
      .bins_dbfs = QVector<float>(128, -90.0F),
      .sequence = 1,
      .timestamp_ns = 1'000'000'000,
      .lower_frequency_hz = 100.0,
      .upper_frequency_hz = 3'000.0,
      .instantaneous_bins_dbfs = QVector<float>(128, -95.0F),
  };
  item.acceptFrame(noise_frame);
  if (item.effectiveUpperBoundDb() - item.effectiveLowerBoundDb() < 59.9 ||
      std::abs(item.estimatedNoiseFloorDb() + 90.0) > 0.1) {
    return 3;
  }
  if (item.storedWaterfallRows() != 1) return 4;

  const double first_ceiling = item.effectiveUpperBoundDb();
  noise_frame.bins_dbfs.fill(-80.0F);
  noise_frame.sequence = 2;
  noise_frame.timestamp_ns = 2'000'000'000;
  item.acceptFrame(noise_frame);
  if (item.effectiveUpperBoundDb() - first_ceiling > 3.0) {
    return 5;
  }
  if (item.storedWaterfallRows() != 31) return 6;

  item.setDisplayMode(1);
  if (item.displayMode() != 1 || item.storedWaterfallRows() != 0) return 8;
  noise_frame.sequence = 3;
  noise_frame.timestamp_ns = 2'100'000'000;
  noise_frame.instantaneous_bins_dbfs.fill(-70.0F);
  item.acceptFrame(noise_frame);
  if (item.storedWaterfallRows() != 1) return 9;
  item.setDisplayMode(99);
  if (item.displayMode() != 1) return 10;

  QSGNode* node = item.updatePaintNode(nullptr, nullptr);
  if (node == nullptr) return 7;

  node = item.updatePaintNode(node, nullptr);
  delete node;

  return 0;
}
