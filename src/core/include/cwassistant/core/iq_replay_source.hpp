#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "cwassistant/core/interfaces.hpp"
#include "cwassistant/core/iq_writer.hpp"

namespace cwassistant::core {

// Deterministic, non-realtime source for a SigMF recording, and the exact
// inverse of IqWriter: what that class wrote, this class hands back as
// RealtimeSampleBlocks shaped the way a live receiver delivers them.
//
// Without it a captured band is write-only. The application can record an
// off-air pileup but cannot feed one back through the decoder, so the only
// population available for regression scoring is the single-station WAV
// corpus -- which is precisely the population in which a multi-station rule
// cannot be measured at all.
//
// The accessor names deliberately match WavReplaySource rather than the
// camelCase used elsewhere in the IQ subsystem: the replay harness drives a
// source through open/start/read plus stream_descriptor/total_frames/
// position_frames/last_error, and a second spelling of that same surface
// would make the two sources gratuitously non-interchangeable. One "frame"
// here is one complex sample, as one frame in the WAV source is one audio
// sample.
class IqReplaySource final : public ISampleSource {
 public:
  // A sidecar this application wrote is a couple of kilobytes plus at most
  // IqWriter::kMaximumSegments retune records. The cap exists so a truncated,
  // corrupted, or hostile file cannot be read into memory unbounded before
  // the parser gets a chance to refuse it.
  static constexpr std::uint64_t kMaximumMetadataBytes = 8ULL * 1024ULL * 1024ULL;

  [[nodiscard]] std::vector<DeviceInfo> enumerate() const override;
  // `device_id` is the path to the `.sigmf-data` file (the `.sigmf-meta`
  // sidecar is accepted as well and resolved to its pair). `requested` must
  // name a complex-IQ stream; everything else about the stream comes from the
  // sidecar, never from the request.
  bool open(std::string_view device_id,
            const StreamDescriptor& requested) override;
  bool start() override;
  void stop() noexcept override;
  [[nodiscard]] bool read(RealtimeSampleBlock& destination,
                          std::chrono::milliseconds timeout) override;

  [[nodiscard]] const StreamDescriptor& stream_descriptor() const noexcept;
  // Everything the sidecar declared about the recording, in the same struct
  // the writer was handed. Lets a caller report the receiver, the gain state
  // and the capture time alongside a decode without reparsing the file.
  [[nodiscard]] const IqCaptureMetadata& capture_metadata() const noexcept;
  // Every capture segment in file order. More than one means the operator
  // retuned mid-recording; read() never spans two of them, so each block's
  // centre frequency is the one that was actually in effect.
  [[nodiscard]] const std::vector<IqCaptureSegment>& capture_segments()
      const noexcept;
  // The writer's own `cwbuddy:stop_reason` text, empty if the sidecar carries
  // none. A short recording is normal when this says the budget was reached
  // or the operator stopped it, and suspicious when it does not.
  [[nodiscard]] const std::string& recorded_stop_reason() const noexcept;
  // Sample count the sidecar claims, 0 when absent. Never used to bound
  // replay: a capture that died before close() still carries the sidecar
  // written at open(), which claims zero samples while the data file holds
  // the whole recording. The data file is the truth.
  [[nodiscard]] std::uint64_t declared_sample_count() const noexcept;
  // True when the data file's last sample is incomplete. The whole samples
  // before it are replayed and the partial one is dropped.
  [[nodiscard]] bool ends_mid_sample() const noexcept;
  [[nodiscard]] std::uint64_t total_frames() const noexcept;
  [[nodiscard]] std::uint64_t position_frames() const noexcept;
  [[nodiscard]] double duration_seconds() const noexcept;
  [[nodiscard]] const std::string& last_error() const noexcept;

  // Resolves a `.sigmf-meta` path back to its `.sigmf-data` pair, the inverse
  // of IqWriter::metadataPathFor. Exposed so a caller can accept either half
  // of a pair from an operator without duplicating the extension rule.
  [[nodiscard]] static std::string dataPathFor(std::string_view path);

 private:
  // The descriptor of a source that has no recording open. A default-built
  // StreamDescriptor claims 48 kHz audio instead, so a caller reading one back
  // after a refused open would be handed a plausible-looking stream that
  // describes nothing.
  static constexpr StreamDescriptor kNoStream{.kind = StreamKind::ComplexIq,
                                              .sample_rate_hz = 0.0,
                                              .center_frequency_hz = 0.0,
                                              .channel_count = 1};

  void close() noexcept;
  [[nodiscard]] bool loadMetadata(const std::string& metadata_path);

  std::ifstream file_;
  StreamDescriptor stream_{kNoStream};
  IqCaptureMetadata metadata_{};
  std::vector<IqCaptureSegment> segments_;
  std::string recorded_stop_reason_;
  std::string data_path_;
  std::string metadata_path_;
  std::string last_error_;
  std::vector<char> byte_buffer_;
  std::uint64_t data_bytes_{0};
  std::uint64_t declared_sample_count_{0};
  std::uint64_t total_samples_{0};
  std::uint64_t position_samples_{0};
  std::uint64_t sequence_{0};
  // Index of the first segment not yet in effect, so the read path costs one
  // comparison per block rather than a search.
  std::size_t next_segment_{0};
  std::size_t sample_bytes_{0};
  bool ends_mid_sample_{false};
  bool running_{false};
};

}  // namespace cwassistant::core
