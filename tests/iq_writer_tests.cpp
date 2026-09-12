#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "cwassistant/core/iq_replay_source.hpp"
#include "cwassistant/core/iq_writer.hpp"
#include "cwassistant/core/sample_block.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::filesystem::path scratch_path(const std::string_view name) {
  return std::filesystem::temp_directory_path() / std::string(name);
}

void remove_recording(const std::filesystem::path& data_path) {
  std::error_code error;
  std::filesystem::remove(data_path, error);
  std::filesystem::remove(
      cwassistant::core::IqWriter::metadataPathFor(data_path.string()), error);
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
}

std::int16_t decode_i16_le(const std::vector<unsigned char>& bytes,
                           const std::size_t offset) {
  const auto raw = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(bytes[offset]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1])
                                 << 8U));
  return static_cast<std::int16_t>(raw);
}

float decode_f32_le(const std::vector<unsigned char>& bytes,
                    const std::size_t offset) {
  const std::uint32_t raw =
      static_cast<std::uint32_t>(bytes[offset]) |
      (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
      (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
      (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
  float value = 0.0F;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

bool contains(const std::string& haystack, const std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

// Every field is filled explicitly: a partial designated initializer trips
// -Wmissing-field-initializers in the strict GCC build.
cwassistant::core::IqCaptureMetadata metadataFor(
    const cwassistant::core::IqSampleFormat format, const double sample_rate_hz,
    const double center_frequency_hz) {
  cwassistant::core::IqCaptureMetadata metadata;
  metadata.format = format;
  metadata.sample_rate_hz = sample_rate_hz;
  metadata.center_frequency_hz = center_frequency_hz;
  metadata.datetime_utc = "2026-01-02T03:04:05.000Z";
  return metadata;
}

cwassistant::core::RealtimeSampleBlock iqBlock(
    const double sample_rate_hz, const double center_frequency_hz,
    const std::size_t sample_count) {
  cwassistant::core::RealtimeSampleBlock block;
  block.stream = {.kind = cwassistant::core::StreamKind::ComplexIq,
                  .sample_rate_hz = sample_rate_hz,
                  .center_frequency_hz = center_frequency_hz,
                  .channel_count = 1};
  block.sample_count = sample_count;
  return block;
}

// The whole reason this writer exists: the audio WavWriter records only the
// real component, which destroys the sideband distinction of a complex
// receiver. Assert that a known complex sequence comes back with its
// imaginary parts intact and in the right interleaved order.
void testComplexRoundTrip() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_writer_roundtrip.sigmf-data");
  remove_recording(path);

  auto metadata =
      metadataFor(IqSampleFormat::Ci16Le, 250'000.0, 14'050'000.0);
  metadata.hardware = "Test receiver";
  metadata.description = "Round-trip fixture";
  metadata.automatic_gain_known = true;
  metadata.automatic_gain = true;

  IqWriter writer;
  expect(writer.open(path.string(), metadata), "a SigMF recording opens");

  auto block = iqBlock(250'000.0, 14'050'000.0, 5);
  const std::complex<float> expected[5] = {
      {0.0F, 0.5F}, {0.25F, -0.25F}, {-0.5F, 0.0F},
      {0.125F, 0.75F}, {-0.75F, -0.125F}};
  for (std::size_t index = 0; index < 5; ++index) {
    block.samples[index] = expected[index];
  }
  expect(writer.writeBlock(block), "a complex block is written");
  expect(writer.samplesWritten() == 5, "every sample of the block is recorded");
  expect(writer.bytesWritten() == 5 * 4,
         "ci16 costs four bytes per complex sample");
  writer.close();

  const auto bytes = read_bytes(path);
  expect(bytes.size() == 20, "the data file holds exactly the interleaved pairs");
  bool interleaved_match = bytes.size() == 20;
  bool imaginary_survived = false;
  for (std::size_t index = 0; index < 5 && bytes.size() == 20; ++index) {
    const auto real = decode_i16_le(bytes, index * 4);
    const auto imaginary = decode_i16_le(bytes, index * 4 + 2);
    const auto want_real =
        static_cast<std::int16_t>(std::lround(expected[index].real() * 32'767.0F));
    const auto want_imaginary =
        static_cast<std::int16_t>(std::lround(expected[index].imag() * 32'767.0F));
    if (real != want_real || imaginary != want_imaginary) interleaved_match = false;
    if (imaginary != 0) imaginary_survived = true;
  }
  expect(interleaved_match,
         "both components round-trip in interleaved I,Q order");
  expect(imaginary_survived,
         "the imaginary component is preserved, unlike the audio WAV path");

  remove_recording(path);
}

void testComplexFloatRoundTripIsBitExact() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_writer_cf32.sigmf-data");
  remove_recording(path);

  IqWriter writer;
  expect(writer.open(path.string(),
                     metadataFor(IqSampleFormat::Cf32Le, 48'000.0, 7'030'000.0)),
         "a cf32 recording opens");
  auto block = iqBlock(48'000.0, 7'030'000.0, 3);
  const std::complex<float> expected[3] = {
      {0.123456789F, -0.987654321F}, {1.0F, -1.0F}, {-3.5e-7F, 2.25e-3F}};
  for (std::size_t index = 0; index < 3; ++index) {
    block.samples[index] = expected[index];
  }
  expect(writer.writeBlock(block), "a cf32 block is written");
  expect(writer.bytesWritten() == 3 * 8,
         "cf32 costs eight bytes per complex sample");
  writer.close();

  const auto bytes = read_bytes(path);
  bool exact = bytes.size() == 24;
  for (std::size_t index = 0; index < 3 && bytes.size() == 24; ++index) {
    if (decode_f32_le(bytes, index * 8) != expected[index].real() ||
        decode_f32_le(bytes, index * 8 + 4) != expected[index].imag()) {
      exact = false;
    }
  }
  expect(exact, "cf32 preserves the provider's samples bit for bit");
  expect(contains(read_text(IqWriter::metadataPathFor(path.string())),
                  "\"core:datatype\": \"cf32_le\""),
         "the sidecar names the cf32 datatype");

  remove_recording(path);
}

void testSidecarDescribesTheRecording() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_writer_sidecar.sigmf-data");
  remove_recording(path);

  auto metadata =
      metadataFor(IqSampleFormat::Ci16Le, 8'000'000.0, 14'050'000.0);
  metadata.hardware = "SDRplay RSPduo \"Single Tuner\"";
  metadata.description = "Line one\nline two";
  metadata.datetime_utc = "2026-03-04T05:06:07.008Z";
  metadata.automatic_gain_known = true;
  metadata.automatic_gain = true;
  metadata.gain_db = 42.5;

  IqWriter writer;
  expect(writer.open(path.string(), metadata), "a high-rate recording opens");
  const auto metadata_path = IqWriter::metadataPathFor(path.string());
  expect(writer.metadataPath() == metadata_path,
         "the sidecar path replaces the .sigmf-data extension");
  expect(std::filesystem::exists(metadata_path),
         "the sidecar exists from the moment the capture starts, so an "
         "interrupted recording still describes itself");

  auto block = iqBlock(8'000'000.0, 14'050'000.0, 8);
  for (std::size_t index = 0; index < 8; ++index) {
    block.samples[index] = {0.5F, -0.25F};
  }
  expect(writer.writeBlock(block), "the block is written");

  // A mid-capture retune must become a new SigMF capture segment rather than
  // silently mislabelling the samples that follow it.
  auto retuned = iqBlock(8'000'000.0, 14'060'000.0, 4);
  for (std::size_t index = 0; index < 4; ++index) {
    retuned.samples[index] = {0.1F, 0.1F};
  }
  expect(writer.writeBlock(retuned), "a retuned block is written");
  writer.close();

  const std::string sidecar = read_text(metadata_path);
  expect(contains(sidecar, "\"core:datatype\": \"ci16_le\""),
         "the sidecar names the default ci16_le datatype");
  expect(contains(sidecar, "\"core:sample_rate\": 8000000"),
         "the sidecar records the true sample rate");
  expect(contains(sidecar, "\"core:version\": \"1.0.0\""),
         "the sidecar declares a SigMF version");
  expect(contains(sidecar, "\"core:frequency\": 14050000"),
         "the first capture segment records the centre frequency");
  expect(contains(sidecar, "\"core:frequency\": 14060000"),
         "a retune opens a second capture segment");
  expect(contains(sidecar, "\"core:sample_start\": 8"),
         "the second segment starts at the sample where the retune landed");
  expect(contains(sidecar, "\"core:datetime\": \"2026-03-04T05:06:07.008Z\""),
         "the supplied capture time is recorded verbatim");
  expect(contains(sidecar, "SDRplay RSPduo \\\"Single Tuner\\\""),
         "quotes inside operator-visible text are escaped");
  expect(contains(sidecar, "Line one\\nline two"),
         "newlines inside operator-visible text are escaped");
  expect(contains(sidecar, "\"cwbuddy:automatic_gain\": true") &&
             contains(sidecar, "\"cwbuddy:gain_db\": 42.5"),
         "the gain state travels with the recording");
  expect(contains(sidecar, "\"cwbuddy:sample_count\": 12"),
         "the final sample count is written on close");
  expect(contains(sidecar, "\"cwbuddy:peak_magnitude\""),
         "level telemetry is written alongside the samples");

  remove_recording(path);
}

// Bounds are expressed in bytes and seconds, not in a frame count shaped for
// an audio rate: at 8 MS/s the audio path's 30-minute frame cap is ~21 s.
void testDurationBudgetStopsAndReports() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_writer_duration.sigmf-data");
  remove_recording(path);

  IqWriter writer;
  expect(writer.open(path.string(),
                     metadataFor(IqSampleFormat::Ci16Le, 1'000.0, 0.0),
                     {.maximum_data_bytes = 1'000'000'000ULL,
                      .maximum_seconds = 0.01}),
         "a duration-bounded recording opens");
  auto block = iqBlock(1'000.0, 0.0, 40);
  expect(!writer.writeBlock(block),
         "the writer stops once the duration budget is spent");
  expect(writer.samplesWritten() == 10,
         "exactly the budgeted 10 ms of samples are recorded");
  expect(writer.stopReason() == IqCaptureStopReason::DurationBudget,
         "the duration budget is reported as the reason");
  expect(!writer.writeBlock(block), "no further block is accepted");
  expect(writer.samplesWritten() == 10, "a stopped capture writes nothing more");
  writer.close();
  expect(read_bytes(path).size() == 40,
         "the data file holds only the budgeted samples");
  expect(contains(read_text(IqWriter::metadataPathFor(path.string())),
                  "\"cwbuddy:stop_reason\": \"Reached the capture duration "
                  "budget\""),
         "the sidecar explains why the recording ended");

  remove_recording(path);
}

void testByteBudgetStopsAndReports() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_writer_bytes.sigmf-data");
  remove_recording(path);

  IqWriter writer;
  expect(writer.open(path.string(),
                     metadataFor(IqSampleFormat::Ci16Le, 1'000'000.0, 0.0),
                     {.maximum_data_bytes = 24, .maximum_seconds = 3'600.0}),
         "a byte-bounded recording opens");
  auto block = iqBlock(1'000'000.0, 0.0, 32);
  expect(!writer.writeBlock(block),
         "the writer stops once the byte budget is spent");
  expect(writer.bytesWritten() == 24, "the byte budget is respected exactly");
  expect(writer.stopReason() == IqCaptureStopReason::ByteBudget,
         "the byte budget is reported as the reason");
  writer.close();
  expect(read_bytes(path).size() == 24,
         "the data file never exceeds the byte budget");

  remove_recording(path);
}

// A provider that overshoots unity must saturate, never wrap: a wrapped
// sample flips sign at full scale and reads as a violent discontinuity to any
// later analysis.
void testCi16ScalingClampsRatherThanWraps() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_writer_clamp.sigmf-data");
  remove_recording(path);

  IqWriter writer;
  expect(writer.open(path.string(),
                     metadataFor(IqSampleFormat::Ci16Le, 48'000.0, 0.0)),
         "a clamping fixture opens");
  auto block = iqBlock(48'000.0, 0.0, 4);
  block.samples[0] = {1.0F, -1.0F};
  block.samples[1] = {2.5F, -2.5F};
  block.samples[2] = {1'000.0F, -1'000.0F};
  block.samples[3] = {0.999985F, -0.999985F};
  expect(writer.writeBlock(block), "an over-range block is written");
  writer.close();

  const auto bytes = read_bytes(path);
  expect(bytes.size() == 16, "four complex samples are recorded");
  bool clamped = bytes.size() == 16;
  for (std::size_t index = 0; index < 4 && bytes.size() == 16; ++index) {
    const auto real = decode_i16_le(bytes, index * 4);
    const auto imaginary = decode_i16_le(bytes, index * 4 + 2);
    if (real != 32'767 || imaginary != -32'767) clamped = false;
  }
  expect(clamped,
         "every over-range component saturates at +/-32767 instead of wrapping");

  const auto levels = writer.captureLevels();
  expect(levels.near_full_scale_samples == 4,
         "the near-full-scale counter flags every clipping sample");
  expect(levels.peak_magnitude > 1'000.0,
         "the peak magnitude records the real input level, not the clamp");

  remove_recording(path);
}

void testLevelTelemetryIsDiagnostic() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_writer_levels.sigmf-data");
  remove_recording(path);

  IqWriter writer;
  expect(writer.open(path.string(),
                     metadataFor(IqSampleFormat::Ci16Le, 48'000.0, 0.0)),
         "a telemetry fixture opens");
  auto quiet = iqBlock(48'000.0, 0.0, 4);
  for (std::size_t index = 0; index < 4; ++index) {
    quiet.samples[index] = {0.25F, 0.75F};
  }
  expect(writer.writeBlock(quiet), "a quiet block is written");
  expect(writer.lastBlockLevels().near_full_scale_samples == 0,
         "a quiet block reports no near-full-scale samples");
  expect(std::abs(writer.lastBlockLevels().mean_real - 0.25) < 1e-6 &&
             std::abs(writer.lastBlockLevels().mean_imaginary - 0.75) < 1e-6,
         "the complex mean reports the DC offset of the block");
  expect(std::abs(writer.lastBlockLevels().peak_magnitude -
                  std::sqrt(0.25 * 0.25 + 0.75 * 0.75)) < 1e-6,
         "peak magnitude is the complex magnitude, not a per-component peak");

  auto loud = iqBlock(48'000.0, 0.0, 2);
  loud.samples[0] = {0.99F, 0.0F};
  loud.samples[1] = {0.0F, 0.0F};
  expect(writer.writeBlock(loud), "a loud block is written");
  expect(writer.lastBlockLevels().near_full_scale_samples == 1,
         "the per-block counter reports only the newest block");
  expect(writer.captureLevels().near_full_scale_samples == 1 &&
             writer.captureLevels().samples == 6,
         "the capture totals accumulate across blocks");
  writer.close();

  remove_recording(path);
}

void testMalformedInputIsRefused() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_writer_refusal.sigmf-data");
  remove_recording(path);

  IqWriter rejected;
  expect(!rejected.open(path.string(),
                        metadataFor(IqSampleFormat::Ci16Le, 0.0, 0.0)),
         "a recording without a sample rate is refused");

  IqWriter writer;
  expect(writer.open(path.string(),
                     metadataFor(IqSampleFormat::Ci16Le, 48'000.0, 0.0)),
         "a valid recording opens");
  cwassistant::core::RealtimeSampleBlock audio;
  audio.stream = {.kind = StreamKind::Audio,
                  .sample_rate_hz = 48'000.0,
                  .center_frequency_hz = 0.0,
                  .channel_count = 1};
  audio.sample_count = 4;
  expect(!writer.writeBlock(audio),
         "an audio block is refused rather than recorded as meaningless IQ");
  expect(writer.stopReason() == IqCaptureStopReason::WriteError,
         "the refusal is reported as a write error");
  writer.close();

  IqWriter retimed;
  const auto second = scratch_path("cwa_iq_writer_retimed.sigmf-data");
  remove_recording(second);
  expect(retimed.open(second.string(),
                      metadataFor(IqSampleFormat::Ci16Le, 48'000.0, 0.0)),
         "a rate-change fixture opens");
  auto good = iqBlock(48'000.0, 0.0, 2);
  expect(retimed.writeBlock(good), "the first block is written");
  auto changed = iqBlock(96'000.0, 0.0, 2);
  expect(!retimed.writeBlock(changed),
         "a mid-stream sample-rate change ends the capture");
  expect(retimed.stopReason() == IqCaptureStopReason::SampleRateChanged,
         "the sample-rate change is reported, because SigMF has one global "
         "rate and the later samples cannot be described by this file");
  retimed.close();

  remove_recording(path);
  remove_recording(second);
}

// ---------------------------------------------------------------------------
// IqReplaySource -- the inverse of the writer above. Without it a capture is
// write-only: the application can record an off-air pileup but cannot feed one
// back through the decoder, so every regression measurement is confined to the
// single-station WAV corpus.
// ---------------------------------------------------------------------------

// The writer's own rounding, duplicated here on purpose. If it and the reader
// ever disagree about the scale, this test must be the thing that notices.
std::int16_t quantize_i16(const float value) {
  const float clamped = std::clamp(value, -1.0F, 1.0F);
  return static_cast<std::int16_t>(std::lround(clamped * 32'767.0F));
}

float dequantize_i16(const std::int16_t value) {
  return static_cast<float>(value) / 32'767.0F;
}

std::vector<std::complex<float>> testSignal(const std::size_t count) {
  std::vector<std::complex<float>> samples;
  samples.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto phase = static_cast<float>(index) * 0.037F;
    samples.push_back({0.8F * std::sin(phase), 0.8F * std::cos(phase * 1.3F)});
  }
  return samples;
}

// Records `samples` through IqWriter in live-sized blocks, so the fixture is
// produced by exactly the code path an operator capture goes through.
bool writeRecording(const std::filesystem::path& path,
                    const cwassistant::core::IqCaptureMetadata& metadata,
                    const std::vector<std::complex<float>>& samples) {
  using namespace cwassistant::core;
  IqWriter writer;
  if (!writer.open(path.string(), metadata)) return false;
  RealtimeSampleBlock block;
  block.stream = {.kind = StreamKind::ComplexIq,
                  .sample_rate_hz = metadata.sample_rate_hz,
                  .center_frequency_hz = metadata.center_frequency_hz,
                  .channel_count = 1};
  const std::size_t capacity = block.samples.size();
  for (std::size_t offset = 0; offset < samples.size(); offset += capacity) {
    const std::size_t count = std::min(capacity, samples.size() - offset);
    block.sample_count = count;
    for (std::size_t index = 0; index < count; ++index) {
      block.samples[index] = samples[offset + index];
    }
    if (!writer.writeBlock(block)) return false;
  }
  writer.close();
  return true;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// The contract in one test: whatever the writer recorded, the reader returns,
// at the writer's own scale, with the stream the sidecar describes. It is also
// the mutation proof for the format details -- the byte order, the interleave
// and the 32767 scale all fail here together if any of them is changed.
void testReplayRoundTripsTheWriter() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_replay_roundtrip.sigmf-data");
  remove_recording(path);

  auto metadata = metadataFor(IqSampleFormat::Ci16Le, 62'500.0, 14'025'000.0);
  metadata.hardware = "Reference receiver";
  metadata.description = "Contest pileup fixture";
  const auto samples = testSignal(9'000);
  expect(writeRecording(path, metadata, samples),
         "the fixture recording is written");

  IqReplaySource source;
  expect(source.open(path.string(), {.kind = StreamKind::ComplexIq}),
         std::string("the recording opens: ") + source.last_error());
  expect(source.stream_descriptor().kind == StreamKind::ComplexIq,
         "replay presents a complex-IQ stream, not a downmixed audio one");
  expect(source.stream_descriptor().sample_rate_hz == 62'500.0,
         "the sample rate comes from the sidecar");
  expect(source.stream_descriptor().center_frequency_hz == 14'025'000.0,
         "the centre frequency comes from the sidecar");
  expect(source.total_frames() == samples.size(),
         "every recorded sample is available for replay");
  expect(!source.ends_mid_sample(),
         "a cleanly closed recording does not end mid-sample");
  expect(source.capture_metadata().hardware == "Reference receiver",
         "the receiver description survives the round trip");
  expect(std::abs(source.duration_seconds() - 0.144) < 1e-9,
         "the duration follows from the sample count and the sidecar rate");

  expect(source.start(), "replay starts");
  RealtimeSampleBlock block;
  std::vector<std::complex<float>> replayed;
  std::vector<std::size_t> counts;
  std::uint64_t expected_sequence = 0;
  bool sequence_ok = true;
  bool stream_ok = true;
  while (source.read(block, std::chrono::milliseconds(0))) {
    if (block.sequence != expected_sequence++) sequence_ok = false;
    if (block.stream.kind != StreamKind::ComplexIq ||
        block.stream.sample_rate_hz != 62'500.0 ||
        block.stream.center_frequency_hz != 14'025'000.0) {
      stream_ok = false;
    }
    counts.push_back(block.sample_count);
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      replayed.push_back(block.samples[index]);
    }
  }

  expect(replayed.size() == samples.size(),
         "replay returns exactly the recorded sample count");
  bool identical = replayed.size() == samples.size();
  for (std::size_t index = 0; index < replayed.size() && identical; ++index) {
    const std::complex<float> want{
        dequantize_i16(quantize_i16(samples[index].real())),
        dequantize_i16(quantize_i16(samples[index].imag()))};
    if (replayed[index] != want) identical = false;
  }
  expect(identical,
         "every sample returns at the writer's own ci16 scale, both "
         "components, in interleaved order");
  expect(counts.size() == 3 && counts[0] == 4'096 && counts[1] == 4'096 &&
             counts[2] == 808,
         "the recording is delivered in fixed-capacity blocks like a live SDR");
  expect(sequence_ok,
         "block sequence numbers increase from zero as the live path's do");
  expect(stream_ok, "every block carries the recorded stream descriptor");
  expect(!source.read(block, std::chrono::milliseconds(0)),
         "replay stops at the end of the recording");

  remove_recording(path);
}

// cf32 exists for the case where the scaling itself is under investigation, so
// its round trip must be bit-exact -- including the over-unity samples an
// overloaded front end produces, which are the evidence such a capture is
// made to preserve.
void testReplayIsBitExactForComplexFloat() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_replay_cf32.sigmf-data");
  remove_recording(path);

  const std::vector<std::complex<float>> samples{
      {0.0F, 0.5F},   {0.25F, -0.25F},  {-0.5F, 0.0F},
      {1.5F, -1.25F}, {0.125F, 0.75F},  {-0.937'5F, 0.062'5F}};
  expect(writeRecording(
             path, metadataFor(IqSampleFormat::Cf32Le, 48'000.0, 7'030'000.0),
             samples),
         "a cf32 fixture is written");

  IqReplaySource source;
  expect(source.open(path.string(), {.kind = StreamKind::ComplexIq}),
         std::string("the cf32 recording opens: ") + source.last_error());
  expect(source.capture_metadata().format == IqSampleFormat::Cf32Le,
         "the sample format comes from the sidecar, not from a default");
  expect(source.total_frames() == samples.size(),
         "cf32 costs eight bytes per complex sample and the count reflects it");
  expect(source.start(), "cf32 replay starts");

  RealtimeSampleBlock block;
  std::vector<std::complex<float>> replayed;
  while (source.read(block, std::chrono::milliseconds(0))) {
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      replayed.push_back(block.samples[index]);
    }
  }
  expect(replayed == samples,
         "cf32 replay is bit-exact, over-unity samples included");

  remove_recording(path);
}

// A capture the operator stops, or one a power loss ends, can stop part way
// through a sample. Every whole sample before that point is a real
// measurement; discarding the recording over a few trailing bytes would throw
// away the entire off-air capture.
void testTruncatedRecordingReplaysEveryWholeSample() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_replay_truncated.sigmf-data");
  remove_recording(path);

  const auto samples = testSignal(10);
  expect(writeRecording(
             path, metadataFor(IqSampleFormat::Ci16Le, 96'000.0, 3'573'000.0),
             samples),
         "a short fixture is written");

  // Nine whole ci16 samples plus half of the tenth.
  std::error_code error;
  std::filesystem::resize_file(path, 9 * 4 + 2, error);
  expect(!error, "the fixture can be truncated mid-sample");

  IqReplaySource source;
  expect(source.open(path.string(), {.kind = StreamKind::ComplexIq}),
         std::string("a truncated recording still opens: ") +
             source.last_error());
  expect(source.total_frames() == 9,
         "replay covers every whole sample before the truncation");
  expect(source.ends_mid_sample(),
         "the partial trailing sample is reported, not silently ignored");
  expect(source.declared_sample_count() == 10,
         "the sidecar still claims what the writer believed it had written");
  expect(source.start(), "a truncated recording replays");

  RealtimeSampleBlock block;
  std::size_t returned = 0;
  bool identical = true;
  while (source.read(block, std::chrono::milliseconds(0))) {
    for (std::size_t index = 0; index < block.sample_count; ++index) {
      const std::complex<float> want{
          dequantize_i16(quantize_i16(samples[returned].real())),
          dequantize_i16(quantize_i16(samples[returned].imag()))};
      if (block.samples[index] != want) identical = false;
      ++returned;
    }
  }
  expect(returned == 9, "exactly the whole samples are handed on");
  expect(identical, "the surviving samples are unchanged by the truncation");
  expect(!source.read(block, std::chrono::milliseconds(0)),
         "replay ends cleanly at the last whole sample");

  remove_recording(path);
}

// The sidecar is the only record of the rate, the centre frequency and the
// sample format. A reader that guessed any of them when the file failed to
// state it would replay a capture at a rate nobody recorded it at, or report
// every station in it on the wrong frequency, while looking entirely healthy.
void testMalformedMetadataIsRefused() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_replay_malformed.sigmf-data");
  const std::filesystem::path metadata_path(
      IqWriter::metadataPathFor(path.string()));
  remove_recording(path);

  expect(writeRecording(
             path, metadataFor(IqSampleFormat::Ci16Le, 62'500.0, 14'025'000.0),
             testSignal(4)),
         "a valid recording is written to damage");
  const std::string good = read_text(metadata_path);
  expect(!good.empty(), "the sidecar is readable");

  const auto refuse = [&](const std::string& sidecar,
                          const std::string_view expected_fragment,
                          const std::string_view why) {
    write_text(metadata_path, sidecar);
    IqReplaySource source;
    const bool opened =
        source.open(path.string(), {.kind = StreamKind::ComplexIq});
    expect(!opened, why);
    expect(contains(source.last_error(), expected_fragment),
           std::string("the refusal says why (wanted \"") +
               std::string(expected_fragment) + "\", got \"" +
               source.last_error() + "\")");
    // Nothing is left behind that a caller could mistake for a real stream.
    expect(source.stream_descriptor().sample_rate_hz == 0.0 &&
               source.total_frames() == 0,
           "a refused recording exposes no guessed sample rate or length");
  };

  const auto replaced = [&](const std::string_view from,
                            const std::string_view to) {
    std::string text = good;
    const auto position = text.find(from);
    if (position != std::string::npos) {
      text.replace(position, from.size(), to);
    }
    return text;
  };

  refuse(good.substr(0, good.size() / 2), "not valid JSON",
         "a half-written sidecar is refused");
  refuse(replaced("\"ci16_le\"", "\"cu8\""), "Unsupported SigMF datatype",
         "an unsupported datatype is refused rather than decoded as ci16");
  refuse(replaced("\"core:sample_rate\"", "\"core:sample_rat\""),
         "core:sample_rate", "a missing sample rate is refused");
  refuse(replaced("\"core:frequency\"", "\"core:frequencx\""),
         "core:frequency", "a capture segment without a centre frequency is refused");
  refuse(replaced("\"core:datatype\"", "\"core:datatyp\""), "core:datatype",
         "a missing datatype is refused");
  refuse(replaced("\"captures\"", "\"capture\""), "capture segment",
         "a sidecar with no captures array is refused");
  refuse(replaced("\"core:sample_rate\": ",
                  "\"core:sample_rate\": 48000,\n    \"core:sample_rate\": "),
         "duplicate member name",
         "a sidecar stating two different sample rates is refused, not "
         "silently resolved to one of them");
  refuse(good + "}", "trailing content",
         "trailing content after the document is refused");
  refuse(replaced("\"core:sample_rate\": 62500", "\"core:sample_rate\": 06250"),
         "leading zero",
         "a number that is not valid JSON is refused rather than read "
         "digit by digit");

  std::error_code error;
  std::filesystem::remove(metadata_path, error);
  IqReplaySource orphan;
  expect(!orphan.open(path.string(), {.kind = StreamKind::ComplexIq}),
         "a recording with no sidecar at all is refused");
  expect(contains(orphan.last_error(), "metadata"),
         "the missing sidecar is named as the reason");

  remove_recording(path);
}

// The decoder measures element lengths in nanoseconds, so a replayed capture
// only behaves as it would live if the timestamps advance at exactly the
// recorded rate. 62.5 kS/s is the rate of the reference off-air capture.
void testBlockTimestampsAdvanceAtTheSampleRate() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_replay_timing.sigmf-data");
  remove_recording(path);

  constexpr std::uint64_t kSampleRate = 62'500;
  expect(writeRecording(path,
                        metadataFor(IqSampleFormat::Ci16Le,
                                    static_cast<double>(kSampleRate),
                                    14'025'000.0),
                        testSignal(9'000)),
         "a timing fixture is written");

  IqReplaySource source;
  expect(source.open(path.string(), {.kind = StreamKind::ComplexIq}),
         std::string("the timing fixture opens: ") + source.last_error());
  expect(source.start(), "timing replay starts");

  RealtimeSampleBlock block;
  std::uint64_t consumed = 0;
  std::uint64_t previous = 0;
  bool first = true;
  bool exact = true;
  bool monotonic = true;
  std::size_t blocks = 0;
  while (source.read(block, std::chrono::milliseconds(0))) {
    const std::uint64_t expected_ns =
        consumed * 1'000'000'000ULL / kSampleRate;
    if (block.timestamp_ns != expected_ns) exact = false;
    if (!first && block.timestamp_ns <= previous) monotonic = false;
    first = false;
    previous = block.timestamp_ns;
    consumed += block.sample_count;
    ++blocks;
  }
  expect(blocks == 3, "the fixture is delivered as three blocks");
  expect(exact,
         "each block is stamped at its own first sample, derived from the "
         "recorded sample rate");
  expect(monotonic, "timestamps increase from block to block");
  // 4096 samples at 62.5 kS/s is 65.536 ms exactly; a reader that stamped
  // blocks by arrival, or at a nominal rate, would not land here.
  expect(consumed == 9'000, "the whole fixture is consumed");

  remove_recording(path);
}

// The writer starts a new SigMF capture segment when the operator retunes, so
// a recording that spans a frequency change stays self-describing. A block
// that straddled that boundary would carry one centre frequency for samples
// received on two, and every spot derived from the later half would be
// reported on the wrong frequency.
void testRetuneEndsTheBlock() {
  using namespace cwassistant::core;
  const auto path = scratch_path("cwa_iq_replay_retune.sigmf-data");
  remove_recording(path);

  {
    IqWriter writer;
    expect(writer.open(path.string(),
                       metadataFor(IqSampleFormat::Ci16Le, 48'000.0,
                                   14'025'000.0)),
           "a retune fixture opens");
    auto first = iqBlock(48'000.0, 14'025'000.0, 100);
    expect(writer.writeBlock(first), "the pre-retune samples are written");
    auto second = iqBlock(48'000.0, 14'031'000.0, 60);
    expect(writer.writeBlock(second), "the post-retune samples are written");
    writer.close();
  }

  IqReplaySource source;
  expect(source.open(path.string(), {.kind = StreamKind::ComplexIq}),
         std::string("the retune fixture opens: ") + source.last_error());
  expect(source.capture_segments().size() == 2,
         "both capture segments are recovered from the sidecar");
  expect(source.capture_segments().size() == 2 &&
             source.capture_segments()[1].sample_start == 100,
         "the retune is placed at the sample it happened on");
  expect(source.start(), "retune replay starts");

  RealtimeSampleBlock block;
  std::vector<std::pair<std::size_t, double>> delivered;
  while (source.read(block, std::chrono::milliseconds(0))) {
    delivered.emplace_back(block.sample_count,
                           block.stream.center_frequency_hz);
  }
  expect(delivered.size() == 2,
         "a block never spans a retune, even though both fit in one block");
  expect(delivered.size() == 2 && delivered[0].first == 100 &&
             delivered[0].second == 14'025'000.0,
         "the samples before the retune carry the frequency they were "
         "received on");
  expect(delivered.size() == 2 && delivered[1].first == 60 &&
             delivered[1].second == 14'031'000.0,
         "the samples after the retune carry the new frequency");

  remove_recording(path);
}

}  // namespace

int main() {
  testComplexRoundTrip();
  testComplexFloatRoundTripIsBitExact();
  testSidecarDescribesTheRecording();
  testDurationBudgetStopsAndReports();
  testByteBudgetStopsAndReports();
  testCi16ScalingClampsRatherThanWraps();
  testLevelTelemetryIsDiagnostic();
  testMalformedInputIsRefused();
  testReplayRoundTripsTheWriter();
  testReplayIsBitExactForComplexFloat();
  testTruncatedRecordingReplaysEveryWholeSample();
  testMalformedMetadataIsRefused();
  testBlockTimestampsAdvanceAtTheSampleRate();
  testRetuneEndsTheBlock();
  if (failures == 0) {
    std::cout << "All IQ writer tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
