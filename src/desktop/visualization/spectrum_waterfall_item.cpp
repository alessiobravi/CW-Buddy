#include "spectrum_waterfall_item.hpp"

#include <QColor>
#include <QImage>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGTexture>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "../replay/replay_controller.hpp"

namespace cwassistant::desktop {
namespace {

constexpr int kMaximumWaterfallRows = 3'600;

class DisplayNode final : public QSGNode {
 public:
  DisplayNode() {
    grid = makeGeometryNode(QSGGeometry::DrawLines, QColor("#2a3a49"));
    appendChildNode(grid);
    spectrum = makeGeometryNode(QSGGeometry::DrawLineStrip,
                                QColor("#64e6d2"));
    appendChildNode(spectrum);
  }

  static QSGGeometryNode* makeGeometryNode(
      const QSGGeometry::DrawingMode mode, const QColor& color) {
    auto* node = new QSGGeometryNode;
    auto* geometry =
        new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
    geometry->setDrawingMode(mode);
    geometry->setLineWidth(1.0F);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    auto* material = new QSGFlatColorMaterial;
    material->setColor(color);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
  }

  QSGImageNode* waterfall{nullptr};
  QSGGeometryNode* grid{nullptr};
  QSGGeometryNode* spectrum{nullptr};
};

// ReplayController::sourceMode() reports 2 for the direct SDR path, the only
// source that delivers centre-shifted complex IQ to this item.
constexpr int kDirectIqSourceMode = 2;

// A wide SDR overview carries far more bins than the display has pixel
// columns: 16'384 bins across a ~1'500 px item is about eleven bins per
// column. One vertex per bin makes every column rasterise as a bar spanning
// the minimum to the maximum of its eleven bins, which is the spiky trace,
// and it forces an ~11x bilinear minification of the waterfall raster with no
// mipmaps, which is the speckle. Both disappear once the reduction to columns
// happens here, before geometry and image are built, instead of in the
// rasteriser.
void columnBinRange(const qsizetype column, const qsizetype column_count,
                    const qsizetype first_bin, const qsizetype visible_bins,
                    qsizetype& begin, qsizetype& end) noexcept {
  begin = first_bin + (column * visible_bins) / column_count;
  end = first_bin + ((column + 1) * visible_bins) / column_count;
  // column_count never exceeds visible_bins, so every column already owns at
  // least one bin; the guard only keeps a degenerate range impossible.
  if (end <= begin) end = begin + 1;
}

// Peak, not mean. A CW carrier occupies one or two of the eleven bins that
// share a column, so averaging them would bury it about 10 dB below its true
// level and hide weak signals the operator is looking for. The analyzer
// already averages each bin over time, so the peak of averaged bins is a
// stable reading rather than the noisiest sample in the group.
double columnPeakDb(const QVector<float>& bins, const qsizetype begin,
                    const qsizetype end) {
  double peak = -std::numeric_limits<double>::infinity();
  const qsizetype first = std::max<qsizetype>(0, begin);
  const qsizetype last = std::min(end, bins.size());
  for (qsizetype index = first; index < last; ++index) {
    const double value = static_cast<double>(bins[index]);
    if (std::isfinite(value) && value > peak) peak = value;
  }
  return peak;
}

// How far the trace's baseline sits below the estimated noise floor. The
// palette wants to start just under the noise so that none of its range is
// spent on it; the trace wants the opposite, because a floor drawn on the
// bottom axis tells an operator nothing about how far a signal stands above
// it. They are therefore not the same number.
constexpr double kTraceFloorHeadroomDb = 12.0;
constexpr double kWaterfallFloorOffsetDb = 10.0;

QRgb waterfallColor(const float value) {
  const float t = std::clamp(value, 0.0F, 1.0F);
  if (t < 0.35F) {
    const float u = t / 0.35F;
    return qRgb(static_cast<int>(4.0F + 8.0F * u),
                static_cast<int>(12.0F + 48.0F * u),
                static_cast<int>(26.0F + 94.0F * u));
  }
  if (t < 0.72F) {
    const float u = (t - 0.35F) / 0.37F;
    return qRgb(static_cast<int>(12.0F + 46.0F * u),
                static_cast<int>(60.0F + 170.0F * u),
                static_cast<int>(120.0F + 75.0F * u));
  }
  const float u = (t - 0.72F) / 0.28F;
  return qRgb(static_cast<int>(58.0F + 197.0F * u),
              static_cast<int>(230.0F + 25.0F * u),
              static_cast<int>(195.0F - 95.0F * u));
}

}  // namespace

SpectrumWaterfallItem::SpectrumWaterfallItem(QQuickItem* parent)
    : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  render_clock_.start();
}

QObject* SpectrumWaterfallItem::source() const noexcept { return source_; }

void SpectrumWaterfallItem::setSource(QObject* source) {
  if (source_ == source) {
    return;
  }
  if (source_ != nullptr) {
    disconnect(source_, nullptr, this, nullptr);
  }
  source_ = source;
  if (auto* replay = qobject_cast<ReplayController*>(source_)) {
    connect(replay, &ReplayController::frameReady, this,
            &SpectrumWaterfallItem::acceptFrame);
    connect(replay, &ReplayController::sourceReset, this,
            &SpectrumWaterfallItem::resetFrames);
  }
  resetFrames();
  emit sourceChanged();
}

