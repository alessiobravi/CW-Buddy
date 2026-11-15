#include "cwassistant/core/cw_character_lane_frontend.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace cwassistant::core {
namespace {

bool powerOfTwo(const std::size_t value) noexcept {
  return value > 0U && (value & (value - 1U)) == 0U;
}

std::uint64_t samplesToNanoseconds(const double samples,
                                   const double sample_rate_hz) noexcept {
  return static_cast<std::uint64_t>(std::llround(
      samples * 1'000'000'000.0 / sample_rate_hz));
}

// How many steps either oscillator may take before its unit magnitude is
// restored. Counted on the stream, not on the current block: both
// oscillators persist across blocks, so a counter that restarted with every
// block measured the wrong clock and never reached its period at all on a
// path whose blocks are shorter than it -- the direct-SDR path batches
// decimated samples at a threshold of 512. A float step is not exactly unit
// magnitude once cos and sin are rounded, and the error compounds: measured
// on the 700 Hz at 48 kHz step, whose magnitude is 1 - 2.2e-8, |oscillator|
// falls to 0.978 after a million multiplies and 0.804 after ten million,
// which scales every feature this lane emits by the same factor.
constexpr std::size_t kOscillatorRenormalizeSteps = 1'024;

}  // namespace

CwCharacterLaneFrontend::CwCharacterLaneFrontend(
    CwCharacterFeatureContract contract) {
  static_cast<void>(configure(contract));
}

bool CwCharacterLaneFrontend::configure(
    CwCharacterFeatureContract contract) {
  const bool finite = std::isfinite(contract.working_sample_rate_hz) &&
      std::isfinite(contract.minimum_frequency_hz) &&
      std::isfinite(contract.maximum_frequency_hz) &&
      std::isfinite(contract.isolated_tone_hz) &&
      std::isfinite(contract.lane_bandwidth_hz) &&
      std::isfinite(contract.window_seconds) &&
      std::isfinite(contract.stride_seconds);
  if (!finite || contract.working_sample_rate_hz < 1'600.0 ||
      contract.working_sample_rate_hz > 8'000.0 ||
      !powerOfTwo(contract.fft_length) || contract.fft_length < 128U ||
      contract.fft_length > 512U || contract.hop_length < 8U ||
      contract.hop_length > contract.fft_length ||
      contract.minimum_frequency_hz < 0.0 ||
      contract.maximum_frequency_hz <= contract.minimum_frequency_hz ||
      contract.maximum_frequency_hz >
          contract.working_sample_rate_hz * 0.5 ||
      contract.isolated_tone_hz < contract.minimum_frequency_hz ||
      contract.isolated_tone_hz > contract.maximum_frequency_hz ||
      contract.lane_bandwidth_hz < 30.0 ||
      contract.lane_bandwidth_hz > 50.0 ||
      contract.lane_filter_taps < 129U ||
      contract.lane_filter_taps > 1'025U ||
      (contract.lane_filter_taps & 1U) == 0U ||
      contract.window_seconds < 5.0 || contract.window_seconds > 12.0 ||
      contract.stride_seconds < 0.25 ||
      contract.stride_seconds > contract.window_seconds * 0.5) {
    valid_ = false;
    return false;
  }
  const double bin_hz = contract.working_sample_rate_hz /
                        static_cast<double>(contract.fft_length);
  const std::size_t first = static_cast<std::size_t>(
      std::ceil(contract.minimum_frequency_hz / bin_hz));
  const std::size_t last = static_cast<std::size_t>(
      std::floor(contract.maximum_frequency_hz / bin_hz));
  const std::size_t bins = last >= first ? last - first + 1U : 0U;
  const std::size_t window_frames = static_cast<std::size_t>(std::llround(
      contract.window_seconds * contract.working_sample_rate_hz /
      static_cast<double>(contract.hop_length)));
  const std::size_t stride_frames = static_cast<std::size_t>(std::llround(
      contract.stride_seconds * contract.working_sample_rate_hz /
      static_cast<double>(contract.hop_length)));
  if (bins == 0U || bins > 129U || window_frames < 2U ||
      window_frames > 2'048U || stride_frames == 0U ||
      bins * window_frames > 150'000U) {
    valid_ = false;
    return false;
  }
  contract_ = contract;
  first_frequency_bin_ = first;
  frequency_bin_count_ = bins;
  window_frame_count_ = window_frames;
  stride_frame_count_ = stride_frames;
  rebuild();
  valid_ = true;
  return true;
}

void CwCharacterLaneFrontend::rebuild() {
  lane_filter_taps_.resize(contract_.lane_filter_taps);
  const double center = static_cast<double>(contract_.lane_filter_taps - 1U) /
                        2.0;
  const double cutoff = contract_.lane_bandwidth_hz * 0.5 /
                        contract_.working_sample_rate_hz;
  double sum = 0.0;
  for (std::size_t index = 0; index < lane_filter_taps_.size(); ++index) {
    const double offset = static_cast<double>(index) - center;
    const double sinc = offset == 0.0
        ? 2.0 * cutoff
        : std::sin(2.0 * std::numbers::pi * cutoff * offset) /
              (std::numbers::pi * offset);
    const double phase = 2.0 * std::numbers::pi *
                         static_cast<double>(index) /
                         static_cast<double>(lane_filter_taps_.size() - 1U);
    const double blackman = 0.42 - 0.5 * std::cos(phase) +
                            0.08 * std::cos(2.0 * phase);
    lane_filter_taps_[index] = static_cast<float>(sinc * blackman);
    sum += lane_filter_taps_[index];
  }
  if (sum != 0.0) {
    for (float& tap : lane_filter_taps_)
      tap = static_cast<float>(static_cast<double>(tap) / sum);
  }
  lane_filter_history_.assign(contract_.lane_filter_taps, {});
  stft_window_.resize(contract_.fft_length);
  stft_history_.assign(contract_.fft_length, 0.0F);
  for (std::size_t index = 0; index < stft_window_.size(); ++index) {
    stft_window_[index] = static_cast<float>(
        0.5 - 0.5 * std::cos(2.0 * std::numbers::pi *
                            static_cast<double>(index) /
                            static_cast<double>(stft_window_.size())));
  }
  feature_ring_.assign(window_frame_count_ * frequency_bin_count_, 0.0F);
  timestamp_ring_.assign(window_frame_count_, 0U);
  pending_features_.assign(feature_ring_.size(), 0.0F);
  pending_timestamps_.assign(timestamp_ring_.size(), 0U);
  resetSignalState();
}

void CwCharacterLaneFrontend::reset(
    CwCharacterTrackKey track, const double center_frequency_hz) noexcept {
  track_ = track;
  center_frequency_hz_ = std::isfinite(center_frequency_hz)
      ? center_frequency_hz : 0.0;
  dropped_windows_ = 0;
  resetSignalState();
}

void CwCharacterLaneFrontend::resetSignalState() noexcept {
  center_oscillator_ = {1.0F, 0.0F};
  feature_oscillator_ = {1.0F, 0.0F};
  center_oscillator_steps_ = 0;
  feature_oscillator_steps_ = 0;
  anti_alias_filter_ = {};
  resample_accumulator_ = 0.0;
  std::fill(lane_filter_history_.begin(), lane_filter_history_.end(),
            std::complex<float>{});
  lane_filter_index_ = 0;
  std::fill(stft_history_.begin(), stft_history_.end(), 0.0F);
  stft_index_ = 0;
  stft_count_ = 0;
  samples_since_frame_ = 0;
  std::fill(feature_ring_.begin(), feature_ring_.end(), 0.0F);
  std::fill(timestamp_ring_.begin(), timestamp_ring_.end(), 0U);
  feature_ring_index_ = 0;
  feature_ring_count_ = 0;
  total_feature_frames_ = 0;
  pending_sequence_ = 0;
  output_sequence_ = 0;
  expected_input_timestamp_ns_ = 0;
  pending_window_ = false;
  stream_initialized_ = false;
  timing_initialized_ = false;
}

void CwCharacterLaneFrontend::setCenterFrequency(
    const double center_frequency_hz) noexcept {
  if (std::isfinite(center_frequency_hz))
    center_frequency_hz_ = center_frequency_hz;
}

void CwCharacterLaneFrontend::process(const RealtimeSampleBlock& block) {
  if (!valid_ || track_.track_id == 0 || block.sample_count == 0U ||
      block.sample_count > block.samples.size() ||
      !std::isfinite(block.stream.sample_rate_hz) ||
      block.stream.sample_rate_hz < contract_.working_sample_rate_hz) {
    return;
  }
  const bool same_stream = stream_initialized_ &&
      stream_.kind == block.stream.kind &&
      stream_.sample_rate_hz == block.stream.sample_rate_hz &&
      stream_.center_frequency_hz == block.stream.center_frequency_hz;
  bool discontinuity = stream_initialized_ && !same_stream;
  if (same_stream && timing_initialized_) {
    const auto difference = block.timestamp_ns > expected_input_timestamp_ns_
        ? block.timestamp_ns - expected_input_timestamp_ns_
        : expected_input_timestamp_ns_ - block.timestamp_ns;
    const auto tolerance = samplesToNanoseconds(
        2.0, block.stream.sample_rate_hz);
    discontinuity = difference > tolerance;
  }
  if (!stream_initialized_) {
    stream_ = block.stream;
    stream_initialized_ = true;
  } else if (discontinuity) {
    resetSignalState();
    ++track_.frontend_generation;
    stream_ = block.stream;
    stream_initialized_ = true;
  }

  const double input_rate = block.stream.sample_rate_hz;
  const double mixed_frequency = block.stream.kind == StreamKind::Audio
      ? center_frequency_hz_
      : center_frequency_hz_ - block.stream.center_frequency_hz;
  const double center_angle = -2.0 * std::numbers::pi * mixed_frequency /
                              input_rate;
  const std::complex<float> center_step{
      static_cast<float>(std::cos(center_angle)),
      static_cast<float>(std::sin(center_angle))};
  const double anti_alias_cutoff = std::min(
      contract_.working_sample_rate_hz * 0.32, input_rate * 0.20);
  const float alpha = static_cast<float>(1.0 - std::exp(
      -2.0 * std::numbers::pi * anti_alias_cutoff / input_rate));
  for (std::size_t index = 0; index < block.sample_count; ++index) {
    const auto input = block.stream.kind == StreamKind::Audio
        ? std::complex<float>(block.samples[index].real(), 0.0F)
        : block.samples[index];
    const auto mixed = input * center_oscillator_;
    anti_alias_filter_[0] += alpha * (mixed - anti_alias_filter_[0]);
    anti_alias_filter_[1] +=
        alpha * (anti_alias_filter_[0] - anti_alias_filter_[1]);
    anti_alias_filter_[2] +=
        alpha * (anti_alias_filter_[1] - anti_alias_filter_[2]);
    center_oscillator_ *= center_step;
    resample_accumulator_ += contract_.working_sample_rate_hz;
    if (resample_accumulator_ >= input_rate) {
      resample_accumulator_ -= input_rate;
      const auto timestamp = block.timestamp_ns + samplesToNanoseconds(
          static_cast<double>(index), input_rate);
      acceptWorkingSample(anti_alias_filter_[2], timestamp);
    }
    if (++center_oscillator_steps_ >= kOscillatorRenormalizeSteps) {
      center_oscillator_steps_ = 0;
      const float magnitude = std::abs(center_oscillator_);
      if (magnitude > 0.0F) center_oscillator_ /= magnitude;
    }
  }
  expected_input_timestamp_ns_ = block.timestamp_ns + samplesToNanoseconds(
      static_cast<double>(block.sample_count), input_rate);
  timing_initialized_ = true;
}

void CwCharacterLaneFrontend::acceptWorkingSample(
    const std::complex<float> sample, const std::uint64_t timestamp_ns) {
  lane_filter_history_[lane_filter_index_] = sample;
  std::complex<float> filtered{};
  std::size_t history = lane_filter_index_;
  for (const float tap : lane_filter_taps_) {
    filtered += tap * lane_filter_history_[history];
    history = history == 0U ? lane_filter_history_.size() - 1U : history - 1U;
  }
  lane_filter_index_ = (lane_filter_index_ + 1U) % lane_filter_history_.size();

  const double angle = 2.0 * std::numbers::pi *
                       contract_.isolated_tone_hz /
                       contract_.working_sample_rate_hz;
  const std::complex<float> feature_step{
      static_cast<float>(std::cos(angle)),
      static_cast<float>(std::sin(angle))};
  const float real_sample = 2.0F * (filtered * feature_oscillator_).real();
  feature_oscillator_ *= feature_step;
  // Stepped once per working sample and, until now, never renormalized on
  // any path at all -- it multiplies every feature this lane emits, so its
  // drift is a slow, invisible gain error in the model's input.
  if (++feature_oscillator_steps_ >= kOscillatorRenormalizeSteps) {
    feature_oscillator_steps_ = 0;
    const float magnitude = std::abs(feature_oscillator_);
    if (magnitude > 0.0F) feature_oscillator_ /= magnitude;
  }
  const bool was_full = stft_count_ == stft_history_.size();
  stft_history_[stft_index_] = real_sample;
  stft_index_ = (stft_index_ + 1U) % stft_history_.size();
  stft_count_ = std::min(stft_count_ + 1U, stft_history_.size());
  if (!was_full && stft_count_ == stft_history_.size()) {
    emitFeatureFrame(timestamp_ns);
    samples_since_frame_ = 0;
  } else if (was_full && ++samples_since_frame_ >= contract_.hop_length) {
    emitFeatureFrame(timestamp_ns);
    samples_since_frame_ = 0;
  }
}

void CwCharacterLaneFrontend::emitFeatureFrame(
    const std::uint64_t timestamp_ns) {
  const std::size_t row = feature_ring_index_ * frequency_bin_count_;
  for (std::size_t output_bin = 0; output_bin < frequency_bin_count_;
       ++output_bin) {
    const std::size_t bin = first_frequency_bin_ + output_bin;
    std::complex<double> value{};
    for (std::size_t index = 0; index < contract_.fft_length; ++index) {
      const std::size_t history = (stft_index_ + index) % stft_history_.size();
      const double angle = -2.0 * std::numbers::pi *
                           static_cast<double>(bin * index) /
                           static_cast<double>(contract_.fft_length);
      value += static_cast<double>(stft_history_[history] *
                                   stft_window_[index]) *
               std::complex<double>(std::cos(angle), std::sin(angle));
    }
    feature_ring_[row + output_bin] = static_cast<float>(
        std::log1p(2.0 * std::abs(value)));
  }
  const double delay_samples =
      static_cast<double>(contract_.lane_filter_taps - 1U) * 0.5 +
      static_cast<double>(contract_.fft_length - 1U) * 0.5;
  const auto delay_ns = samplesToNanoseconds(
      delay_samples, contract_.working_sample_rate_hz);
  timestamp_ring_[feature_ring_index_] = timestamp_ns > delay_ns
      ? timestamp_ns - delay_ns : 0U;
  feature_ring_index_ = (feature_ring_index_ + 1U) % window_frame_count_;
  feature_ring_count_ = std::min(feature_ring_count_ + 1U,
                                 window_frame_count_);
  ++total_feature_frames_;
  if (feature_ring_count_ == window_frame_count_ &&
      (total_feature_frames_ - window_frame_count_) %
              stride_frame_count_ == 0U) {
    stageWindow();
  }
}

void CwCharacterLaneFrontend::stageWindow() {
  if (pending_window_) ++dropped_windows_;
  for (std::size_t frame = 0; frame < window_frame_count_; ++frame) {
    const std::size_t source = (feature_ring_index_ + frame) %
                               window_frame_count_;
    std::copy_n(feature_ring_.begin() + static_cast<std::ptrdiff_t>(
                    source * frequency_bin_count_),
                frequency_bin_count_,
                pending_features_.begin() + static_cast<std::ptrdiff_t>(
                    frame * frequency_bin_count_));
    pending_timestamps_[frame] = timestamp_ring_[source];
  }
  pending_sequence_ = ++output_sequence_;
  pending_window_ = true;
}

bool CwCharacterLaneFrontend::takeWindow(
    CwCharacterFeatureWindow& destination) {
  if (!pending_window_) return false;
  destination.track = track_;
  destination.sequence = pending_sequence_;
  destination.frame_count = window_frame_count_;
  destination.frequency_bins = frequency_bin_count_;
  destination.features = pending_features_;
  destination.frame_timestamps_ns = pending_timestamps_;
  const auto half_window_ns = samplesToNanoseconds(
      static_cast<double>(contract_.fft_length) * 0.5,
      contract_.working_sample_rate_hz);
  destination.started_ns = pending_timestamps_.front() > half_window_ns
      ? pending_timestamps_.front() - half_window_ns : 0U;
  destination.ended_ns = pending_timestamps_.back() + half_window_ns;
  pending_window_ = false;
  return true;
}

bool CwCharacterLaneFrontend::valid() const noexcept { return valid_; }

std::size_t CwCharacterLaneFrontend::frequencyBinCount() const noexcept {
  return frequency_bin_count_;
}

std::uint64_t CwCharacterLaneFrontend::droppedWindows() const noexcept {
  return dropped_windows_;
}

std::size_t CwCharacterLaneFrontend::stateBytes() const noexcept {
  return sizeof(*this) + lane_filter_taps_.capacity() * sizeof(float) +
      lane_filter_history_.capacity() * sizeof(std::complex<float>) +
      stft_window_.capacity() * sizeof(float) +
      stft_history_.capacity() * sizeof(float) +
      feature_ring_.capacity() * sizeof(float) +
      timestamp_ring_.capacity() * sizeof(std::uint64_t) +
      pending_features_.capacity() * sizeof(float) +
      pending_timestamps_.capacity() * sizeof(std::uint64_t);
}

}  // namespace cwassistant::core
