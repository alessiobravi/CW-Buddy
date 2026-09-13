#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cwassistant/core/iq_receive.hpp"
#include "cwassistant/core/iq_replay_source.hpp"
#include "cwassistant/core/sample_block.hpp"
#include "cwassistant/core/spectrum_analyzer.hpp"

namespace cwassistant::test {

// The receive chain a wide complex capture has to pass through before the
// channel bank can read it, extracted from the live desktop worker so a replay
// scores the same signal path the application decodes with.
//
// WHY A REPLAY CANNOT SIMPLY FEED THE BLOCK IN. The audio replay path hands
// each block straight to SpectrumAnalyzer::process() and
// CwChannelBank::processSamples(), which is correct because a WAV block IS
// what the decoder consumes. A capture block is not: it is 62.5 kS/s or more
// of complex baseband covering tens of kilohertz, and every narrowband filter
// and timing constant in the bank is sized for the decode region, not for the
// hardware passband. The live path therefore runs two spectrum stages with a
// decimating tuner between them -- an overview transform over the whole
// acquired span, an IqSubbandDecimator that extracts the decode window at a
// far lower rate, and a second transform whose bins are what detection reads.
// Reproducing that here is what makes a replayed score comparable to what the
// operator saw.
//
// WHAT IS DELIBERATELY NOT REPRODUCED. The live worker also publishes spectrum
// frames to the display, demodulates region audio, drives the character lane
// front ends and rate-limits its model publications. None of that reaches a
// decode, so none of it is here; the overview transform is kept because it is
// the stage that consumes the wide block, and dropping it would quietly change
// how much of the capture the decoder branch ever sees.

// The slice of spectrum a capture is decoded in, in absolute RF. Both fields
// are absolute so that a retune inside the recording moves the samples without
// moving the window: a station stays where it is when the receiver moves.
struct IqDecoderWindow {
  double center_frequency_hz{0.0};
  double bandwidth_hz{0.0};
};

// The shipped application's default decode width (AppSettings'
// sdr_decoder_bandwidth_hz_). A replay that picks its own default would score
// a window no operator ever uses.
inline constexpr double kDefaultIqDecoderBandwidthHz = 24'000.0;

// The window to decode a capture in when the caller names none.
//
// The recording's own centre is the only choice that cannot be wrong about the
// file, and it is frequently wrong about the operator: a receiver watching a
// pileup 16 kHz down from where it happened to be tuned produces a capture
// whose centre holds nothing at all. It is still the right default, because
// the alternative -- guessing at the window from the sidecar's free-text
// description -- would be a guess that reads like a fact.
[[nodiscard]] inline IqDecoderWindow defaultIqDecoderWindow(
    const core::IqReplaySource& source) noexcept {
  return {.center_frequency_hz = source.capture_metadata().center_frequency_hz,
          .bandwidth_hz = kDefaultIqDecoderBandwidthHz};
}

// IqSubbandDecimator::process()'s own admission rule, restated where a caller
// can ask about it before spending a replay on a window that will never be
// accepted.
//
// This predicate exists because of how the rejection presents. The decimator
// answers RejectedDescriptor and produces no samples, while the overview
// transform keeps painting a perfectly normal spectrum -- so a decoder window
// placed outside the acquired passband looks exactly like a decoder that
// silently stopped decoding, and the one thing it is not is a decoder fault.
[[nodiscard]] inline bool iqDecoderWindowFitsPassband(
    const IqDecoderWindow& window,
    const core::StreamDescriptor& stream) noexcept {
  return std::abs(window.center_frequency_hz - stream.center_frequency_hz) +
             window.bandwidth_hz * 0.5 <
         stream.sample_rate_hz * 0.5;
}

class IqReplayChain {
 public:
  // One decoder-rate block together with the spectrum frames computed from it.
  // `block` is null when this capture block produced no decoder block: either
  // the decimator refused it, or too few decimated samples have accumulated to
  // be worth a transform yet.
  struct Frame {
    const core::RealtimeSampleBlock* block{nullptr};
    std::span<const core::SpectrumSnapshot> spectra;
    core::IqBlockStatus status{core::IqBlockStatus::Accepted};
  };