double SpectrumWaterfallItem::lowerBoundDb() const noexcept {
  return lower_bound_db_;
}
void SpectrumWaterfallItem::setLowerBoundDb(const double value) {
  const double clamped = std::clamp(value, -200.0, 40.0);
  if (qFuzzyCompare(lower_bound_db_, clamped)) return;
  lower_bound_db_ = clamped;
  if (!automatic_range_) {
    effective_lower_bound_db_ = lower_bound_db_;
    emit rangeChanged();
  }
  emit displayChanged();
  update();
}
double SpectrumWaterfallItem::upperBoundDb() const noexcept {
  return upper_bound_db_;
}
void SpectrumWaterfallItem::setUpperBoundDb(const double value) {
  const double clamped = std::clamp(value, -190.0, 50.0);
  if (qFuzzyCompare(upper_bound_db_, clamped)) return;
  upper_bound_db_ = clamped;
  if (!automatic_range_) {
    effective_upper_bound_db_ = upper_bound_db_;
    emit rangeChanged();
  }
  emit displayChanged();
  update();
}
bool SpectrumWaterfallItem::automaticRange() const noexcept {
  return automatic_range_;
}
void SpectrumWaterfallItem::setAutomaticRange(const bool value) {
  if (automatic_range_ == value) return;
  automatic_range_ = value;
  if (!automatic_range_) {
    automatic_range_initialized_ = false;
    effective_lower_bound_db_ = lower_bound_db_;
    effective_upper_bound_db_ = upper_bound_db_;
    emit rangeChanged();
  } else if (!latest_bins_.isEmpty()) {
    updateAutomaticRange(latest_bins_);
  }
  emit displayChanged();
  update();
}
double SpectrumWaterfallItem::automaticRangeSpanDb() const noexcept {
  return automatic_range_span_db_;
}
void SpectrumWaterfallItem::setAutomaticRangeSpanDb(const double value) {
  const double clamped = std::clamp(value, 30.0, 100.0);
  if (qFuzzyCompare(automatic_range_span_db_, clamped)) return;
  automatic_range_span_db_ = clamped;
  if (automatic_range_ && !latest_bins_.isEmpty()) {
    automatic_range_initialized_ = false;
    updateAutomaticRange(latest_bins_);
  }
  emit displayChanged();
  update();
}
bool SpectrumWaterfallItem::noiseSuppression() const noexcept {
  return noise_suppression_;
}
void SpectrumWaterfallItem::setNoiseSuppression(const bool value) {
  if (noise_suppression_ == value) return;
  noise_suppression_ = value;
  waterfall_rows_.clear();
  last_row_timestamp_ns_ = 0;
  has_row_timestamp_ = false;
  emit displayChanged();
  update();
}
double SpectrumWaterfallItem::noiseMarginDb() const noexcept {
  return noise_margin_db_;
}
void SpectrumWaterfallItem::setNoiseMarginDb(const double value) {
  const double clamped = std::clamp(value, 0.0, 30.0);
  if (qFuzzyCompare(noise_margin_db_, clamped)) return;
  noise_margin_db_ = clamped;
  waterfall_rows_.clear();
  last_row_timestamp_ns_ = 0;
  has_row_timestamp_ = false;
  emit displayChanged();
  update();
}
int SpectrumWaterfallItem::targetFps() const noexcept { return target_fps_; }
void SpectrumWaterfallItem::setTargetFps(const int value) {
  const int clamped = std::clamp(value, 10, 120);
  if (target_fps_ == clamped) return;
  target_fps_ = clamped;
  emit displayChanged();
}
int SpectrumWaterfallItem::waterfallRate() const noexcept {
  return waterfall_rate_;
}
void SpectrumWaterfallItem::setWaterfallRate(const int value) {
  const int clamped = std::clamp(value, 1, 120);
  if (waterfall_rate_ == clamped) return;
  waterfall_rate_ = clamped;
  waterfall_rows_.clear();
  has_row_timestamp_ = false;
  emit displayChanged();
  update();
}
int SpectrumWaterfallItem::waterfallTimeSpanSeconds() const noexcept {
  return waterfall_time_span_seconds_;
}
int SpectrumWaterfallItem::displayMode() const noexcept {
  return display_mode_;
}
void SpectrumWaterfallItem::setDisplayMode(const int value) {
  const int clamped = std::clamp(value, 0, 1);
  if (display_mode_ == clamped) return;
  display_mode_ = clamped;
  waterfall_rows_.clear();
  conditioner_.reset();
  has_row_timestamp_ = false;
  emit displayChanged();
  update();
}
void SpectrumWaterfallItem::setWaterfallTimeSpanSeconds(const int value) {
  const int clamped = std::clamp(value, 5, 30);
  if (waterfall_time_span_seconds_ == clamped) return;
  waterfall_time_span_seconds_ = clamped;
  while (waterfall_rows_.size() >
         static_cast<std::size_t>(waterfallRowCapacity())) {
    waterfall_rows_.pop_back();
  }
  emit displayChanged();
  update();
}
int SpectrumWaterfallItem::waterfallRowCapacity() const noexcept {
  return std::clamp(waterfall_rate_ * waterfall_time_span_seconds_, 1,
                    kMaximumWaterfallRows);
}
int SpectrumWaterfallItem::storedWaterfallRows() const noexcept {
  return static_cast<int>(waterfall_rows_.size());
}
bool SpectrumWaterfallItem::showGrid() const noexcept { return show_grid_; }
void SpectrumWaterfallItem::setShowGrid(const bool value) {
  if (show_grid_ == value) return;
  show_grid_ = value;
  emit displayChanged();
  update();
}
double SpectrumWaterfallItem::effectiveLowerBoundDb() const noexcept {
  return effective_lower_bound_db_;
}
double SpectrumWaterfallItem::effectiveUpperBoundDb() const noexcept {
  return effective_upper_bound_db_;
}
double SpectrumWaterfallItem::lowerFrequencyHz() const noexcept {
  return lower_frequency_hz_;
}
double SpectrumWaterfallItem::upperFrequencyHz() const noexcept {
  return upper_frequency_hz_;
}
double SpectrumWaterfallItem::sourceLowerFrequencyHz() const noexcept {
  return source_lower_frequency_hz_;
}
double SpectrumWaterfallItem::sourceUpperFrequencyHz() const noexcept {
  return source_upper_frequency_hz_;
}
double SpectrumWaterfallItem::preferredSpanHz() const noexcept {
  return preferred_span_hz_;
}

