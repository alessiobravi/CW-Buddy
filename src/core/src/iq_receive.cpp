#include "cwassistant/core/iq_receive.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace cwassistant::core {
namespace {

template <typename Value>
void saturatingAdd(Value& destination, const Value increment = 1) noexcept {
  const auto maximum = std::numeric_limits<Value>::max();
  destination = increment > maximum - destination ? maximum
                                                   : destination + increment;
}

bool finite(const double value) noexcept { return std::isfinite(value); }

bool validLimits(const IqReceiveLimits& limits) noexcept {
  return finite(limits.minimum_sample_rate_hz) &&
         finite(limits.maximum_sample_rate_hz) &&
         finite(limits.maximum_center_frequency_hz) &&
         std::isfinite(limits.maximum_sample_magnitude) &&
         limits.minimum_sample_rate_hz > 0.0 &&
         limits.maximum_sample_rate_hz >= limits.minimum_sample_rate_hz &&
         limits.maximum_center_frequency_hz > 0.0 &&
         limits.maximum_sample_magnitude > 0.0F;
}

bool sameStream(const StreamDescriptor& left,
                const StreamDescriptor& right) noexcept {
  return left.kind == right.kind &&
         left.sample_rate_hz == right.sample_rate_hz &&
         left.center_frequency_hz == right.center_frequency_hz &&
         left.channel_count == right.channel_count;
}

void normalize(std::complex<double>& oscillator) noexcept {
  const double magnitude = std::abs(oscillator);
  if (magnitude > 0.0 && finite(magnitude)) {
    oscillator /= magnitude;
  } else {
    oscillator = {1.0, 0.0};
  }
}

// sin(pi x) / (pi x), continuous at zero. The ideal low-pass impulse response
// this resampler's kernel is windowed from.
double normalizedSinc(const double x) noexcept {
  if (std::abs(x) < 1.0e-12) return 1.0;
  const double scaled = std::numbers::pi * x;
  return std::sin(scaled) / scaled;
}

// Zeroth-order modified Bessel function of the first kind, evaluated by its
// own series. Written out rather than taken from <cmath>, whose std::cyl_bessel_i
// is not available on every toolchain this core has to stay warning-clean and
// buildable on.
double besselI0(const double x) noexcept {
  double sum = 1.0;
  double term = 1.0;
  for (int index = 1; index < 64; ++index) {
    term *= (x * 0.5) / static_cast<double>(index);
    const double contribution = term * term;
    sum += contribution;
    if (contribution < 1.0e-17 * sum) break;
  }
  return sum;
}

}  // namespace

IqReceiveValidator::IqReceiveValidator(const IqReceiveLimits limits) {
  if (validLimits(limits)) {
    limits_ = limits;
  }
}

bool IqReceiveValidator::configure(const IqReceiveLimits limits) noexcept {
  if (!validLimits(limits)) {
    return false;
  }
  limits_ = limits;
  reset();
  return true;
}

void IqReceiveValidator::reset() noexcept {
  telemetry_ = {};
  previous_stream_ = {};
  expected_sequence_ = 0;
  expected_timestamp_ns_ = 0;
  have_previous_block_ = false;
}

