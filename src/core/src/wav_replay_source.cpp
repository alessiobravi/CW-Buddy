#include "cwassistant/core/wav_replay_source.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace cwassistant::core {
namespace {

std::uint16_t read_u16(const std::byte* bytes) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<unsigned int>(bytes[0]) |
                                    (std::to_integer<unsigned int>(bytes[1])
                                     << 8U));
}

std::uint32_t read_u32(const std::byte* bytes) noexcept {
  return std::to_integer<std::uint32_t>(bytes[0]) |
         (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

bool chunk_is(const std::byte* id, const char* expected) noexcept {
  return std::memcmp(id, expected, 4) == 0;
}

}  // namespace

std::vector<DeviceInfo> WavReplaySource::enumerate() const { return {}; }

bool WavReplaySource::open(const std::string_view device_id,
                           const StreamDescriptor& requested) {
  close();
  if (requested.kind != StreamKind::Audio || device_id.empty()) {
    last_error_ = "WAV replay requires an audio file path";
    return false;
  }

  file_.open(std::string(device_id), std::ios::binary);
  if (!file_) {
    last_error_ = "WAV file could not be opened";
    return false;
  }

  std::byte header[12]{};
  if (!file_.read(reinterpret_cast<char*>(header), sizeof(header)) ||
      !chunk_is(header, "RIFF") || !chunk_is(header + 8, "WAVE")) {
    last_error_ = "File is not a little-endian RIFF/WAVE stream";
    close();
    return false;
  }

  bool found_format = false;
  bool found_data = false;
  std::uint16_t format_code = 0;
  std::uint32_t sample_rate = 0;
  while (file_ && (!found_format || !found_data)) {
    std::byte chunk_header[8]{};
    if (!file_.read(reinterpret_cast<char*>(chunk_header),
                    sizeof(chunk_header))) {
      break;
    }
    const std::uint32_t chunk_size = read_u32(chunk_header + 4);
    const auto chunk_data = static_cast<std::uint64_t>(file_.tellg());
    if (chunk_is(chunk_header, "fmt ")) {
      if (chunk_size < 16) {
        last_error_ = "WAV format chunk is truncated";
        close();
        return false;
      }
      std::byte format[16]{};
      if (!file_.read(reinterpret_cast<char*>(format), sizeof(format))) {
        last_error_ = "WAV format chunk could not be read";
        close();
        return false;
      }
      format_code = read_u16(format);
      input_channels_ = read_u16(format + 2);
      sample_rate = read_u32(format + 4);
      block_align_ = read_u16(format + 12);
      bits_per_sample_ = read_u16(format + 14);
      found_format = true;
    } else if (chunk_is(chunk_header, "data")) {
      data_offset_ = chunk_data;
      data_bytes_ = chunk_size;
      found_data = true;
    }

    const std::uint64_t next = chunk_data + chunk_size + (chunk_size & 1U);
    if (next > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max())) {
      last_error_ = "WAV chunk offset is too large";
      close();
      return false;
    }
    file_.seekg(static_cast<std::streamoff>(next), std::ios::beg);
  }

  if (!found_format || !found_data || input_channels_ == 0 ||
      sample_rate == 0 || block_align_ == 0) {
    last_error_ = "WAV file is missing a usable format or data chunk";
    close();
    return false;
  }

  const auto bytes_per_sample = static_cast<std::uint16_t>(bits_per_sample_ / 8U);
  if (format_code == 1 && bits_per_sample_ == 8) {
    encoding_ = Encoding::PcmUnsigned8;
  } else if (format_code == 1 &&
             (bits_per_sample_ == 16 || bits_per_sample_ == 24 ||
              bits_per_sample_ == 32)) {
    encoding_ = Encoding::PcmSigned;
  } else if (format_code == 3 && bits_per_sample_ == 32) {
    encoding_ = Encoding::Float32;
  } else {
    last_error_ = "Only PCM 8/16/24/32-bit and IEEE float32 WAV are supported";
    close();
    return false;
  }
  if (bytes_per_sample == 0 ||
      block_align_ != input_channels_ * bytes_per_sample) {
    last_error_ = "WAV block alignment is inconsistent with its format";
    close();
    return false;
  }

  total_frames_ = data_bytes_ / block_align_;
  stream_ = {
      .kind = StreamKind::Audio,
      .sample_rate_hz = static_cast<double>(sample_rate),
      .center_frequency_hz = 0.0,
      .channel_count = 1,
  };
  byte_buffer_.resize(RealtimeSampleBlock{}.samples.size() * block_align_);
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
  last_error_.clear();
  return static_cast<bool>(file_) && total_frames_ > 0;
}

bool WavReplaySource::start() {
  if (!file_.is_open() || total_frames_ == 0) {
    last_error_ = "No WAV file is open";
    return false;
  }
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
  position_frames_ = 0;
  sequence_ = 0;
  running_ = static_cast<bool>(file_);
  return running_;
}

void WavReplaySource::stop() noexcept { running_ = false; }