void SpectrumWaterfallItem::setPreferredSpanHz(const double span_hz) {
  const double sanitized =
      std::isfinite(span_hz) && span_hz > 0.0 ? span_hz : 0.0;
  if (qFuzzyCompare(preferred_span_hz_, sanitized)) return;
  preferred_span_hz_ = sanitized;
  // Deliberately not applied to the view in flight: changing the preference
  // while watching would yank the display out from under the operator. It
  // takes effect the next time the view is established, which is exactly when
  // a new receiver setting reaches the spectrum anyway.
  emit displayChanged();
}

bool SpectrumWaterfallItem::zoomed() const noexcept {
  return view_initialized_ &&
      (lower_frequency_hz_ > source_lower_frequency_hz_ + 0.5 ||
       upper_frequency_hz_ < source_upper_frequency_hz_ - 0.5);
}
qsizetype SpectrumWaterfallItem::appliedRowShiftBins() const noexcept {
  return applied_row_shift_bins_;
}

int SpectrumWaterfallItem::waterfallRowCount() const noexcept {
  return static_cast<int>(waterfall_rows_.size());
}

qulonglong SpectrumWaterfallItem::droppedRows() const noexcept {
  return dropped_rows_;
}
double SpectrumWaterfallItem::estimatedNoiseFloorDb() const noexcept {
  return estimated_noise_floor_db_;
}

