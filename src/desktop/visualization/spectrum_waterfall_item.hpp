#pragma once

#include <QElapsedTimer>
#include <QQuickItem>
#include <QVector>

#include <deque>

#include "spectrum_frame.hpp"
#include "waterfall_conditioner.hpp"

namespace cwassistant::desktop {

class SpectrumWaterfallItem : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY sourceChanged)
  Q_PROPERTY(double lowerBoundDb READ lowerBoundDb WRITE setLowerBoundDb NOTIFY displayChanged)
  Q_PROPERTY(double upperBoundDb READ upperBoundDb WRITE setUpperBoundDb NOTIFY displayChanged)
  Q_PROPERTY(bool automaticRange READ automaticRange WRITE setAutomaticRange NOTIFY displayChanged)
  Q_PROPERTY(double automaticRangeSpanDb READ automaticRangeSpanDb WRITE setAutomaticRangeSpanDb NOTIFY displayChanged)
  Q_PROPERTY(bool noiseSuppression READ noiseSuppression WRITE setNoiseSuppression NOTIFY displayChanged)
  Q_PROPERTY(double noiseMarginDb READ noiseMarginDb WRITE setNoiseMarginDb NOTIFY displayChanged)
  Q_PROPERTY(int targetFps READ targetFps WRITE setTargetFps NOTIFY displayChanged)
  Q_PROPERTY(int waterfallRate READ waterfallRate WRITE setWaterfallRate NOTIFY displayChanged)
  Q_PROPERTY(int waterfallTimeSpanSeconds READ waterfallTimeSpanSeconds WRITE setWaterfallTimeSpanSeconds NOTIFY displayChanged)
  Q_PROPERTY(int displayMode READ displayMode WRITE setDisplayMode NOTIFY displayChanged)
  Q_PROPERTY(int waterfallRowCapacity READ waterfallRowCapacity NOTIFY displayChanged)
  Q_PROPERTY(bool showGrid READ showGrid WRITE setShowGrid NOTIFY displayChanged)
  Q_PROPERTY(double effectiveLowerBoundDb READ effectiveLowerBoundDb NOTIFY rangeChanged)
  Q_PROPERTY(double effectiveUpperBoundDb READ effectiveUpperBoundDb NOTIFY rangeChanged)
  Q_PROPERTY(double lowerFrequencyHz READ lowerFrequencyHz NOTIFY frequencyRangeChanged)
  Q_PROPERTY(double upperFrequencyHz READ upperFrequencyHz NOTIFY frequencyRangeChanged)
  Q_PROPERTY(double sourceLowerFrequencyHz READ sourceLowerFrequencyHz NOTIFY frequencyRangeChanged)
  Q_PROPERTY(double sourceUpperFrequencyHz READ sourceUpperFrequencyHz NOTIFY frequencyRangeChanged)
  Q_PROPERTY(bool zoomed READ zoomed NOTIFY frequencyRangeChanged)
  // The span the operator asked the receiver for, in hertz; 0 means "no
  // preference, show whatever arrives". A device often cannot deliver the
  // requested rate and runs faster, so a 2 MHz selection can arrive as an
  // 8 MHz frame and the whole CW segment collapses into a few pixels. This is
  // the width the first view is opened at, centred on the capture.
  Q_PROPERTY(double preferredSpanHz READ preferredSpanHz WRITE
                 setPreferredSpanHz NOTIFY displayChanged)
  Q_PROPERTY(qulonglong droppedRows READ droppedRows NOTIFY droppedRowsChanged)
  Q_PROPERTY(double estimatedNoiseFloorDb READ estimatedNoiseFloorDb NOTIFY noiseFloorChanged)

 public:
  explicit SpectrumWaterfallItem(QQuickItem* parent = nullptr);

  [[nodiscard]] QObject* source() const noexcept;
  void setSource(QObject* source);
  [[nodiscard]] double lowerBoundDb() const noexcept;
  void setLowerBoundDb(double value);
  [[nodiscard]] double upperBoundDb() const noexcept;
  void setUpperBoundDb(double value);
  [[nodiscard]] bool automaticRange() const noexcept;
  void setAutomaticRange(bool value);
  [[nodiscard]] double automaticRangeSpanDb() const noexcept;
  void setAutomaticRangeSpanDb(double value);
  [[nodiscard]] bool noiseSuppression() const noexcept;
  void setNoiseSuppression(bool value);
  [[nodiscard]] double noiseMarginDb() const noexcept;
  void setNoiseMarginDb(double value);
  [[nodiscard]] int targetFps() const noexcept;
  void setTargetFps(int value);
  [[nodiscard]] int waterfallRate() const noexcept;
  void setWaterfallRate(int value);
  [[nodiscard]] int waterfallTimeSpanSeconds() const noexcept;
  void setWaterfallTimeSpanSeconds(int value);
  [[nodiscard]] int displayMode() const noexcept;
  void setDisplayMode(int value);
  [[nodiscard]] int waterfallRowCapacity() const noexcept;
  [[nodiscard]] int storedWaterfallRows() const noexcept;
  [[nodiscard]] bool showGrid() const noexcept;
  void setShowGrid(bool value);
  [[nodiscard]] double effectiveLowerBoundDb() const noexcept;
  [[nodiscard]] double effectiveUpperBoundDb() const noexcept;
  [[nodiscard]] double lowerFrequencyHz() const noexcept;
  [[nodiscard]] double upperFrequencyHz() const noexcept;
  [[nodiscard]] double sourceLowerFrequencyHz() const noexcept;
  [[nodiscard]] double sourceUpperFrequencyHz() const noexcept;
  [[nodiscard]] bool zoomed() const noexcept;
  [[nodiscard]] double preferredSpanHz() const noexcept;
  void setPreferredSpanHz(double span_hz);
  [[nodiscard]] qulonglong droppedRows() const noexcept;
  // How many waterfall rows are currently retained. Exposed so that the
  // history's survival across a retune can be asserted: a receiver moving in
  // frequency slides its history rather than discarding it, and nothing else
  // observable distinguishes those two outcomes.
  [[nodiscard]] int waterfallRowCount() const noexcept;
  // How many bins the retained rows were slid by on the last source change.
  // Exposed because nothing else observable says it: the history is not
  // readable from outside, and the difference between a slide that carries
  // its rounding remainder and one that throws it away is only ever a bin
  // here and there -- which is exactly the skew that accumulates into
  // kilohertz across a band.
  [[nodiscard]] qsizetype appliedRowShiftBins() const noexcept;
  [[nodiscard]] double estimatedNoiseFloorDb() const noexcept;

 public slots:
  void acceptFrame(const cwassistant::desktop::SpectrumFrame& frame);
  void zoomAt(double frequency_hz, double factor);
  void panBy(double frequency_delta_hz);
  void resetZoom();

 signals:
  void sourceChanged();
  void displayChanged();
  void rangeChanged();
  void frequencyRangeChanged();
  void droppedRowsChanged();
  void noiseFloorChanged();

 protected:
  QSGNode* updatePaintNode(QSGNode* old_node,
                           UpdatePaintNodeData*) override;

 private slots:
  void resetFrames();

 private:
  void updateAutomaticRange(const QVector<float>& bins);
  void updateNoiseFloor(const QVector<float>& bins);
  [[nodiscard]] bool directIqSource() const;
  // Repairs the display copy of a complex-IQ spectrum in place. The decoder is
  // fed elsewhere, from the analyzer directly, and is unaffected.
  void suppressLocalOscillatorBin(QVector<float>& bins) const;
  [[nodiscard]] QVector<float> conditionedWaterfallRow(
      const QVector<float>& bins);
  [[nodiscard]] QVector<float> blankWaterfallRow(qsizetype width) const;
  void appendWaterfallRow(QVector<float> row);
  void scheduleRender();

  QObject* source_{nullptr};
  QVector<float> latest_bins_;
  std::deque<QVector<float>> waterfall_rows_;
  double lower_bound_db_{-120.0};
  double upper_bound_db_{-20.0};
  double effective_lower_bound_db_{-120.0};
  double effective_upper_bound_db_{-20.0};
  double automatic_range_span_db_{60.0};
  double noise_margin_db_{6.0};
  double estimated_noise_floor_db_{-120.0};
  double lower_frequency_hz_{0.0};
  double upper_frequency_hz_{0.0};
  double source_lower_frequency_hz_{0.0};
  double source_upper_frequency_hz_{0.0};
  bool view_initialized_{false};
  double preferred_span_hz_{0.0};
  bool automatic_range_{true};
  bool automatic_range_initialized_{false};
  bool noise_suppression_{true};
  bool noise_floor_initialized_{false};
  bool show_grid_{true};
  int target_fps_{60};
  int waterfall_rate_{60};
  int waterfall_time_span_seconds_{10};
  int display_mode_{0};
  std::uint64_t last_row_timestamp_ns_{0};
  bool has_row_timestamp_{false};
  std::uint64_t last_sequence_{0};
  bool has_sequence_{false};
  qulonglong dropped_rows_{0};
  QElapsedTimer render_clock_;
  WaterfallConditioner conditioner_;
  // The part of the last retune's slide that would not fit in whole bins,
  // kept so the next one can spend it. See acceptFrame().
  double row_shift_residual_bins_{0.0};
  qsizetype applied_row_shift_bins_{0};
};

}  // namespace cwassistant::desktop