IqBlockStatus IqReceiveValidator::validate(
    const RealtimeSampleBlock& block) noexcept {
  const auto reject = [this](const IqBlockStatus status) {
    saturatingAdd(telemetry_.rejected_blocks);
    return status;
  };
  if (block.stream.kind != StreamKind::ComplexIq) {
    return reject(IqBlockStatus::RejectedStreamKind);
  }
  if (!finite(block.stream.sample_rate_hz) ||
      !finite(block.stream.center_frequency_hz) ||
      block.stream.sample_rate_hz < limits_.minimum_sample_rate_hz ||
      block.stream.sample_rate_hz > limits_.maximum_sample_rate_hz ||
      block.stream.center_frequency_hz < 0.0 ||
      block.stream.center_frequency_hz > limits_.maximum_center_frequency_hz ||
      block.stream.channel_count != 1) {
    return reject(IqBlockStatus::RejectedDescriptor);
  }
  if (block.sample_count == 0 || block.sample_count > block.samples.size()) {
    return reject(IqBlockStatus::RejectedSampleCount);
  }
  for (std::size_t index = 0; index < block.sample_count; ++index) {
    const auto& sample = block.samples[index];
    if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) {
      saturatingAdd(telemetry_.non_finite_samples);
      return reject(IqBlockStatus::RejectedNonFiniteSample);
    }
    if (std::abs(sample) > limits_.maximum_sample_magnitude) {
      saturatingAdd(telemetry_.excessive_magnitude_samples);
      return reject(IqBlockStatus::RejectedSampleMagnitude);
    }
  }

  bool discontinuity = false;
  if (have_previous_block_) {
    if (!sameStream(previous_stream_, block.stream) ||
        block.sequence != expected_sequence_) {
      discontinuity = true;
    }
    if (block.sequence > expected_sequence_) {
      saturatingAdd(telemetry_.missing_block_sequences,
                    block.sequence - expected_sequence_);
    }
    const auto timing_tolerance_ns = static_cast<std::uint64_t>(std::ceil(
        2.0 * 1'000'000'000.0 / block.stream.sample_rate_hz));
    const auto timing_difference_ns =
        block.timestamp_ns > expected_timestamp_ns_
            ? block.timestamp_ns - expected_timestamp_ns_
            : expected_timestamp_ns_ - block.timestamp_ns;
    discontinuity = discontinuity || timing_difference_ns > timing_tolerance_ns;
  }

  previous_stream_ = block.stream;
  expected_sequence_ = block.sequence + 1;
  expected_timestamp_ns_ =
      block.timestamp_ns + static_cast<std::uint64_t>(
                               static_cast<long double>(block.sample_count) *
                               1'000'000'000.0L /
                               block.stream.sample_rate_hz);
  have_previous_block_ = true;
  saturatingAdd(telemetry_.accepted_blocks);
  saturatingAdd(telemetry_.accepted_samples,
                static_cast<std::uint64_t>(block.sample_count));
  if (discontinuity) {
    saturatingAdd(telemetry_.discontinuities);
    return IqBlockStatus::AcceptedAfterDiscontinuity;
  }
  return IqBlockStatus::Accepted;
}

const IqReceiveLimits& IqReceiveValidator::limits() const noexcept {
  return limits_;
}

const IqReceiveTelemetry& IqReceiveValidator::telemetry() const noexcept {
  return telemetry_;
}

IqToAudioChannelizer::IqToAudioChannelizer(
    const IqChannelizerConfig config, const IqReceiveLimits limits)
    : validator_(limits) {
  static_cast<void>(configure(config));
}

bool IqToAudioChannelizer::configure(
    const IqChannelizerConfig config) noexcept {
  if (!finite(config.target_frequency_hz) ||
      !finite(config.output_sample_rate_hz) ||
      !finite(config.output_tone_hz) ||
      !finite(config.channel_bandwidth_hz) ||
      config.target_frequency_hz < 0.0 ||
      config.target_frequency_hz > 99'000'000'000.0 ||
      config.output_sample_rate_hz < 8'000.0 ||
      config.output_sample_rate_hz > 192'000.0 ||
      config.output_tone_hz < 100.0 ||
      config.channel_bandwidth_hz < 40.0 ||
      config.channel_bandwidth_hz > 2'000.0 ||
      config.output_tone_hz + config.channel_bandwidth_hz / 2.0 >=
          config.output_sample_rate_hz / 2.0) {
    return false;
  }
  config_ = config;
  resetSignalState();
  stream_initialized_ = false;
  return true;
}

void IqToAudioChannelizer::resetSignalState() noexcept {
  channel_filter_ = {};
  tuning_oscillator_ = {1.0, 0.0};
  tone_oscillator_ = {1.0, 0.0};
  decimation_sum_ = {};
  decimation_phase_ = 0.0;
  decimation_count_ = 0;
  saturatingAdd(state_resets_);
}

void IqToAudioChannelizer::reset() noexcept {
  validator_.reset();
  input_stream_ = {};
  output_sequence_ = 0;
  state_resets_ = 0;
  output_blocks_ = 0;
  output_samples_ = 0;
  stream_initialized_ = false;
  resetSignalState();
  state_resets_ = 0;
}

IqBlockStatus IqToAudioChannelizer::process(
    const RealtimeSampleBlock& input,
    RealtimeSampleBlock& output) noexcept {
  output = {};
  const IqBlockStatus status = validator_.validate(input);
  if (status != IqBlockStatus::Accepted &&
      status != IqBlockStatus::AcceptedAfterDiscontinuity) {
    return status;
  }

  const double offset_hz =
      config_.target_frequency_hz - input.stream.center_frequency_hz;
  if (config_.output_sample_rate_hz > input.stream.sample_rate_hz ||
      std::abs(offset_hz) + config_.channel_bandwidth_hz / 2.0 >=
          input.stream.sample_rate_hz / 2.0) {
    return IqBlockStatus::RejectedDescriptor;
  }

  const bool changed_stream =
      !stream_initialized_ || !sameStream(input_stream_, input.stream);
  if (changed_stream || status == IqBlockStatus::AcceptedAfterDiscontinuity) {
    resetSignalState();
    input_stream_ = input.stream;
    stream_initialized_ = true;
    const double tuning_angle =
        -2.0 * std::numbers::pi * offset_hz / input.stream.sample_rate_hz;
    tuning_step_ = {std::cos(tuning_angle), std::sin(tuning_angle)};
    const double tone_angle = 2.0 * std::numbers::pi * config_.output_tone_hz /
                              config_.output_sample_rate_hz;
    tone_step_ = {std::cos(tone_angle), std::sin(tone_angle)};
    filter_alpha_ = 1.0 - std::exp(-2.0 * std::numbers::pi *
                                   (config_.channel_bandwidth_hz / 2.0) /
                                   input.stream.sample_rate_hz);
  }

  output.stream = {.kind = StreamKind::Audio,
                   .sample_rate_hz = config_.output_sample_rate_hz,
                   .center_frequency_hz = 0.0,
                   .channel_count = 1};
  output.sequence = output_sequence_;
  output.timestamp_ns = input.timestamp_ns;

  for (std::size_t index = 0; index < input.sample_count; ++index) {
    const std::complex<double> mixed =
        static_cast<std::complex<double>>(input.samples[index]) *
        tuning_oscillator_;
    tuning_oscillator_ *= tuning_step_;
    std::complex<double> stage_input = mixed;
    for (auto& stage : channel_filter_) {
      stage += filter_alpha_ * (stage_input - stage);
      stage_input = stage;
    }
    decimation_sum_ += channel_filter_.back();
    ++decimation_count_;
    decimation_phase_ += config_.output_sample_rate_hz;
    if (decimation_phase_ + 1.0e-9 >= input.stream.sample_rate_hz) {
      decimation_phase_ -= input.stream.sample_rate_hz;
      const auto baseband =
          decimation_sum_ / static_cast<double>(decimation_count_);
      const double audio = (baseband * tone_oscillator_).real();
      output.samples[output.sample_count++] = {
          static_cast<float>(std::clamp(audio, -1.0, 1.0)), 0.0F};
      tone_oscillator_ *= tone_step_;
      decimation_sum_ = {};
      decimation_count_ = 0;
    }
  }
  normalize(tuning_oscillator_);
  normalize(tone_oscillator_);

  if (output.sample_count > 0) {
    output.sequence = output_sequence_++;
    saturatingAdd(output_blocks_);
    saturatingAdd(output_samples_,
                  static_cast<std::uint64_t>(output.sample_count));
  }
  return status;
}

const IqChannelizerConfig& IqToAudioChannelizer::config() const noexcept {
  return config_;
}

IqChannelizerTelemetry IqToAudioChannelizer::telemetry() const noexcept {
  return {.input = validator_.telemetry(),
          .state_resets = state_resets_,
          .output_blocks = output_blocks_,
          .output_samples = output_samples_};
}

IqSubbandDecimator::IqSubbandDecimator(
    const IqSubbandDecimatorConfig config, const IqReceiveLimits limits)
    : validator_(limits) {
  static_cast<void>(configure(config));
}

bool IqSubbandDecimator::configure(
    const IqSubbandDecimatorConfig config) noexcept {
  if (!finite(config.center_frequency_hz) ||
      !finite(config.bandwidth_hz) ||
      !finite(config.maximum_output_sample_rate_hz) ||
      config.center_frequency_hz < 0.0 ||
      config.center_frequency_hz > 99'000'000'000.0 ||
      config.bandwidth_hz < 2'000.0 || config.bandwidth_hz > 192'000.0 ||
      config.maximum_output_sample_rate_hz < 8'000.0 ||
      config.maximum_output_sample_rate_hz > 192'000.0 ||
      config.bandwidth_hz >= config.maximum_output_sample_rate_hz * 0.8) {
    return false;
  }
  config_ = config;
  reset();
  return true;
}

void IqSubbandDecimator::resetSignalState() noexcept {
  tuning_oscillator_ = {1.0, 0.0};
  output_phase_ = 0.0;
  oscillator_samples_ = 0;
  for (auto& stage : low_pass_) {
    stage.z1 = {};
    stage.z2 = {};
  }
}

void IqSubbandDecimator::reset() noexcept {
  validator_.reset();
  input_stream_ = {};
  output_sample_rate_hz_ = 0.0;
  output_sequence_ = 0;
  stream_initialized_ = false;
  resetSignalState();
}

void IqSubbandDecimator::initializeForStream(
    const StreamDescriptor& stream) noexcept {
  input_stream_ = stream;
  output_sample_rate_hz_ =
      std::min(stream.sample_rate_hz, config_.maximum_output_sample_rate_hz);
  const double offset_hz =
      config_.center_frequency_hz - stream.center_frequency_hz;
  const double angle =
      -2.0 * std::numbers::pi * offset_hz / stream.sample_rate_hz;
  tuning_step_ = {std::cos(angle), std::sin(angle)};

  // A fourth-order Butterworth response gives a flat CW decoding window and
  // meaningful rejection before the lower-rate Nyquist boundary. The two Q
  // values are the conjugate pole pairs of a normalized fourth-order filter.
  const double cutoff_hz = std::min(
      output_sample_rate_hz_ * 0.44, config_.bandwidth_hz * 0.45);
  constexpr std::array<double, 2> q_values{0.541196100146197,
                                           1.306562964876377};
  const double omega =
      2.0 * std::numbers::pi * cutoff_hz / stream.sample_rate_hz;
  const double cosine = std::cos(omega);
  const double sine = std::sin(omega);
  for (std::size_t index = 0; index < low_pass_.size(); ++index) {
    const double alpha = sine / (2.0 * q_values[index]);
    const double a0 = 1.0 + alpha;
    auto& stage = low_pass_[index];
    stage.b0 = ((1.0 - cosine) * 0.5) / a0;
    stage.b1 = (1.0 - cosine) / a0;
    stage.b2 = stage.b0;
    stage.a1 = (-2.0 * cosine) / a0;
    stage.a2 = (1.0 - alpha) / a0;
  }
  resetSignalState();
  stream_initialized_ = true;
}

IqBlockStatus IqSubbandDecimator::process(
    const RealtimeSampleBlock& input,
    RealtimeSampleBlock& output) noexcept {
  output = {};
  const IqBlockStatus status = validator_.validate(input);
  if (status != IqBlockStatus::Accepted &&
      status != IqBlockStatus::AcceptedAfterDiscontinuity) {
    return status;
  }
  const double offset_hz =
      config_.center_frequency_hz - input.stream.center_frequency_hz;
  if (std::abs(offset_hz) + config_.bandwidth_hz * 0.5 >=
      input.stream.sample_rate_hz * 0.5) {
    return IqBlockStatus::RejectedDescriptor;
  }

  const bool changed_stream = !stream_initialized_ ||
      !sameStream(input_stream_, input.stream);
  if (changed_stream) {
    initializeForStream(input.stream);
  } else if (status == IqBlockStatus::AcceptedAfterDiscontinuity) {
    resetSignalState();
  }

  output.stream = {.kind = StreamKind::ComplexIq,
                   .sample_rate_hz = output_sample_rate_hz_,
                   .center_frequency_hz = config_.center_frequency_hz,
                   .channel_count = 1};
  bool have_timestamp = false;
  for (std::size_t index = 0; index < input.sample_count; ++index) {
    std::complex<double> filtered =
        static_cast<std::complex<double>>(input.samples[index]) *
        tuning_oscillator_;
    tuning_oscillator_ *= tuning_step_;
    ++oscillator_samples_;
    if ((oscillator_samples_ & 1'023U) == 0U) normalize(tuning_oscillator_);

    for (auto& stage : low_pass_) {
      const std::complex<double> next = stage.b0 * filtered + stage.z1;
      stage.z1 = stage.b1 * filtered - stage.a1 * next + stage.z2;
      stage.z2 = stage.b2 * filtered - stage.a2 * next;
      filtered = next;
    }

    output_phase_ += output_sample_rate_hz_;
    if (output_phase_ + 1.0e-9 < input.stream.sample_rate_hz) continue;
    output_phase_ -= input.stream.sample_rate_hz;
    if (output.sample_count >= output.samples.size()) break;
    if (!have_timestamp) {
      output.timestamp_ns = input.timestamp_ns + static_cast<std::uint64_t>(
          static_cast<long double>(index) * 1'000'000'000.0L /
          input.stream.sample_rate_hz);
      have_timestamp = true;
    }
    output.samples[output.sample_count++] = {
        static_cast<float>(filtered.real()),
        static_cast<float>(filtered.imag())};
  }
  if (output.sample_count > 0) output.sequence = output_sequence_++;
  return status;
}

const IqSubbandDecimatorConfig& IqSubbandDecimator::config() const noexcept {
  return config_;
}

double IqSubbandDecimator::outputSampleRateHz() const noexcept {
  return output_sample_rate_hz_;
}


double IqRegionDemodulator::outputSampleRateForBandwidthHz(
    const double bandwidth_hz) noexcept {
  if (!finite(bandwidth_hz) || bandwidth_hz <= 0.0) return 0.0;
  // Real audio sampled at Fs carries 0..Fs/2, so the region needs Fs >= 2W.
  // Smallest acceptable candidate wins, because every extra sample is paid for
  // by the interpolator, the wire and the operator's sound card alike.
  constexpr std::array<double, 3> candidates{48'000.0, 96'000.0, 192'000.0};
  for (const double rate : candidates) {
    if (rate >= 2.0 * bandwidth_hz) return rate;
  }
  return 0.0;
}

bool IqRegionDemodulator::configure(
    const IqRegionDemodulatorConfig config) noexcept {
  if (!finite(config.bandwidth_hz) || !finite(config.input_sample_rate_hz)) {
    return false;
  }
  if (config.bandwidth_hz < 100.0 || config.bandwidth_hz > 96'000.0) {
    return false;
  }
  // Strictly greater, not merely different: the region has to fit inside the
  // analytic stream that carries it, and a region equal to its own sample rate
  // reaches the Nyquist boundary at both edges at once.
  if (!(config.input_sample_rate_hz > config.bandwidth_hz)) return false;
  // This is the decoder branch, never the hardware stream. A megasample rate
  // arriving here means a caller wired the wrong tap in, and demodulating it
  // would burn a core producing audio nobody asked for.
  if (config.input_sample_rate_hz > 1'000'000.0) return false;
  const double output_rate = outputSampleRateForBandwidthHz(config.bandwidth_hz);
  if (output_rate <= 0.0) return false;

  config_ = config;
  output_sample_rate_hz_ = output_rate;
  input_samples_per_output_ = config.input_sample_rate_hz / output_rate;
  // The single-sideband shift, stepped at the OUTPUT rate. It cannot be done
  // at the input rate: the region fills that stream's whole Nyquist span, so a
  // shift of W/2 there would wrap the top of the region straight onto the
  // bottom -- the failure this class exists to avoid, introduced by the fix
  // for it.
  const double angle =
      2.0 * std::numbers::pi * (config.bandwidth_hz * 0.5) / output_rate;
  shift_step_ = {std::cos(angle), std::sin(angle)};
  buildKernel();
  reset();
  configured_ = true;
  return true;
}

void IqRegionDemodulator::buildKernel() noexcept {
  // Cutoff at the region edge, expressed against the input rate. The kernel is
  // the region filter and the anti-imaging filter at once, so its corner is the
  // one frequency that means something to an operator: the edge of the window
  // they chose.
  const double cutoff = std::min(
      config_.bandwidth_hz * 0.5 / config_.input_sample_rate_hz, 0.49);
  constexpr double kBeta = 7.0;
  const double beta_normalizer = besselI0(kBeta);
  kernel_.assign(kSubPhases * kTaps, 0.0F);
  for (std::size_t phase = 0; phase < kSubPhases; ++phase) {
    const double fraction =
        static_cast<double>(phase) / static_cast<double>(kSubPhases);
    double sum = 0.0;
    for (std::size_t tap = 0; tap < kTaps; ++tap) {
      const double offset = static_cast<double>(tap) -
                            static_cast<double>(kHalfTaps) + 1.0 - fraction;
      const double window_position =
          offset / static_cast<double>(kHalfTaps);
      double window = 0.0;
      if (std::abs(window_position) <= 1.0) {
        window = besselI0(kBeta * std::sqrt(std::max(
                              0.0, 1.0 - window_position * window_position))) /
                 beta_normalizer;
      }
      const double value =
          2.0 * cutoff * normalizedSinc(2.0 * cutoff * offset) * window;
      kernel_[phase * kTaps + tap] = static_cast<float>(value);
      sum += value;
    }
    // Each fractional position is normalized to unit gain on its own. An
    // unnormalized polyphase bank has a slightly different gain at every
    // fractional position, and because those positions are visited in a
    // repeating pattern the difference becomes amplitude modulation at the
    // resampling beat rate -- heard as a steady flutter over every signal in
    // the region at once, which is far more obvious than the fraction of a
    // decibel it measures.
    const double scale = std::abs(sum) > 1.0e-12 ? 1.0 / sum : 1.0;
    for (std::size_t tap = 0; tap < kTaps; ++tap) {
      kernel_[phase * kTaps + tap] =
          static_cast<float>(static_cast<double>(kernel_[phase * kTaps + tap]) *
                             scale);
    }
  }
}

void IqRegionDemodulator::reset() noexcept {
  history_.fill({});
  shift_oscillator_ = {1.0, 0.0};
  next_output_time_ = 0.0;
  written_samples_ = 0;
  oscillator_samples_ = 0;
}

bool IqRegionDemodulator::configured() const noexcept { return configured_; }

const IqRegionDemodulatorConfig& IqRegionDemodulator::config() const noexcept {
  return config_;
}

double IqRegionDemodulator::outputSampleRateHz() const noexcept {
  return output_sample_rate_hz_;
}

std::size_t IqRegionDemodulator::maximumOutputSamples(
    const std::size_t input_samples) const noexcept {
  if (!configured_ || input_samples_per_output_ <= 0.0) return 0;
  return static_cast<std::size_t>(
             std::ceil(static_cast<double>(input_samples) /
                       input_samples_per_output_)) +
         1U;
}

void IqRegionDemodulator::process(const std::complex<float>* samples,
                                  const std::size_t sample_count,
                                  std::vector<float>& audio) {
  if (!configured_ || samples == nullptr || sample_count == 0) return;
  for (std::size_t index = 0; index < sample_count; ++index) {
    history_[static_cast<std::size_t>(written_samples_ & kHistoryMask)] =
        static_cast<std::complex<double>>(samples[index]);
    ++written_samples_;
    // An output at time t reads input samples floor(t) - kHalfTaps + 1 through
    // floor(t) + kHalfTaps, so it becomes producible the moment the last of
    // them has been written. Everything the loop needs is therefore already in
    // the ring, and the interpolator never has to wait for a whole block.
    while (true) {
      const auto floor_index =
          static_cast<std::uint64_t>(std::floor(next_output_time_));
      if (floor_index + kHalfTaps + 1U > written_samples_) break;
      const double fraction =
          next_output_time_ - static_cast<double>(floor_index);
      const auto phase = std::min<std::size_t>(
          kSubPhases - 1U,
          static_cast<std::size_t>(fraction *
                                       static_cast<double>(kSubPhases) +
                                   0.5));
      const float* coefficients = kernel_.data() + phase * kTaps;
      // Unsigned arithmetic on purpose. Before the stream has produced
      // kHalfTaps samples this subtraction wraps, and the mask then selects
      // ring slots that have never been written -- which hold zero, and zero is
      // exactly what "the samples before the stream started" should be. No
      // special case for the first block, and none needed after it, because by
      // then the index is genuinely positive.
      const std::uint64_t first = floor_index + 1U - kHalfTaps;
      std::complex<double> accumulator{};
      for (std::size_t tap = 0; tap < kTaps; ++tap) {
        accumulator +=
            history_[static_cast<std::size_t>((first + tap) & kHistoryMask)] *
            static_cast<double>(coefficients[tap]);
      }
      // The single-sideband step, and the only place the analytic property is
      // spent. real() of a shifted analytic sample is a real signal whose
      // spectrum is conjugate-symmetric by construction, but there was nothing
      // at the reflected frequency to add to it, so the region arrives in
      // [0, W] intact and un-mirrored.
      const std::complex<double> shifted = accumulator * shift_oscillator_;
      shift_oscillator_ *= shift_step_;
      ++oscillator_samples_;
      if ((oscillator_samples_ & 1'023U) == 0U) normalize(shift_oscillator_);
      audio.push_back(static_cast<float>(shifted.real()));
      next_output_time_ += input_samples_per_output_;
    }
  }
  // Keeps the fractional output clock at full precision however long a session
  // runs. The interval is a multiple of the ring size, so every slot still
  // holds the sample its index says it does.
  if (written_samples_ >= kRebaseInterval) {
    written_samples_ -= kRebaseInterval;
    next_output_time_ -= static_cast<double>(kRebaseInterval);
  }
}

}  // namespace cwassistant::core