void SpectrumWaterfallItem::acceptFrame(const SpectrumFrame& frame) {
  if (frame.bins_dbfs.isEmpty()) return;
  if (!latest_bins_.isEmpty() && latest_bins_.size() != frame.bins_dbfs.size()) {
    waterfall_rows_.clear();
    has_row_timestamp_ = false;
    // A carried remainder is measured in bins, so it means nothing once the
    // bins change width.
    row_shift_residual_bins_ = 0.0;
    applied_row_shift_bins_ = 0;
  }
  latest_bins_ = frame.bins_dbfs;
  suppressLocalOscillatorBin(latest_bins_);
  const QVector<float>& waterfall_bins =
      display_mode_ == 1 &&
              frame.instantaneous_bins_dbfs.size() == frame.bins_dbfs.size()
          ? frame.instantaneous_bins_dbfs
          : latest_bins_;
  const bool source_changed =
      !qFuzzyCompare(source_lower_frequency_hz_, frame.lower_frequency_hz) ||
      !qFuzzyCompare(source_upper_frequency_hz_, frame.upper_frequency_hz);
  // A retune moves the absolute RF bounds of every SDR frame. Snapping back to
  // the full span there threw away the operator's zoom on each frequency
  // change; audio frames sit on a tuning-invariant axis and never reach this
  // path. Read the current zoom before the bounds move under it.
  const bool preserve_zoom = source_changed && zoomed() &&
      frame.upper_frequency_hz > frame.lower_frequency_hz;
  const double previous_source_center_hz =
      0.5 * (source_lower_frequency_hz_ + source_upper_frequency_hz_);
  const double previous_view_span_hz =
      upper_frequency_hz_ - lower_frequency_hz_;
  if (source_changed) {
    const double previous_span_hz =
        source_upper_frequency_hz_ - source_lower_frequency_hz_;
    const double next_span_hz =
        frame.upper_frequency_hz - frame.lower_frequency_hz;
    const double shift_hz =
        frame.lower_frequency_hz - source_lower_frequency_hz_;
    source_lower_frequency_hz_ = frame.lower_frequency_hz;
    source_upper_frequency_hz_ = frame.upper_frequency_hz;

    // A row is a history of frequency, so when the receiver moves the history
    // is still true -- it simply sits at different bins now. Retuning an SDR
    // changes the absolute bounds of every frame, and discarding the waterfall
    // on each step wiped the display at every click of the dial, which is not
    // what the same action does on audio, whose axis does not move with
    // tuning. Slide the rows instead, by the same number of bins the band
    // moved, so what was drawn stays under the frequency it belongs to.
    //
    // Only a pure translation can be slid. If the span itself changed, the
    // bins no longer mean the same width and the old rows cannot be placed;
    // those are dropped as before.
    const bool same_span = previous_span_hz > 0.0 && next_span_hz > 0.0 &&
        std::abs(previous_span_hz - next_span_hz) <
            0.001 * std::max(previous_span_hz, next_span_hz);
    const qsizetype bins = latest_bins_.size();
    const double bin_width_hz =
        bins > 1 ? next_span_hz / static_cast<double>(bins - 1) : 0.0;
    // Rows move in whole bins, but a click of the dial is almost never a
    // whole number of them, and the part that will not fit is not noise: it
    // is the same fraction, in the same direction, on every click. At 2 MS/s
    // over 16384 bins a 1 kHz step is a little over 8.19 bins, so dropping
    // the remainder leaves about 23 Hz of skew each time, all of it one way.
    // Tuning across a band accumulates kilohertz of it, and because the live
    // top row and the axis stay correct the history quietly slides out from
    // under its own frequency scale -- a signature parked where no signal is.
    //
    // So carry the remainder instead of discarding it, and spend it as soon
    // as it amounts to a whole bin. The carried value is in this item's own
    // bins, which is the grid the axis and the renderer use, so the history
    // stays aligned with what is drawn over it.
    const double wanted_shift_bins = bin_width_hz > 0.0
        ? shift_hz / bin_width_hz + row_shift_residual_bins_
        : 0.0;
    const auto shift_bins =
        static_cast<qsizetype>(std::llround(wanted_shift_bins));
    if (!same_span || bins <= 1 || std::abs(shift_bins) >= bins) {
      waterfall_rows_.clear();
      has_row_timestamp_ = false;
      conditioner_.reset();
      // Nothing survives to carry the remainder for.
      row_shift_residual_bins_ = 0.0;
      applied_row_shift_bins_ = 0;
    } else if (shift_bins != 0) {
      row_shift_residual_bins_ =
          wanted_shift_bins - static_cast<double>(shift_bins);
      applied_row_shift_bins_ = shift_bins;
      // Vacated bins carry no history and are filled with the row's own
      // quietest value, so newly exposed spectrum reads as empty rather than
      // as a copy of whatever was previously at that edge.
      for (QVector<float>& row : waterfall_rows_) {
        if (row.size() != bins) continue;
        const float empty = *std::min_element(row.cbegin(), row.cend());
        if (shift_bins > 0) {
          std::move(row.begin() + shift_bins, row.end(), row.begin());
          std::fill(row.end() - shift_bins, row.end(), empty);
        } else {
          std::move_backward(row.begin(), row.end() + shift_bins, row.end());
          std::fill(row.begin(), row.begin() - shift_bins, empty);
        }
      }
      // The conditioner's baseline is per-bin, so it slides by exactly the
      // bins the rows did. Resetting it instead discarded a calibration that
      // takes about a second to re-converge, and every retune therefore
      // painted a horizontal band across the waterfall while it settled --
      // tuning across a band produced a row of them.
      conditioner_.shiftBins(shift_bins);
    } else {
      // Too small to spend this time; it keeps accruing until it is not.
      row_shift_residual_bins_ = wanted_shift_bins;
      applied_row_shift_bins_ = 0;
    }
  }
  // Only a source change may move the view. Testing `!preserve_zoom` here
  // instead reset the view on every ordinary frame, because preservation is
  // predicated on a source change and is therefore false in the steady state:
  // a zoom survived only until the next frame arrived.
  if (!view_initialized_ || (source_changed && !preserve_zoom)) {
    const double frame_span_hz =
        frame.upper_frequency_hz - frame.lower_frequency_hz;
    // Open at the span the operator configured rather than at whatever the
    // hardware happened to deliver. A receiver asked for 2 MHz frequently
    // runs at the nearest rate it supports instead, and opening at 8 MHz puts
    // the entire CW segment inside a handful of pixels -- the operator then
    // has to zoom in by hand before the display says anything at all.
    if (preferred_span_hz_ > 0.0 && frame_span_hz > 0.0 &&
        preferred_span_hz_ < frame_span_hz) {
      const double center_hz =
          0.5 * (frame.lower_frequency_hz + frame.upper_frequency_hz);
      lower_frequency_hz_ = center_hz - preferred_span_hz_ * 0.5;
      upper_frequency_hz_ = center_hz + preferred_span_hz_ * 0.5;
    } else {
      lower_frequency_hz_ = frame.lower_frequency_hz;
      upper_frequency_hz_ = frame.upper_frequency_hz;
    }
    view_initialized_ = true;
    emit frequencyRangeChanged();
  } else if (preserve_zoom) {
    const double source_span_hz =
        source_upper_frequency_hz_ - source_lower_frequency_hz_;
    const double next_span_hz =
        std::clamp(previous_view_span_hz, 0.0, source_span_hz);
    const double retune_shift_hz =
        0.5 * (source_lower_frequency_hz_ + source_upper_frequency_hz_) -
        previous_source_center_hz;
    const double next_lower_hz = std::clamp(
        lower_frequency_hz_ + retune_shift_hz, source_lower_frequency_hz_,
        source_upper_frequency_hz_ - next_span_hz);
    lower_frequency_hz_ = next_lower_hz;
    upper_frequency_hz_ = next_lower_hz + next_span_hz;
    emit frequencyRangeChanged();
  }
  if (has_sequence_ && frame.sequence > last_sequence_ + 1) {
    dropped_rows_ += frame.sequence - last_sequence_ - 1;
    emit droppedRowsChanged();
  }
  last_sequence_ = frame.sequence;
  has_sequence_ = true;
  updateNoiseFloor(latest_bins_);
  if (automatic_range_) updateAutomaticRange(latest_bins_);

  const std::uint64_t row_interval =
      1'000'000'000ULL / static_cast<std::uint64_t>(waterfall_rate_);
  if (!has_row_timestamp_) {
    appendWaterfallRow(conditionedWaterfallRow(waterfall_bins));
    last_row_timestamp_ns_ = frame.timestamp_ns;
    has_row_timestamp_ = true;
  } else if (frame.timestamp_ns >= last_row_timestamp_ns_ + row_interval) {
    const std::uint64_t elapsed_intervals =
        (frame.timestamp_ns - last_row_timestamp_ns_) / row_interval;
    // A blank row says "nothing was received for this interval", which is
    // worth showing: a break in reception should read as a break, and padding
    // keeps the time axis honest against a real input stall.
    //
    // It must not say "the display could not keep up". Frames are refused to
    // bound memory, and painting those intervals black filled the waterfall
    // with stripes and made a continuous band of signal look interrupted --
    // reporting a fault in reception that did not happen. When the frame says
    // frames were dropped before it, the receiver was fine and the row clock
    // simply resynchronises.
    const std::uint64_t missing_intervals =
        frame.dropped_before > 0 ? 0 : elapsed_intervals - 1;
    const std::uint64_t retained_missing = std::min<std::uint64_t>(
        missing_intervals,
        static_cast<std::uint64_t>(waterfallRowCapacity() - 1));
    const QVector<float> blank = blankWaterfallRow(waterfall_bins.size());
    for (std::uint64_t i = 0; i < retained_missing; ++i) {
      appendWaterfallRow(blank);
    }
    appendWaterfallRow(conditionedWaterfallRow(waterfall_bins));
    last_row_timestamp_ns_ += elapsed_intervals * row_interval;
  }
  scheduleRender();
}

