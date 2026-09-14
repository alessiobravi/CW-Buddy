#include "sdr/sdr_receiver.hpp"

#include <cmath>
#include <complex>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace {

void expect(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

class FakeBackend final : public cwassistant::desktop::SdrReceiveBackend {
 public:
  cwassistant::desktop::SdrDiscoveryReport report{
      .backend_available = true,
      .backend_version = "fake-1",
      .modules = {"fake-module"},
      .loaded_drivers = {"fake"},
      .devices = {{.id = "fake:01:0",
                   .label = "Fake RX",
                   .driver = "fake",
                   .serial = "01"}},
      .diagnostic = "Fake receiver ready."};
  cwassistant::desktop::SdrActualConfiguration actual{
      .center_frequency_hz = 14'050'000.0,
      .sample_rate_hz = 250'000.0,
      .automatic_gain = false,
      .gain_db = 17.0};
  bool open_result{true};
  bool retune_result{true};
  bool opened{false};
  int retune_calls{0};
  std::deque<cwassistant::desktop::SdrReadResult> reads;
  bool corrupt_next_sample{false};

  cwassistant::desktop::SdrDiscoveryReport discover() override {
    return report;
  }

  cwassistant::desktop::SdrDeviceCapabilities probe(
      const std::string& device_id) override {
    return {.available = device_id == "fake:01:0",
            .sample_rates_hz = {96'000.0, 250'000.0},
            .bandwidths_hz = {200'000.0, 300'000.0},
            .antennas = {"RX"},
            .automatic_gain_available = true,
            .minimum_gain_db = -10.0,
            .maximum_gain_db = 50.0,
            .gain_step_db = 1.0,
            .diagnostic = "Fake capabilities ready."};
  }

  bool open(const cwassistant::desktop::SdrReceiveConfiguration&,
            cwassistant::desktop::SdrActualConfiguration& output,
            std::string& error) override {
    opened = open_result;
    output = actual;
    if (!open_result) error = "fake open failed";
    return open_result;
  }

  bool retuneCenterFrequency(const double center_frequency_hz,
                             double& actual_center_frequency_hz,
                             std::string& error) override {
    ++retune_calls;
    if (!retune_result) {
      error = "fake retune failed";
      return false;
    }
    actual.center_frequency_hz = center_frequency_hz;
    actual_center_frequency_hz = center_frequency_hz;
    return true;
  }

  cwassistant::desktop::SdrReadResult read(
      std::span<std::complex<float>> samples, long) override {
    if (reads.empty()) return {.timeout = true};
    auto result = reads.front();
    reads.pop_front();
    for (std::size_t index = 0;
         index < std::min(result.sample_count, samples.size()); ++index) {
      samples[index] = {static_cast<float>(index),
                        -static_cast<float>(index)};
    }
    if (corrupt_next_sample && result.sample_count > 0) {
      samples[0] = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
      corrupt_next_sample = false;
    }
    return result;
  }

  void close() noexcept override { opened = false; }
};

}  // namespace

