#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cwassistant/core/cw_character_decoder.hpp"
#include "cwassistant/core/sample_block.hpp"

namespace cwassistant::core {

struct CwCharacterFeatureContract {
  double working_sample_rate_hz{3'200.0};
  std::size_t fft_length{256};
  std::size_t hop_length{48};
  double minimum_frequency_hz{400.0};
  double maximum_frequency_hz{1'200.0};
  double isolated_tone_hz{800.0};
  double lane_bandwidth_hz{40.0};
  std::size_t lane_filter_taps{641};
  double window_seconds{8.0};
  double stride_seconds{1.0};
};

struct CwCharacterFeatureWindow {
  CwCharacterTrackKey track{};
  std::uint64_t sequence{0};
  std::uint64_t started_ns{0};
  std::uint64_t ended_ns{0};
  std::size_t frame_count{0};
  std::size_t frequency_bins{0};
  // Time-major [frame_count, frequency_bins] log-magnitude features.
  std::vector<float> features;
  std::vector<std::uint64_t> frame_timestamps_ns;
};

// One bounded, phase-continuous narrow lane. A coordinator owns only a small
// configured number of these objects; this class performs no inference and
// has no effect on the classical channel bank.
class CwCharacterLaneFrontend {
 public:
  explicit CwCharacterLaneFrontend(
      CwCharacterFeatureContract contract = {});

  [[nodiscard]] bool configure(CwCharacterFeatureContract contract);
  void reset(CwCharacterTrackKey track, double center_frequency_hz) noexcept;
  void setCenterFrequency(double center_frequency_hz) noexcept;
  void process(const RealtimeSampleBlock& block);
  [[nodiscard]] bool takeWindow(CwCharacterFeatureWindow& destination);
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::size_t frequencyBinCount() const noexcept;
  [[nodiscard]] std::uint64_t droppedWindows() const noexcept;
  [[nodiscard]] std::size_t stateBytes() const noexcept;

 private:
  void resetSignalState() noexcept;
  void rebuild();
  void acceptWorkingSample(std::complex<float> sample,
                           std::uint64_t timestamp_ns);
  void emitFeatureFrame(std::uint64_t timestamp_ns);
  void stageWindow();

  CwCharacterFeatureContract contract_{};
  CwCharacterTrackKey track_{};
  StreamDescriptor stream_{};
  double center_frequency_hz_{0.0};
  std::complex<float> center_oscillator_{1.0F, 0.0F};
  std::complex<float> feature_oscillator_{1.0F, 0.0F};
  // Steps taken since each oscillator above was last restored to unit
  // magnitude. They count the stream rather than the current block, because
  // the oscillators themselves survive block boundaries.
  std::size_t center_oscillator_steps_{0};
  std::size_t feature_oscillator_steps_{0};
  std::array<std::complex<float>, 3> anti_alias_filter_{};
  double resample_accumulator_{0.0};
  std::vector<float> lane_filter_taps_;
  std::vector<std::complex<float>> lane_filter_history_;
  std::size_t lane_filter_index_{0};
  std::vector<float> stft_window_;
  std::vector<float> stft_history_;
  std::size_t stft_index_{0};
  std::size_t stft_count_{0};
  std::size_t samples_since_frame_{0};
  std::vector<float> feature_ring_;
  std::vector<std::uint64_t> timestamp_ring_;
  std::size_t feature_ring_index_{0};
  std::size_t feature_ring_count_{0};
  std::uint64_t total_feature_frames_{0};
  std::vector<float> pending_features_;
  std::vector<std::uint64_t> pending_timestamps_;
  std::uint64_t pending_sequence_{0};
  std::uint64_t output_sequence_{0};
  std::uint64_t expected_input_timestamp_ns_{0};
  std::uint64_t dropped_windows_{0};
  std::size_t first_frequency_bin_{0};
  std::size_t frequency_bin_count_{0};
  std::size_t window_frame_count_{0};
  std::size_t stride_frame_count_{0};
  bool valid_{false};
  bool stream_initialized_{false};
  bool timing_initialized_{false};
  bool pending_window_{false};
};

}  // namespace cwassistant::core