void SpectrumWaterfallItem::zoomAt(const double frequency_hz,
                                   const double factor) {
  if (!view_initialized_ || !std::isfinite(frequency_hz) ||
      !std::isfinite(factor) || factor <= 0.0 ||
      source_upper_frequency_hz_ <= source_lower_frequency_hz_) {
    return;
  }
  const double source_span =
      source_upper_frequency_hz_ - source_lower_frequency_hz_;
  const double current_span = upper_frequency_hz_ - lower_frequency_hz_;
  const double minimum_span = std::max(
      500.0, source_span / std::max(64.0, static_cast<double>(latest_bins_.size())) * 32.0);
  const double next_span =
      std::clamp(current_span * factor, minimum_span, source_span);
  const double anchor = std::clamp(
      (frequency_hz - lower_frequency_hz_) / std::max(1.0, current_span),
      0.0, 1.0);
  double next_lower = frequency_hz - anchor * next_span;
  next_lower = std::clamp(next_lower, source_lower_frequency_hz_,
                          source_upper_frequency_hz_ - next_span);
  const double next_upper = next_lower + next_span;
  if (qFuzzyCompare(lower_frequency_hz_, next_lower) &&
      qFuzzyCompare(upper_frequency_hz_, next_upper)) {
    return;
  }
  lower_frequency_hz_ = next_lower;
  upper_frequency_hz_ = next_upper;
  emit frequencyRangeChanged();
  update();
}

void SpectrumWaterfallItem::panBy(const double frequency_delta_hz) {
  if (!zoomed() || !std::isfinite(frequency_delta_hz)) return;
  const double span = upper_frequency_hz_ - lower_frequency_hz_;
  const double next_lower = std::clamp(
      lower_frequency_hz_ + frequency_delta_hz, source_lower_frequency_hz_,
      source_upper_frequency_hz_ - span);
  if (qFuzzyCompare(lower_frequency_hz_, next_lower)) return;
  lower_frequency_hz_ = next_lower;
  upper_frequency_hz_ = next_lower + span;
  emit frequencyRangeChanged();
  update();
}

void SpectrumWaterfallItem::resetZoom() {
  if (!view_initialized_) return;
  lower_frequency_hz_ = source_lower_frequency_hz_;
  upper_frequency_hz_ = source_upper_frequency_hz_;
  emit frequencyRangeChanged();
  update();
}