  // The bins of one decoder spectrum frame that lie inside the requested
  // window, with the frequency span they actually cover. The decimated stream
  // is wider than the window it was asked for -- the output rate is chosen
  // from a small set of supported rates, not from the bandwidth -- so handing
  // the whole frame to detection would discover signals outside the window the
  // operator asked to decode.
  struct DetectorView {
    double lower_frequency_hz{0.0};
    double upper_frequency_hz{0.0};
    std::span<const float> bins_dbfs;
  };

  [[nodiscard]] bool configure(const IqDecoderWindow& window) {
    // Matches applySdrDecoderWindow(): the decoder branch is allowed up to
    // 2.5x the window width, bounded by the rates the decimator accepts.
    const double output_rate_hz =
        std::clamp(window.bandwidth_hz * 2.5, 48'000.0, 192'000.0);
    if (!decimator_.configure(
            {.center_frequency_hz = window.center_frequency_hz,
             .bandwidth_hz = window.bandwidth_hz,
             .maximum_output_sample_rate_hz = output_rate_hz})) {
      return false;
    }
    window_ = window;
    pending_ = {};
    pending_carry_ = {};
    frame_produced_ = false;
    pending_sequence_ = 0;
    decoder_analyzer_.reset();
    overview_analyzer_.reset();
    spectra_.clear();
    return true;
  }

  [[nodiscard]] Frame process(const core::RealtimeSampleBlock& wide) {
    Frame frame;
    ++wide_blocks_;
    // Discarded, exactly as the harness discards the audio path's display
    // frames: this stage is here so the wide block is consumed the way the
    // live worker consumes it, not for its output.
    static_cast<void>(overview_analyzer_.process(wide));

    core::RealtimeSampleBlock decimated;
    frame.status = decimator_.process(wide, decimated);
    if (frame.status != core::IqBlockStatus::Accepted &&
        frame.status != core::IqBlockStatus::AcceptedAfterDiscontinuity) {
      ++rejected_blocks_;
      last_rejection_ = frame.status;
      return frame;
    }
    ++accepted_blocks_;
    if (frame.status == core::IqBlockStatus::AcceptedAfterDiscontinuity) {
      // The samples after a discontinuity do not continue the ones before it,
      // so a half-filled batch of the previous stream must not be completed
      // with samples from the new one.
      pending_ = {};
    }

    core::RealtimeSampleBlock carry;
    if (decimated.sample_count > 0) {
      if (pending_.sample_count > 0 &&
          (pending_.stream.sample_rate_hz != decimated.stream.sample_rate_hz ||
           pending_.stream.center_frequency_hz !=
               decimated.stream.center_frequency_hz)) {
        pending_ = {};
      }
      if (pending_.sample_count == 0) {
        pending_.stream = decimated.stream;
        pending_.timestamp_ns = decimated.timestamp_ns;
        pending_.sequence = pending_sequence_++;
      }
      const std::size_t available =
          pending_.samples.size() - pending_.sample_count;
      const std::size_t copied = std::min(available, decimated.sample_count);
      std::copy_n(decimated.samples.cbegin(), copied,
                  pending_.samples.begin() +
                      static_cast<std::ptrdiff_t>(pending_.sample_count));
      pending_.sample_count += copied;
      if (copied < decimated.sample_count) {
        carry.stream = decimated.stream;
        carry.timestamp_ns =
            decimated.timestamp_ns +
            static_cast<std::uint64_t>(static_cast<long double>(copied) *
                                       1'000'000'000.0L /
                                       decimated.stream.sample_rate_hz);
        carry.sequence = pending_sequence_++;
        carry.sample_count = decimated.sample_count - copied;
        std::copy_n(decimated.samples.cbegin() +
                        static_cast<std::ptrdiff_t>(copied),
                    carry.sample_count, carry.samples.begin());
      }
    }
    // The live worker's batching bound. A wide device yields only a few dozen
    // decimated samples per hardware block, and decoding those one at a time
    // costs a transform per handful of samples. Nothing is carried here: the
    // batch can only overflow once it is already full, which is far above this
    // bound, so an unfinished batch always absorbed the whole block.
    if (pending_.sample_count < kMinimumDecoderSamples) return frame;
    spectra_ = decoder_analyzer_.process(pending_);
    detector_frames_ += spectra_.size();
    ++decoder_blocks_;
    decoder_samples_ += pending_.sample_count;
    frame.block = &pending_;
    frame.spectra = spectra_;
    pending_carry_ = carry;
    frame_produced_ = true;
    return frame;
  }

