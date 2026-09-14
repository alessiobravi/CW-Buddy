#pragma once

#include "cwassistant/core/iq_receive.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cwassistant::desktop {

struct SdrDeviceDescriptor {
  std::string id;
  std::string label;
  std::string driver;
  std::string serial;
  std::string physical_id;
  std::string physical_label;
  std::string mode_id;
  std::string mode_label;
  bool recommended_mode{false};
};

struct SdrPhysicalDeviceDescriptor {
  std::string id;
  std::string label;
  std::vector<SdrDeviceDescriptor> modes;
};

[[nodiscard]] std::vector<SdrPhysicalDeviceDescriptor> groupSdrDevices(
    const std::vector<SdrDeviceDescriptor>& devices);

struct SdrDiscoveryReport {
  bool backend_available{false};
  std::string backend_version;
  std::vector<std::string> modules;
  // Factory names whose modules loaded and registered without an error.
  // This distinguishes a module file being present from a usable driver.
  std::vector<std::string> loaded_drivers;
  std::vector<std::string> module_load_errors;
  std::vector<SdrDeviceDescriptor> devices;
  std::string diagnostic;
};

struct SdrDeviceCapabilities {
  bool available{false};
  std::vector<double> sample_rates_hz;
  std::vector<double> bandwidths_hz;
  std::vector<std::string> antennas;
  bool automatic_gain_available{false};
  double minimum_gain_db{0.0};
  double maximum_gain_db{0.0};
  double gain_step_db{0.0};
  std::string diagnostic;
};

struct SdrReceiveConfiguration {
  std::string device_id;
  double center_frequency_hz{0.0};
  double sample_rate_hz{250'000.0};
  // Zero asks the provider to retain its automatic/default RF filter width.
  double bandwidth_hz{0.0};
  std::string antenna;
  bool automatic_gain{true};
  double gain_db{0.0};
};

// How far the decode window's centre may sit from the acquisition centre.
//
// IqSubbandDecimator::process() refuses a block outright unless
//   |decoder centre - capture centre| + decoder bandwidth / 2 < Nyquist,
// so the arithmetic limit is sample rate / 2 - bandwidth / 2. The margin
// subtracted below that is not decoration: the decimator's anti-alias
// response is only meaningful with some spectrum left above the window edge.
//
// One rule, one definition. It used to be written twice with different
// numbers -- the LO-offset clamp allowed an offset a kilohertz larger than
// the decode-window publisher would accept -- so every offset big enough to
// be clamped was then judged out of reach and snapped onto the acquisition
// centre, which applies the LO offset a second time to every frequency the
// operator is shown. Tens of kilohertz at ordinary IQ rates, permanently, and
// invisibly: the correction is made downstream of the settings pane, which
// goes on displaying the value it was configured with.
[[nodiscard]] constexpr double sdrDecoderWindowReachHz(
    const double sample_rate_hz,
    const double decoder_bandwidth_hz) noexcept {
  constexpr double kEdgeMarginHz = 2'000.0;
  return sample_rate_hz * 0.5 - decoder_bandwidth_hz * 0.5 - kEdgeMarginHz;
}

struct SdrActualConfiguration {
  double center_frequency_hz{0.0};
  double sample_rate_hz{0.0};
  double bandwidth_hz{0.0};
  bool automatic_gain{false};
  double gain_db{0.0};
};

struct SdrReadResult {
  std::size_t sample_count{0};
  std::uint64_t timestamp_ns{0};
  bool timestamp_valid{false};
  bool overflow{false};
  bool timeout{false};
  std::string error;
};

// Device APIs stay behind this RX-only contract. There is deliberately no TX,
// PTT, or KEY operation at this boundary.
class SdrReceiveBackend {
 public:
  virtual ~SdrReceiveBackend() = default;

  [[nodiscard]] virtual SdrDiscoveryReport discover() = 0;
  [[nodiscard]] virtual SdrDeviceCapabilities probe(
      const std::string& device_id) = 0;
  [[nodiscard]] virtual bool open(const SdrReceiveConfiguration& configuration,
                                  SdrActualConfiguration& actual,
                                  std::string& error) = 0;
  [[nodiscard]] virtual bool retuneCenterFrequency(
      double center_frequency_hz, double& actual_center_frequency_hz,
      std::string& error) = 0;
  [[nodiscard]] virtual SdrReadResult read(
      std::span<std::complex<float>> samples, long timeout_microseconds) = 0;
  virtual void close() noexcept = 0;
};

struct SdrReceiverDiagnostics {
  bool running{false};
  std::uint64_t blocks{0};
  std::uint64_t samples{0};
  std::uint64_t overflows{0};
  std::uint64_t timeouts{0};
  std::uint64_t read_errors{0};
  std::uint64_t invalid_blocks{0};
  std::uint64_t discontinuities{0};
  // Samples dropped on purpose across a retune, because they were captured
  // before the receiver moved and cannot honestly carry the new frequency.
  std::uint64_t retune_discarded_samples{0};
  std::string last_error;
};

// Converts backend reads into the same fixed, timestamped IQ block contract
// consumed by the receiver DSP. The owner decides which thread calls pump().
class SdrReceiver final {
 public:
  explicit SdrReceiver(std::unique_ptr<SdrReceiveBackend> backend);
  ~SdrReceiver();

  SdrReceiver(const SdrReceiver&) = delete;
  SdrReceiver& operator=(const SdrReceiver&) = delete;

  [[nodiscard]] SdrDiscoveryReport discover();
  [[nodiscard]] SdrDeviceCapabilities probe(const std::string& device_id);
  [[nodiscard]] bool start(const SdrReceiveConfiguration& configuration,
                           std::string& error);
  [[nodiscard]] bool retuneCenterFrequency(double center_frequency_hz,
                                           std::string& error);
  void stop() noexcept;
  [[nodiscard]] bool pump(core::RealtimeSampleBlock& block,
                          long timeout_microseconds = 100'000);
  [[nodiscard]] const SdrReceiverDiagnostics& diagnostics() const noexcept;
  [[nodiscard]] const SdrActualConfiguration& actualConfiguration() const noexcept;

 private:
  // Empties whatever the backend has already queued, and answers how many
  // samples that was. Called across a retune; see the definition for why the
  // rule lives here and not in one provider's backend.
  [[nodiscard]] std::size_t discardQueuedSamples();

  std::unique_ptr<SdrReceiveBackend> backend_;
  SdrReceiveConfiguration configuration_{};
  SdrActualConfiguration actual_{};
  SdrReceiverDiagnostics diagnostics_{};
  core::IqReceiveValidator validator_{};
  std::uint64_t sequence_{0};
  std::uint64_t synthesized_timestamp_ns_{0};
  // Scratch for discardQueuedSamples(). Held rather than made on the stack
  // because the drain runs on the capture thread, in the middle of a retune.
  core::RealtimeSampleBlock drain_block_{};
};

// Always available. Without SoapySDR it returns a diagnostic-only backend so
// packaged default builds remain usable and can explain how to enable SDR.
[[nodiscard]] std::unique_ptr<SdrReceiveBackend> makeSoapySdrReceiveBackend();

}  // namespace cwassistant::desktop