void SpectrumWaterfallItem::resetFrames() {
  latest_bins_.clear();
  waterfall_rows_.clear();
  last_row_timestamp_ns_ = 0;
  has_row_timestamp_ = false;
  last_sequence_ = 0;
  has_sequence_ = false;
  automatic_range_initialized_ = false;
  noise_floor_initialized_ = false;
  conditioner_.reset();
  row_shift_residual_bins_ = 0.0;
  applied_row_shift_bins_ = 0;
  estimated_noise_floor_db_ = -120.0;
  if (automatic_range_) {
    effective_lower_bound_db_ = -120.0;
    effective_upper_bound_db_ = -20.0;
    emit rangeChanged();
  }
  dropped_rows_ = 0;
  lower_frequency_hz_ = 0.0;
  upper_frequency_hz_ = 0.0;
  source_lower_frequency_hz_ = 0.0;
  source_upper_frequency_hz_ = 0.0;
  view_initialized_ = false;
  emit droppedRowsChanged();
  emit noiseFloorChanged();
  emit frequencyRangeChanged();
  update();
}

void SpectrumWaterfallItem::updateAutomaticRange(const QVector<float>& bins) {
  QVector<float> finite;
  finite.reserve(bins.size());
  for (const float bin : bins) {
    if (std::isfinite(bin)) finite.push_back(bin);
  }
  if (finite.isEmpty()) return;
  std::sort(finite.begin(), finite.end());
  const qsizetype high_index = static_cast<qsizetype>(
      (static_cast<quint64>(finite.size() - 1) * 99ULL) / 100ULL);
  // Keep the palette bottom just under the real noise, not far below it. The
  // estimate is now a median, which sits ~1.6 dB below the mean noise power,
  // so a 2 dB margin puts the mean floor ~3.6 dB up a 60 dB palette (t=0.06)
  // and its p99 tail (+6.6 dB on a single look) at t=0.17, both safely inside
  // the dark blue leg. The previous p20-minus-8 dB bottom sat 14.5 dB below
  // the mean, which spent 40% of the palette on noise and pushed that same
  // tail onto the blue-to-green breakpoint at t=0.35, so quiet bins flickered
  // green.
  // This bound is the trace's baseline and what the axis is labelled with, so
  // it needs room beneath the noise: put the floor two decibels under it and
  // the noise lies flat along the bottom of the plot, where its shape and its
  // distance from a signal cannot be read at all. Twelve decibels of headroom
  // puts the mean floor about a fifth of the way up and leaves the rest for
  // signals. The waterfall palette keeps the tighter bottom it needs, derived
  // from this one just below.
  double low = std::clamp(estimated_noise_floor_db_ - kTraceFloorHeadroomDb,
                          -200.0, 20.0);
  double high = std::max(static_cast<double>(finite[high_index]) + 3.0,
                         low + automatic_range_span_db_);
  high = std::clamp(high, -190.0, 50.0);
  if (high - low < automatic_range_span_db_) {
    low = std::max(-200.0, high - automatic_range_span_db_);
  }
  if (!automatic_range_initialized_) {
    effective_lower_bound_db_ = low;
    effective_upper_bound_db_ = high;
    automatic_range_initialized_ = true;
    emit rangeChanged();
    return;
  }
  constexpr double floor_smoothing = 0.04;
  const double ceiling_smoothing =
      high > effective_upper_bound_db_ ? 0.25 : 0.02;
  const double next_low =
      effective_lower_bound_db_ * (1.0 - floor_smoothing) +
      low * floor_smoothing;
  const double next_high =
      effective_upper_bound_db_ * (1.0 - ceiling_smoothing) +
      high * ceiling_smoothing;
  if (std::abs(next_low - effective_lower_bound_db_) > 0.02 ||
      std::abs(next_high - effective_upper_bound_db_) > 0.02) {
    effective_lower_bound_db_ = next_low;
    effective_upper_bound_db_ = next_high;
    emit rangeChanged();
  }
}

void SpectrumWaterfallItem::updateNoiseFloor(const QVector<float>& bins) {
  QVector<float> finite;
  finite.reserve(bins.size());
  for (const float bin : bins) {
    if (std::isfinite(bin)) finite.push_back(bin);
  }
  if (finite.isEmpty()) return;
  // Median, not the 20th percentile. Bin power is exponentially distributed
  // around the true noise power, so on a single look the p20 sits
  // 10*log10(-ln(0.8)) = -6.5 dB under the mean while the median sits only
  // 10*log10(ln 2) = -1.6 dB under it. The p20 offset also moves with the
  // averaging setting (about -2.9 dB after three looks) whereas the median
  // offset shrinks towards zero, so the palette floor no longer walks when
  // the operator changes averaging. A median still ignores carriers as long
  // as signals occupy under half the span, which any real band does.
  const qsizetype index = static_cast<qsizetype>(finite.size() / 2);
  std::nth_element(finite.begin(), finite.begin() + index, finite.end());
  const double observed = static_cast<double>(finite[index]);
  const double previous = estimated_noise_floor_db_;
  if (!noise_floor_initialized_) {
    estimated_noise_floor_db_ = observed;
    noise_floor_initialized_ = true;
  } else {
    // Follow an AGC-driven rise promptly so the waterfall gate does not flash
    // yellow, but release slowly enough that momentary quiet does not pump the
    // palette in the opposite direction.
    const double smoothing = observed > estimated_noise_floor_db_ ? 0.15 : 0.03;
    estimated_noise_floor_db_ +=
        smoothing * (observed - estimated_noise_floor_db_);
  }
  if (std::abs(previous - estimated_noise_floor_db_) > 0.02) {
    emit noiseFloorChanged();
  }
}