bool WavReplaySource::read(RealtimeSampleBlock& destination,
                           const std::chrono::milliseconds timeout) {
  static_cast<void>(timeout);
  if (!running_ || position_frames_ >= total_frames_) {
    return false;
  }

  const auto capacity = destination.samples.size();
  const auto remaining = total_frames_ - position_frames_;
  const auto frames = static_cast<std::size_t>(
      std::min<std::uint64_t>(remaining, capacity));
  const auto byte_count = frames * block_align_;
  file_.read(reinterpret_cast<char*>(byte_buffer_.data()),
             static_cast<std::streamsize>(byte_count));
  const auto bytes_read = static_cast<std::size_t>(file_.gcount());
  const auto complete_frames = bytes_read / block_align_;
  if (complete_frames == 0) {
    running_ = false;
    return false;
  }

  destination.stream = stream_;
  destination.sequence = sequence_++;
  destination.timestamp_ns = static_cast<std::uint64_t>(
      static_cast<long double>(position_frames_) * 1'000'000'000.0L /
      stream_.sample_rate_hz);
  destination.sample_count = complete_frames;
  const auto bytes_per_sample = bits_per_sample_ / 8U;
  for (std::size_t frame = 0; frame < complete_frames; ++frame) {
    float sum = 0.0F;
    const auto* frame_bytes = byte_buffer_.data() + frame * block_align_;
    for (std::uint16_t channel = 0; channel < input_channels_; ++channel) {
      sum += decode_sample(frame_bytes + channel * bytes_per_sample);
    }
    destination.samples[frame] = {
        sum / static_cast<float>(input_channels_), 0.0F};
  }
  position_frames_ += complete_frames;
  if (position_frames_ >= total_frames_) {
    running_ = false;
  }
  return true;
}

const StreamDescriptor& WavReplaySource::stream_descriptor() const noexcept {
  return stream_;
}

std::uint64_t WavReplaySource::total_frames() const noexcept {
  return total_frames_;
}

std::uint64_t WavReplaySource::position_frames() const noexcept {
  return position_frames_;
}

double WavReplaySource::duration_seconds() const noexcept {
  return stream_.sample_rate_hz > 0.0
             ? static_cast<double>(total_frames_) / stream_.sample_rate_hz
             : 0.0;
}

const std::string& WavReplaySource::last_error() const noexcept {
  return last_error_;
}

void WavReplaySource::close() noexcept {
  if (file_.is_open()) {
    file_.close();
  }
  running_ = false;
  stream_ = {};
  data_offset_ = 0;
  data_bytes_ = 0;
  total_frames_ = 0;
  position_frames_ = 0;
  sequence_ = 0;
  input_channels_ = 0;
  bits_per_sample_ = 0;
  block_align_ = 0;
  byte_buffer_.clear();
}

float WavReplaySource::decode_sample(const std::byte* bytes) const noexcept {
  if (encoding_ == Encoding::PcmUnsigned8) {
    // Unsigned 8-bit WAV is offset binary with silence at 128, so its rails
    // are 0 and 255 and no divisor makes them equidistant from silence. 128
    // is the only one that keeps silence at exactly zero and the positive
    // rail inside unity; 127 would push the negative rail 0.8% past full
    // scale to buy a symmetry the format does not have. Nothing in this
    // application writes 8-bit, so no round trip depends on the choice.
    return (static_cast<float>(std::to_integer<unsigned int>(bytes[0])) -
            128.0F) /
           128.0F;
  }
  if (encoding_ == Encoding::Float32) {
    const float value = std::bit_cast<float>(read_u32(bytes));
    return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
  }

  // Signed PCM divides by 2^(bits-1), which is deliberately NOT the exact
  // inverse of WavWriter, and the asymmetry is a decision rather than an
  // oversight.
  //
  // WavWriter multiplies by 32767 because it has to: it clamps to [-1, 1]
  // first, and 1.0 scaled by 32768 overflows int16. That also keeps its two
  // rails equidistant from zero, the same choice IqWriter's to_i16()
  // documents. This reader cannot follow it, because its main job is other
  // people's recordings rather than this application's own captures, and
  // 2^(bits-1) is the decode convention those files are written against: it
  // is what maps the most negative code to exactly -1.0, and being a power
  // of two it is the only scale that converts every code to a float with no
  // rounding at all. IqWriter and IqReplaySource get to be exact inverses
  // because that pair owns both ends of its format; this pair does not.
  //
  // So a capture written here and replayed here comes back one part in
  // 32768 small -- 0.00027 dB, bounded, and the same for every sample.
  // Nothing in this project compares replayed samples against recorded ones
  // (cw_capture_replay scores decoded text and callsigns), so that costs
  // nothing, while matching the convention is worth something on every
  // foreign file. Changing either constant to close the gap would shift the
  // decoder's input on all 22 stored captures in exchange for a
  // bit-exactness no caller asks for.
  if (bits_per_sample_ == 16) {
    const auto value = static_cast<std::int16_t>(read_u16(bytes));
    return static_cast<float>(value) / 32'768.0F;
  }
  if (bits_per_sample_ == 24) {
    std::int32_t value = static_cast<std::int32_t>(
        std::to_integer<std::uint32_t>(bytes[0]) |
        (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
        (std::to_integer<std::uint32_t>(bytes[2]) << 16U));
    if ((value & 0x0080'0000) != 0) {
      value |= static_cast<std::int32_t>(0xFF00'0000U);
    }
    return static_cast<float>(value) / 8'388'608.0F;
  }
  const auto value = static_cast<std::int32_t>(read_u32(bytes));
  return static_cast<float>(static_cast<double>(value) / 2'147'483'648.0);
}

}  // namespace cwassistant::core