  // Called once the caller has finished with the Frame returned by process().
  // Separate from process() because Frame::block points into `pending_`, and
  // replacing it with the carry while the caller still holds the pointer is
  // exactly the kind of mistake a replay would only notice as a wrong score.
  void advance() noexcept {
    if (!frame_produced_) return;
    pending_ = pending_carry_;
    pending_carry_ = {};
    frame_produced_ = false;
  }

  [[nodiscard]] DetectorView detectorView(
      const core::SpectrumSnapshot& snapshot) const noexcept {
    const double bin_width_hz = snapshot.bin_width_hz;
    const double requested_lower_hz =
        window_.center_frequency_hz - window_.bandwidth_hz * 0.5;
    const double requested_upper_hz =
        window_.center_frequency_hz + window_.bandwidth_hz * 0.5;
    const auto last_index =
        static_cast<double>(snapshot.instantaneous_bins_dbfs.size() - 1U);
    const auto first_bin = static_cast<std::size_t>(std::clamp(
        std::ceil((requested_lower_hz - snapshot.lower_frequency_hz) /
                  bin_width_hz),
        0.0, last_index));
    const auto last_bin = static_cast<std::size_t>(std::clamp(
        std::floor((requested_upper_hz - snapshot.lower_frequency_hz) /
                   bin_width_hz),
        static_cast<double>(first_bin), last_index));
    const double lower_hz = snapshot.lower_frequency_hz +
                            static_cast<double>(first_bin) * bin_width_hz;
    const auto bins = std::span<const float>(
        snapshot.instantaneous_bins_dbfs.data() + first_bin,
        last_bin - first_bin + 1U);
    return {.lower_frequency_hz = lower_hz,
            .upper_frequency_hz =
                lower_hz + static_cast<double>(bins.size()) * bin_width_hz,
            .bins_dbfs = bins};
  }

  [[nodiscard]] const IqDecoderWindow& window() const noexcept {
    return window_;
  }
  [[nodiscard]] double decoderSampleRateHz() const noexcept {
    return decimator_.outputSampleRateHz();
  }
  [[nodiscard]] std::uint64_t wideBlocks() const noexcept {
    return wide_blocks_;
  }
  [[nodiscard]] std::uint64_t acceptedBlocks() const noexcept {
    return accepted_blocks_;
  }
  [[nodiscard]] std::uint64_t rejectedBlocks() const noexcept {
    return rejected_blocks_;
  }
  [[nodiscard]] std::uint64_t decoderBlocks() const noexcept {
    return decoder_blocks_;
  }
  [[nodiscard]] std::uint64_t decoderSamples() const noexcept {
    return decoder_samples_;
  }
  [[nodiscard]] std::uint64_t detectorFrames() const noexcept {
    return detector_frames_;
  }
  [[nodiscard]] core::IqBlockStatus lastRejection() const noexcept {
    return last_rejection_;
  }

 private:
  static constexpr std::size_t kMinimumDecoderSamples = 512;

  // fft_size 16'384 for complex IQ and 8'192 for the decoder branch are the
  // live worker's own sizes. The overview size decides the resolution of the
  // stage the wide block is consumed by; the decoder size decides the bin
  // width detection actually sees.
  core::SpectrumAnalyzer overview_analyzer_{
      {.fft_size = 16'384, .averaging_frames = 3, .frame_rate_hz = 60}};
  core::SpectrumAnalyzer decoder_analyzer_{
      {.fft_size = 8'192, .averaging_frames = 3, .frame_rate_hz = 60}};
  core::IqSubbandDecimator decimator_;
  core::RealtimeSampleBlock pending_;
  core::RealtimeSampleBlock pending_carry_;
  std::vector<core::SpectrumSnapshot> spectra_;
  IqDecoderWindow window_{};
  std::uint64_t pending_sequence_{0};
  std::uint64_t wide_blocks_{0};
  std::uint64_t accepted_blocks_{0};
  std::uint64_t rejected_blocks_{0};
  std::uint64_t decoder_blocks_{0};
  std::uint64_t decoder_samples_{0};
  std::uint64_t detector_frames_{0};
  core::IqBlockStatus last_rejection_{core::IqBlockStatus::Accepted};
  bool frame_produced_{false};
};

}  // namespace cwassistant::test