bool SpectrumWaterfallItem::directIqSource() const {
  auto* replay = qobject_cast<ReplayController*>(source_);
  return replay != nullptr && replay->sourceMode() == kDirectIqSourceMode;
}

void SpectrumWaterfallItem::suppressLocalOscillatorBin(
    QVector<float>& bins) const {
  // Nothing removes DC on the IQ path: the analyzer's mean subtraction is
  // gated on audio streams, so the receiver's LO leakage arrives as a
  // permanent spike in the centre bin, tens of dB above the band. Left in, it
  // pins the p99 that sets the palette ceiling and draws a carrier that is not
  // on the air. Only this display copy is repaired. Detection is fed straight
  // from the analyzer's unaveraged bins inside the capture worker and never
  // reads this array, so which signals are found does not change.
  constexpr qsizetype kMinimumIqBins = 64;
  // A Hann main lobe is four bins wide, so the leakage reaches centre +/- 2.
  constexpr qsizetype kHalfWidth = 2;
  // A complex spectrum carries one bin per transform point, always a power of
  // two, and puts DC at the exact middle; an audio spectrum is a half-band
  // slice. Require both signals so a channelized audio view can never lose
  // the bins at the middle of its passband.
  const qsizetype bin_count = bins.size();
  if (bin_count < kMinimumIqBins || (bin_count & (bin_count - 1)) != 0) return;
  if (!directIqSource()) return;
  const qsizetype center = bin_count / 2;
  const qsizetype begin = center - kHalfWidth;
  const qsizetype end = center + kHalfWidth;
  const double left = static_cast<double>(bins[begin - 1]);
  const double right = static_cast<double>(bins[end + 1]);
  if (!std::isfinite(left) || !std::isfinite(right)) return;
  const double steps = static_cast<double>(2 * kHalfWidth + 2);
  for (qsizetype index = begin; index <= end; ++index) {
    const double fraction =
        static_cast<double>(index - begin + 1) / steps;
    bins[index] = static_cast<float>(left + fraction * (right - left));
  }
}

QVector<float> SpectrumWaterfallItem::conditionedWaterfallRow(
    const QVector<float>& bins) {
  if (display_mode_ == 1) {
    const auto* replay = qobject_cast<ReplayController*>(source_);
    return cwSymbolRow(
        replay == nullptr ? QVariantList{} : replay->decoderChannels(),
        bins.size(), source_lower_frequency_hz_, source_upper_frequency_hz_,
        effective_lower_bound_db_, effective_upper_bound_db_);
  }
  if (!noise_floor_initialized_) return bins;
  const double bin_width_hz = bins.size() > 1
      ? (source_upper_frequency_hz_ - source_lower_frequency_hz_) /
            static_cast<double>(bins.size() - 1)
      : 1.0;
  return conditioner_.process(
      bins, noise_suppression_, noise_margin_db_,
      effective_lower_bound_db_, effective_upper_bound_db_, bin_width_hz,
      estimated_noise_floor_db_);
}

QVector<float> SpectrumWaterfallItem::blankWaterfallRow(
    const qsizetype width) const {
  const float level = static_cast<float>(
      noise_floor_initialized_ ? estimated_noise_floor_db_ - 18.0
                               : effective_lower_bound_db_);
  return QVector<float>(width, level);
}

void SpectrumWaterfallItem::appendWaterfallRow(QVector<float> row) {
  waterfall_rows_.push_front(std::move(row));
  while (waterfall_rows_.size() >
         static_cast<std::size_t>(waterfallRowCapacity())) {
    waterfall_rows_.pop_back();
  }
}

void SpectrumWaterfallItem::scheduleRender() {
  const qint64 minimum_interval = 1'000 / target_fps_;
  if (!render_clock_.isValid() || render_clock_.elapsed() >= minimum_interval) {
    render_clock_.restart();
    update();
  }
}