int main() {
  using namespace cwassistant::desktop;

  {
    const std::vector<SdrDeviceDescriptor> variants{
        {.id = "sdrplay:1806067C32:DT",
         .label = "RSPduo dual",
         .driver = "sdrplay",
         .serial = "1806067C32",
         .physical_id = "sdrplay:1806067C32",
         .physical_label = "SDRplay RSPduo",
         .mode_id = "DT",
         .mode_label = "DT - Dual tuner"},
        {.id = "sdrplay:1806067C32:ST",
         .label = "RSPduo single",
         .driver = "sdrplay",
         .serial = "1806067C32",
         .physical_id = "sdrplay:1806067C32",
         .physical_label = "SDRplay RSPduo",
         .mode_id = "ST",
         .mode_label = "ST - Single tuner",
         .recommended_mode = true},
        {.id = "sdrplay:1806067C32:MA",
         .label = "RSPduo master",
         .driver = "sdrplay",
         .serial = "1806067C32",
         .physical_id = "sdrplay:1806067C32",
         .physical_label = "SDRplay RSPduo",
         .mode_id = "MA",
         .mode_label = "MA - Master 6 MHz"},
        {.id = "sdrplay:1806067C32:MA8",
         .label = "RSPduo master 8 MHz",
         .driver = "sdrplay",
         .serial = "1806067C32",
         .physical_id = "sdrplay:1806067C32",
         .physical_label = "SDRplay RSPduo",
         .mode_id = "MA8",
         .mode_label = "MA8 - Master 8 MHz"},
        {.id = "rtlsdr:00000001:0",
         .label = "Generic RTL2832U",
         .driver = "rtlsdr",
         .serial = "00000001",
         .physical_id = "rtlsdr:00000001",
         .physical_label = "Generic RTL2832U",
         .mode_id = "default",
         .mode_label = "Default",
         .recommended_mode = true}};
    const auto grouped = groupSdrDevices(variants);
    expect(grouped.size() == 2 && grouped.front().modes.size() == 4,
           "same-serial SDR operating modes group as one physical receiver");
    expect(grouped.front().modes.front().mode_id == "ST" &&
               grouped.back().id == "rtlsdr:00000001",
           "recommended mode sorts first without merging another receiver");
  }

  {
    SdrReceiver unavailable(makeSoapySdrReceiveBackend());
    const auto report = unavailable.discover();
    expect(!report.backend_available,
           "default test build reports SoapySDR unavailable");
    expect(report.diagnostic.find("CWA_ENABLE_SOAPY_SDR=ON") !=
               std::string::npos,
           "unavailable diagnostic explains how to enable SoapySDR");
  }

  auto fake = std::make_unique<FakeBackend>();
  auto* fake_view = fake.get();
  SdrReceiver receiver(std::move(fake));
  const auto report = receiver.discover();
  expect(report.backend_available && report.devices.size() == 1 &&
             report.loaded_drivers == std::vector<std::string>{"fake"},
         "fake discovery crosses backend-neutral contract");
  const auto capabilities = receiver.probe("fake:01:0");
  expect(capabilities.available && capabilities.sample_rates_hz.size() == 2 &&
             capabilities.antennas == std::vector<std::string>{"RX"},
         "device capabilities cross the backend-neutral contract");

  std::string error;
  expect(!receiver.start({}, error) && error.find("device") != std::string::npos,
         "invalid configuration fails before hardware open");
  expect(!fake_view->opened, "invalid configuration never opens hardware");

  SdrReceiveConfiguration out_of_bounds{
      .device_id = "fake:01:0",
      .center_frequency_hz = 100'000'000'000.0,
      .sample_rate_hz = 250'000.0,
      .automatic_gain = true,
      .gain_db = 0.0};
  expect(!receiver.start(out_of_bounds, error) && !fake_view->opened,
         "out-of-contract frequency is rejected before hardware open");

  const SdrReceiveConfiguration requested{
      .device_id = "fake:01:0",
      .center_frequency_hz = 14'049'900.0,
      .sample_rate_hz = 240'000.0,
      .automatic_gain = false,
      .gain_db = 17.0};
  expect(receiver.start(requested, error), "valid fake receiver starts");
  expect(receiver.actualConfiguration().sample_rate_hz == 250'000.0,
         "receiver exposes authoritative device sample-rate readback");

  fake_view->reads.push_back({.timeout = true});
  cwassistant::core::RealtimeSampleBlock block;
  expect(!receiver.pump(block, 1), "read timeout produces no block");
  expect(receiver.diagnostics().timeouts == 1 &&
             receiver.diagnostics().read_errors == 0,
         "timeouts are distinct from read errors");

  fake_view->reads.push_back({.sample_count = 3,
                              .timestamp_ns = 9'000,
                              .timestamp_valid = true});
  expect(receiver.pump(block, 1), "timestamped IQ block is published");
  expect(block.stream.kind == cwassistant::core::StreamKind::ComplexIq &&
             block.stream.sample_rate_hz == 250'000.0 &&
             block.stream.center_frequency_hz == 14'050'000.0,
         "IQ descriptor uses authoritative hardware readback");
  expect(block.sequence == 0 && block.timestamp_ns == 9'000 &&
             block.sample_count == 3 && block.samples[2].real() == 2.0F,
         "IQ samples, sequence, and hardware timestamp are preserved");

  // Whatever the device had already queued when the retune was asked for was
  // captured on the old frequency. Publishing it stamped with the new one is
  // not a passing smear: a track discovered inside that block latches its
  // identity origin at a frequency it was never received on, and stays
  // clamped to it for the rest of its life. On a band jump that is tens of
  // kilohertz of permanent error on a station's reported frequency.
  fake_view->reads.push_back({.sample_count = 5,
                              .timestamp_ns = 20'000,
                              .timestamp_valid = true});
  expect(receiver.retuneCenterFrequency(14'075'000.0, error) &&
             fake_view->retune_calls == 1 &&
             receiver.diagnostics().running,
         "center-only retune preserves the running receiver");
  expect(receiver.diagnostics().retune_discarded_samples == 5 &&
             fake_view->reads.empty(),
         "samples queued before the retune are dropped, not relabelled");
  fake_view->reads.push_back({.sample_count = 2,
                              .timestamp_ns = 22'000,
                              .timestamp_valid = true});
  expect(receiver.pump(block, 1) &&
             block.stream.center_frequency_hz == 14'075'000.0 &&
             block.sample_count == 2 && block.timestamp_ns == 22'000 &&
             receiver.diagnostics().discontinuities == 1,
         "first retuned block carries authoritative RF and a discontinuity");
  fake_view->retune_result = false;
  expect(!receiver.retuneCenterFrequency(14'076'000.0, error) &&
             receiver.diagnostics().running &&
             receiver.actualConfiguration().center_frequency_hz ==
                 14'075'000.0,
         "failed retune leaves the current stream running and unchanged");

  fake_view->reads.push_back({.overflow = true});
  expect(!receiver.pump(block, 1), "device overflow produces no block");
  expect(receiver.diagnostics().overflows == 1,
         "device overflow telemetry is retained");

  fake_view->reads.push_back({.sample_count = 2});
  expect(receiver.pump(block, 1), "untimestamped IQ block is published");
  // Sequence 4, not 3: the retune above discarded a queued block, and a
  // deliberate hole is marked the same way an overflow's is, so the next
  // block reads as discontinuous instead of being joined onto the last one.
  expect(block.sequence == 4 && block.timestamp_ns == 30'000 &&
             receiver.diagnostics().discontinuities == 2,
         "overflow creates an explicit discontinuity before monotonic sample time resumes");

  fake_view->corrupt_next_sample = true;
  fake_view->reads.push_back({.sample_count = 2});
  expect(!receiver.pump(block, 1), "invalid IQ samples fail closed");
  expect(receiver.diagnostics().invalid_blocks == 1 &&
             receiver.diagnostics().read_errors == 1,
         "invalid provider blocks are counted separately");

  fake_view->reads.push_back({.error = "fake USB disconnect"});
  expect(!receiver.pump(block, 1), "backend error produces no block");
  expect(receiver.diagnostics().read_errors == 2 &&
             receiver.diagnostics().last_error == "fake USB disconnect",
         "actionable backend read error is retained");

  receiver.stop();
  expect(!fake_view->opened && !receiver.diagnostics().running,
         "stop closes RX backend");

  fake_view->actual.sample_rate_hz = 100'000'000.0;
  expect(!receiver.start(requested, error) && !fake_view->opened,
         "unsafe actual hardware readback closes the receiver before streaming");
  std::cout << "SDR receiver contract tests passed\n";
  return 0;
}
