#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cwassistant/core/sample_block.hpp"

namespace cwassistant::core {

enum class IqBlockStatus {
  Accepted,
  AcceptedAfterDiscontinuity,
  RejectedStreamKind,
  RejectedDescriptor,
  RejectedSampleCount,
  RejectedNonFiniteSample,
  RejectedSampleMagnitude,
};

struct IqReceiveLimits {
  double minimum_sample_rate_hz{8'000.0};
  double maximum_sample_rate_hz{64'000'000.0};
  double maximum_center_frequency_hz{99'000'000'000.0};
  float maximum_sample_magnitude{8.0F};
};

struct IqReceiveTelemetry {
  std::uint64_t accepted_blocks{0};
  std::uint64_t accepted_samples{0};
  std::uint64_t rejected_blocks{0};
  std::uint64_t discontinuities{0};
  std::uint64_t missing_block_sequences{0};
  std::uint64_t non_finite_samples{0};
  std::uint64_t excessive_magnitude_samples{0};
};

// Validates provider-produced IQ at the dependency-free boundary. The class
// never allocates, throws, sleeps, or calls hardware and keeps all counters
// saturating so malformed or long-running streams cannot wrap diagnostics.
class IqReceiveValidator {
 public:
  explicit IqReceiveValidator(IqReceiveLimits limits = {});

  [[nodiscard]] bool configure(IqReceiveLimits limits) noexcept;
  void reset() noexcept;
  [[nodiscard]] IqBlockStatus validate(
      const RealtimeSampleBlock& block) noexcept;
  [[nodiscard]] const IqReceiveLimits& limits() const noexcept;
  [[nodiscard]] const IqReceiveTelemetry& telemetry() const noexcept;

 private:
  IqReceiveLimits limits_{};
  IqReceiveTelemetry telemetry_{};
  StreamDescriptor previous_stream_{};
  std::uint64_t expected_sequence_{0};
  std::uint64_t expected_timestamp_ns_{0};
  bool have_previous_block_{false};
};

struct IqChannelizerConfig {
  double target_frequency_hz{0.0};
  double output_sample_rate_hz{48'000.0};
  double output_tone_hz{700.0};
  double channel_bandwidth_hz{500.0};
};

struct IqChannelizerTelemetry {
  IqReceiveTelemetry input{};
  std::uint64_t state_resets{0};
  std::uint64_t output_blocks{0};
  std::uint64_t output_samples{0};
};

// Converts a selected RF channel from complex IQ into a conventional real
// audio CW tone. The unmodified IQ block remains suitable for the shared
// SpectrumAnalyzer, while this bounded bridge feeds the existing audio
// decoder. It is receive-only and exposes no radio-control or TX operation.
class IqToAudioChannelizer {
 public:
  explicit IqToAudioChannelizer(IqChannelizerConfig config = {},
                                IqReceiveLimits limits = {});

  [[nodiscard]] bool configure(IqChannelizerConfig config) noexcept;
  void reset() noexcept;
  [[nodiscard]] IqBlockStatus process(const RealtimeSampleBlock& input,
                                      RealtimeSampleBlock& output) noexcept;
  [[nodiscard]] const IqChannelizerConfig& config() const noexcept;
  [[nodiscard]] IqChannelizerTelemetry telemetry() const noexcept;

 private:
  void resetSignalState() noexcept;

  IqChannelizerConfig config_{};
  IqReceiveValidator validator_{};
  StreamDescriptor input_stream_{};
  std::array<std::complex<double>, 3> channel_filter_{};
  std::complex<double> tuning_oscillator_{1.0, 0.0};
  std::complex<double> tuning_step_{1.0, 0.0};
  std::complex<double> tone_oscillator_{1.0, 0.0};
  std::complex<double> tone_step_{1.0, 0.0};
  std::complex<double> decimation_sum_{};
  double decimation_phase_{0.0};
  double filter_alpha_{0.0};
  std::size_t decimation_count_{0};
  std::uint64_t output_sequence_{0};
  std::uint64_t state_resets_{0};
  std::uint64_t output_blocks_{0};
  std::uint64_t output_samples_{0};
  bool stream_initialized_{false};
};

struct IqSubbandDecimatorConfig {
  // Absolute RF at the centre of the decoder window.
  double center_frequency_hz{0.0};
  // The portion of the wide IQ passband admitted to detection/decoding.
  double bandwidth_hz{24'000.0};
  // Upper bound for the decoder branch. The input is never upsampled.
  double maximum_output_sample_rate_hz{96'000.0};
};

// Creates one bounded, lower-rate complex-IQ branch while leaving the original
// block untouched for the overview spectrum. A shared DDC is substantially
// cheaper than running every CW track across the hardware sample rate.
class IqSubbandDecimator {
 public:
  explicit IqSubbandDecimator(IqSubbandDecimatorConfig config = {},
                              IqReceiveLimits limits = {});

  [[nodiscard]] bool configure(IqSubbandDecimatorConfig config) noexcept;
  void reset() noexcept;
  [[nodiscard]] IqBlockStatus process(const RealtimeSampleBlock& input,
                                      RealtimeSampleBlock& output) noexcept;
  [[nodiscard]] const IqSubbandDecimatorConfig& config() const noexcept;
  [[nodiscard]] double outputSampleRateHz() const noexcept;

 private:
  struct Biquad {
    double b0{1.0};
    double b1{0.0};
    double b2{0.0};
    double a1{0.0};
    double a2{0.0};
    std::complex<double> z1{};
    std::complex<double> z2{};
  };

  void initializeForStream(const StreamDescriptor& stream) noexcept;
  void resetSignalState() noexcept;

  IqSubbandDecimatorConfig config_{};
  IqReceiveValidator validator_{};
  StreamDescriptor input_stream_{};
  std::array<Biquad, 2> low_pass_{};
  std::complex<double> tuning_oscillator_{1.0, 0.0};
  std::complex<double> tuning_step_{1.0, 0.0};
  double output_sample_rate_hz_{0.0};
  double output_phase_{0.0};
  std::uint64_t output_sequence_{0};
  std::uint64_t oscillator_samples_{0};
  bool stream_initialized_{false};
};


struct IqRegionDemodulatorConfig {
  // The width of the decode region, in Hz. The complex input is taken to carry
  // exactly this region, centred on its own DC.
  double bandwidth_hz{24'000.0};
  // The rate of that complex input. Must exceed `bandwidth_hz`: an analytic
  // stream sampled at R carries R Hz, so a region as wide as its own sample
  // rate has no room left to be a region.
  double input_sample_rate_hz{0.0};
};

// Turns the whole decode region into real audio somebody can listen to.
//
// WHY THIS IS NOT THE PER-TRACK MONITOR. The channel bank's monitor narrows to
// one tracked signal and re-pitches it to the operator's sidetone, which is the
// right answer when the question is "what is that one station sending". This
// answers a different question -- "what is in my decode region" -- so every
// signal keeps its true offset from the region centre and therefore its own
// pitch. Where a station sits in the region is what its pitch tells you, and
// two stations 400 Hz apart are heard 400 Hz apart.
//
// THE RATE INVARIANT, WHICH IS THE WHOLE REASON THIS CLASS EXISTS. An analytic
// (complex) stream sampled at R represents R Hz of bandwidth, from -R/2 to
// +R/2. A REAL stream sampled at Fs represents only Fs/2, from 0 to Fs/2. So a
// region W wide cannot be carried by W Hz of real audio however carefully it is
// filtered: it needs Fs >= 2W. That is not a quality preference, it is what the
// sampling theorem permits, and getting it wrong does not sound slightly worse
// -- the top half of the region folds onto the bottom half and two stations at
// opposite ends of the window arrive at the same pitch. The decode region is
// capped at 24 kHz precisely so that 48 kHz audio always satisfies it.
//
// THE DEMODULATION. Single sideband, and deliberately so: shift the baseband up
// by W/2 so the region occupies [0, W] instead of [-W/2, +W/2], then take the
// real part. Because the source is ANALYTIC there is no image to fold back --
// nothing exists at the reflected frequency to interfere with what lands there
// -- which is exactly why the region is demodulated from complex baseband
// rather than from a real passband. A component at region offset +d is heard at
// W/2 + d and at nothing else; one at -d is heard at W/2 - d. Reverse the shift
// and every signal in the region swaps sides of the window while still sounding
// entirely plausible, which is the failure this class's tests exist to catch.
//
// The interpolation is a polyphase windowed-sinc bank rather than anything
// cheaper, and the reason is specific to the shift above rather than to audio
// quality in general. Rotating the band by W/2 wraps whatever sits outside the
// region around the output Nyquist boundary, so out-of-region content just
// BELOW the region does not stay out of the way: at Fs = 48 kHz a signal
// 1 kHz below the region's lower edge lands at 1 kHz of audio, right in the
// middle of where an operator is listening. A resampler whose stopband is only
// a dozen dB down -- linear interpolation is about 14 dB at the rates in use
// here -- therefore does not merely soften the audio, it imports neighbours
// into the middle of the region. The kernel is the region filter as well as
// the anti-imaging filter, which is why there is one filter here and not two.
class IqRegionDemodulator {
 public:
  // The output sample rate for a region `bandwidth_hz` wide, or 0.0 when no
  // supported rate can carry it.
  //
  // THE RULE, EXACTLY: the smallest rate in {48 000, 96 000, 192 000} that is
  // at least twice the region bandwidth; nothing wider than 96 kHz can be
  // demodulated to audio at all. The candidates are the rates a sound card and
  // a remote listener's player both accept without argument, so the ordinary
  // case -- the 24 kHz the decode region is capped at -- lands on 48 kHz and
  // asks nothing unusual of either.
  [[nodiscard]] static double outputSampleRateForBandwidthHz(
      double bandwidth_hz) noexcept;

  [[nodiscard]] bool configure(IqRegionDemodulatorConfig config) noexcept;
  // Clears the oscillator phase and the interpolator's delay line, keeping the
  // configuration. Call it whenever the samples stop being continuous with the
  // ones before them: the region moved, the receiver restarted, a block was
  // dropped, or nobody wanted audio for a while and now somebody does.
  //
  // What a stale oscillator sounds like, since that is the point of doing this
  // deliberately: the shift oscillator is a free-running complex phasor, so
  // carrying it across a discontinuity steps the phase of every signal in the
  // region at once -- heard as a click on each retune, and after a bandwidth
  // change as every station sitting at the wrong pitch, because the shift
  // frequency is W/2 and W has changed. The delay line is worse than the
  // oscillator: left alone it plays out half a millisecond of the PREVIOUS
  // region's samples through the new region's filter, a short chirp at each
  // move that is easy to mistake for a signal.
  void reset() noexcept;

  [[nodiscard]] bool configured() const noexcept;
  [[nodiscard]] const IqRegionDemodulatorConfig& config() const noexcept;
  [[nodiscard]] double outputSampleRateHz() const noexcept;

  // Appends the real audio produced by `sample_count` complex baseband samples
  // to `audio`. Appends nothing at all until configured, so a caller that never
  // configures never pays: this is the one producer for three consumers and it
  // must be possible to leave it entirely idle when none of them is listening.
  void process(const std::complex<float>* samples, std::size_t sample_count,
               std::vector<float>& audio);

  // An upper bound on how many output samples `input_samples` can produce, so a
  // caller can reserve once instead of growing a buffer inside the loop.
  [[nodiscard]] std::size_t maximumOutputSamples(
      std::size_t input_samples) const noexcept;

 private:
  void buildKernel() noexcept;

  // 64 taps is chosen against the wrap described above rather than against a
  // listening preference. With a Kaiser window at beta = 7 it puts the
  // stopband roughly 70 dB down within about a sixth of the region's width of
  // the region edge, so a neighbour strong enough to be heard at all has to be
  // very close to the window before it wraps in audibly, while costing 64
  // multiply-accumulates per output sample -- about three megaflops per second
  // of audio, which is nothing beside the decoder the same samples feed.
  static constexpr std::size_t kTaps = 64;
  static constexpr std::size_t kHalfTaps = kTaps / 2;
  // Fractional positions the kernel is pre-evaluated at. 256 leaves the worst
  // timing error at 1/512 of an input sample, whose phase error at the top of
  // a 24 kHz region is about 2.5 milliradians -- spurious products near 58 dB
  // down, and spread rather than concentrated in one tone.
  static constexpr std::size_t kSubPhases = 256;
  // A power of two so the modular index arithmetic below needs a mask rather
  // than a division, and twice the tap count so a burst of input samples can
  // never overwrite a sample an output still needs.
  static constexpr std::size_t kHistorySamples = 128;
  static constexpr std::uint64_t kHistoryMask = kHistorySamples - 1U;
  // Absolute sample indices are rebased at this interval so the fractional
  // output clock keeps full precision in a session that runs for days. A
  // multiple of kHistorySamples, so rebasing does not move the ring.
  static constexpr std::uint64_t kRebaseInterval = 1ULL << 30U;

  IqRegionDemodulatorConfig config_{};
  std::vector<float> kernel_{};
  std::array<std::complex<double>, kHistorySamples> history_{};
  std::complex<double> shift_oscillator_{1.0, 0.0};
  std::complex<double> shift_step_{1.0, 0.0};
  double output_sample_rate_hz_{0.0};
  // Input samples per output sample. Greater than one when the region stream
  // runs faster than the audio it becomes, which is the usual case.
  double input_samples_per_output_{0.0};
  // The input-sample time of the next output, on the same absolute timeline as
  // `written_samples_`.
  double next_output_time_{0.0};
  std::uint64_t written_samples_{0};
  std::uint64_t oscillator_samples_{0};
  bool configured_{false};
};

}  // namespace cwassistant::core