QSGNode* SpectrumWaterfallItem::updatePaintNode(
    QSGNode* old_node, UpdatePaintNodeData*) {
  auto* root = static_cast<DisplayNode*>(old_node);
  if (root == nullptr) root = new DisplayNode;

  const float width = static_cast<float>(this->width());
  const float height = static_cast<float>(this->height());
  const float spectrum_height = height * 0.36F;
  const float waterfall_top = spectrum_height + 8.0F;
  const float waterfall_height = std::max(0.0F, height - waterfall_top);

  qsizetype first_bin = 0;
  qsizetype last_bin = latest_bins_.isEmpty() ? -1 : latest_bins_.size() - 1;
  const double source_span =
      source_upper_frequency_hz_ - source_lower_frequency_hz_;
  if (last_bin > 0 && source_span > 0.0) {
    first_bin = std::clamp<qsizetype>(static_cast<qsizetype>(std::floor(
        (lower_frequency_hz_ - source_lower_frequency_hz_) / source_span *
        static_cast<double>(last_bin))), 0, last_bin);
    last_bin = std::clamp<qsizetype>(static_cast<qsizetype>(std::ceil(
        (upper_frequency_hz_ - source_lower_frequency_hz_) / source_span *
        static_cast<double>(latest_bins_.size() - 1))), first_bin,
        latest_bins_.size() - 1);
  }
  const qsizetype visible_bins = last_bin >= first_bin
      ? last_bin - first_bin + 1 : 0;
  const double span = std::max(1.0, effective_upper_bound_db_ -
                                       effective_lower_bound_db_);
  // The palette starts higher than the trace's baseline, so none of its range
  // is spent colouring noise. Derived from the same smoothed bound so the two
  // move together.
  const double waterfall_floor_db = automatic_range_
      ? std::min(effective_lower_bound_db_ + kWaterfallFloorOffsetDb,
                 effective_upper_bound_db_ - 1.0)
      : effective_lower_bound_db_;
  const double waterfall_span =
      std::max(1.0, effective_upper_bound_db_ - waterfall_floor_db);
  // One vertex and one raster column per pixel column, never per source bin.
  // Zooming in stops the reduction at one column per bin, so a narrow view is
  // still drawn at full resolution.
  const qsizetype column_count = visible_bins > 0
      ? std::clamp<qsizetype>(static_cast<qsizetype>(std::lround(width)), 1,
                              visible_bins)
      : 0;
  auto* spectrum_geometry = root->spectrum->geometry();
  spectrum_geometry->allocate(static_cast<int>(column_count));
  auto* vertices = spectrum_geometry->vertexDataAsPoint2D();
  for (qsizetype column = 0; column < column_count; ++column) {
    qsizetype bin_begin = 0;
    qsizetype bin_end = 0;
    columnBinRange(column, column_count, first_bin, visible_bins, bin_begin,
                   bin_end);
    const float x = column_count > 1
                        ? width * static_cast<float>(column) /
                              static_cast<float>(column_count - 1)
                        : 0.0F;
    const double normalized = std::clamp(
        (columnPeakDb(latest_bins_, bin_begin, bin_end) -
         effective_lower_bound_db_) / span,
        0.0, 1.0);
    vertices[column].set(x, spectrum_height *
                                static_cast<float>(1.0 - normalized));
  }
  root->spectrum->markDirty(QSGNode::DirtyGeometry);

  auto* grid_geometry = root->grid->geometry();
  const int grid_lines = show_grid_ ? 8 : 0;
  grid_geometry->allocate(grid_lines * 2);
  auto* grid_vertices = grid_geometry->vertexDataAsPoint2D();
  if (show_grid_) {
    int vertex = 0;
    for (int i = 1; i < 5; ++i) {
      const float x = width * static_cast<float>(i) / 5.0F;
      grid_vertices[vertex++].set(x, 0.0F);
      grid_vertices[vertex++].set(x, height);
    }
    for (int i = 1; i < 4; ++i) {
      const float y = spectrum_height * static_cast<float>(i) / 4.0F;
      grid_vertices[vertex++].set(0.0F, y);
      grid_vertices[vertex++].set(width, y);
    }
    grid_vertices[vertex++].set(0.0F, waterfall_top);
    grid_vertices[vertex].set(width, waterfall_top);
  }
  root->grid->markDirty(QSGNode::DirtyGeometry);

  if (column_count > 0 && window() != nullptr) {
    // Sized in columns, not bins: at 16'384 bins the per-frame raster was
    // ~39 MB and had to be minified ~11x on upload. One texel per pixel
    // column keeps it around 3 MB and lands close to 1:1 on screen.
    const int image_width = static_cast<int>(column_count);
    const int image_height = waterfallRowCapacity();
    QImage image(image_width, image_height, QImage::Format_RGB32);
    const QRgb blank_color = waterfallColor(0.0F);
    for (int y = 0; y < image_height; ++y) {
      auto* scanline = reinterpret_cast<QRgb*>(image.scanLine(y));
      if (y >= storedWaterfallRows()) {
        std::fill_n(scanline, image_width, blank_color);
        continue;
      }
      const auto& row = waterfall_rows_[static_cast<std::size_t>(y)];
      for (int x = 0; x < image_width; ++x) {
        qsizetype bin_begin = 0;
        qsizetype bin_end = 0;
        columnBinRange(x, column_count, first_bin, visible_bins, bin_begin,
                       bin_end);
        const float normalized = static_cast<float>(std::clamp(
            (columnPeakDb(row, bin_begin, bin_end) - waterfall_floor_db) /
                waterfall_span,
            0.0, 1.0));
        scanline[x] = waterfallColor(normalized);
      }
    }
    auto* texture = window()->createTextureFromImage(image);
    if (texture == nullptr) {
      if (root->waterfall != nullptr) root->waterfall->setRect(QRectF{});
      return root;
    }
    if (root->waterfall == nullptr) {
      root->waterfall = window()->createImageNode();
      if (root->waterfall == nullptr) {
        delete texture;
        return root;
      }
      root->waterfall->setTexture(texture);
      root->waterfall->setOwnsTexture(true);
      root->prependChildNode(root->waterfall);
    } else {
      // The image node owns and releases the previous render-thread texture.
      root->waterfall->setTexture(texture);
    }
    root->waterfall->setRect(0.0F, waterfall_top, width, waterfall_height);
    root->waterfall->setFiltering(
        display_mode_ == 1 ? QSGTexture::Nearest : QSGTexture::Linear);
  } else if (root->waterfall != nullptr) {
    root->waterfall->setRect(QRectF{});
  }
  return root;
}

}  // namespace cwassistant::desktop
