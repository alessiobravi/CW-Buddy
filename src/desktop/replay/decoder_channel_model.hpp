#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "cwassistant/core/cw_channel_bank.hpp"

namespace cwassistant::desktop {

// The color every track is drawn in, generated for any color index instead of
// being read out of a table.
//
// It replaced 24 hand-written hex strings, and those 24 strings were setting
// the decoder's capacity: the color-lease array was sized to the palette and
// the track cap was sized to the leases, so how many stations could be
// followed was decided by how many colors somebody had felt like typing. A
// generator removes the palette from that chain entirely -- there is a color
// for index 200 as readily as for index 3 -- which is what lets
// `CwChannelBankConfig::maximum_tracks` be chosen from a processor budget.
//
// Three properties are load-bearing, and each one rules out an easier answer.
//
// Separated. Colors exist so an operator can tell one stream from the one next
// to it, so what has to differ is neighbours, and a track takes whichever
// index is free rather than the next one up. Stepping the hue circle evenly
// fails as soon as the number of tracks is not known in advance, and drawing
// RGB from a generator fails outright: independent draws collide, and two
// near-identical colors landing on two adjacent carriers is precisely the case
// this is for. Successive indices are therefore placed a golden angle apart,
// which is the standard low-discrepancy rotation: any prefix of the sequence
// is close to evenly spread, without knowing its length.
//
// Legible on a dark waterfall. These are drawn over a near-black spectrum, so
// the generator works in CIE L*a*b* rather than HSL -- in HSL a fixed
// lightness makes yellows glare and blues vanish, because HSL's lightness is
// not a perceptual quantity. Lightness and chroma are held inside the band the
// 24 replaced colors occupied, and any color the sRGB gamut cannot reach at
// that lightness loses chroma until it can, never lightness.
//
// Stable for the life of a run. Everything is derived from one seed drawn at
// construction, so index N gives the same color every time it is asked. That
// is not an aesthetic preference: `color_identity_retention_seconds` holds a
// color against a frequency for five minutes so that a station heard again is
// recognisable, and a color that moved under a live track would break exactly
// the identification the lease exists to provide, as well as making one track
// look like a new station on every redraw. Across runs there is no such
// requirement -- no color is persisted -- so the seed is drawn afresh at each
// launch and the whole palette looks different, which is what was asked for.
class ChannelColorPalette {
 public:
  explicit ChannelColorPalette(std::uint64_t seed) noexcept;

  // `#rrggbb`, as the QML expects. Defined for every `std::uint8_t` color
  // index the core can hand out.
  [[nodiscard]] QString color(std::uint32_t index) const;

  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  // Four lightness steps and three chroma steps, so one (lightness, chroma)
  // pair recurs every twelve indices. Hue alone stops carrying beyond roughly
  // twenty tracks, and the periods are not free choices -- see the generator
  // for which index distances the golden angle brings close together and why
  // three and four miss all of them.
  static constexpr std::size_t kLightnessTiers = 4;
  static constexpr std::size_t kChromaTiers = 3;

  std::uint64_t seed_{0};
  double hue_offset_degrees_{0.0};
  std::array<double, kLightnessTiers> lightness_{};
  std::array<double, kChromaTiers> chroma_{};
};

// The palette this process draws with, seeded once on first use. Held here
// rather than passed down from the caller because it has to be the same object
// for the whole run: two palettes would give one track two colors.
[[nodiscard]] const ChannelColorPalette& sessionChannelColorPalette();

enum class LocalDecoderPresentationState {
  Unavailable,
  Disabled,
  Loading,
  Ready,
  Error,
};

// Presentation-only output from an optional local decoder. Stable text must
// be append-only for a channel ID. It is deliberately kept outside the core
// verification, callsign, and publication evidence paths.
struct LocalDecoderChannelPresentation {
  std::uint64_t channel_id{0};
  LocalDecoderPresentationState state{
      LocalDecoderPresentationState::Unavailable};
  QString stable_text;
  QString status;
};

[[nodiscard]] QVariantList decoderChannelModel(
    std::span<const cwassistant::core::CwChannelSnapshot> channels,
    std::span<const LocalDecoderChannelPresentation> local_decoder = {},
    const ChannelColorPalette& palette = sessionChannelColorPalette());

// The keys the spectrum marker delegate reads, in the order it reads them,
// and no others. Published here so a test can hold the delegate to them: a
// key the delegate reads and this list omits arrives in QML as `undefined`,
// which is a marker drawn wrong rather than a build that fails.
[[nodiscard]] std::span<const char* const> decoderChannelMarkerKeys() noexcept;

// One published channel reduced to what a spectrum marker is drawn from.
//
// A full channel map carries forty-two keys, and most of them are the
// transcript and its derivatives: the raw, refined, provisional and
// contextual text, the acoustic alternatives, the per-character evidence, the
// completed transmissions and the sender cadences. A marker draws none of
// that. It also leaves out the continuously moving scalars the card shows --
// signal-to-noise, speed and the confidences -- because a marker does not
// display them either.
//
// That exclusion is the whole reason an incremental marker model can stay
// quiet. The transcript and the levels are what move on nearly every publish;
// a marker row holds a frequency, a colour, three booleans and a callsign, so
// a verified stream that is not being retuned and has decoded no new callsign
// produces a row equal to the one before it, and the model reports nothing.
//
// Redaction is inherited rather than re-decided. The fields that could name a
// station -- callsign, the advisory suggestion and its raw span -- are already
// empty on an unverified track, and its colour is already the neutral grey,
// because decoderChannelModel() gates them on `expose_verified_content` and
// the presentation cache derives a suggestion only from evidence that gating
// leaves behind. Copying those values forward cannot un-redact them, and a
// second gate here would be a second place for the rule to drift.
[[nodiscard]] QVariantMap decoderChannelMarkerRow(const QVariantMap& channel);

// Keeps the QML spectrum-marker delegates alive while the streams behind them
// change.
//
// This is DecoderSessionListModel's problem one panel over, and it was left
// unsolved for longer. The marker Repeater's model was the plain
// `decoderChannels` QVariantList, reassigned whole on every decoder publish,
// so every marker -- three Rectangles, a rotated plated Label and a
// SequentialAnimation each -- was destroyed and rebuilt up to twenty-five
// times a second, for as many as sixty-four tracked signals. Reassignment
// also forced the conversion of all sixty-four forty-two-key channel maps
// into JavaScript on every publish whether or not anything had moved.
//
// Three things follow from rebuilding a delegate rather than updating it, and
// only the first is a matter of cost. The pulse on a "CALLING YOU" marker
// restarts from the top each time, so the one alert the operator must not
// miss flickers instead of breathing. The 90 ms hover magnification restarts
// with it. And a delegate destroyed between a pointer press and its release
// swallows the gesture, which is the fault this tree has already been bitten
// by three times on the decoder cards.
//
// So the rows are reconciled instead:
//
//  - Rows whose channel id has gone are removed, one row at a time.
//  - Channel ids that are new are inserted at their final position.
//  - Every surviving row is compared with its replacement and only a row that
//    actually differs is reported changed.
//  - A reorder that is neither of the first two -- the survivors no longer
//    appearing in their old relative order -- falls back to a full reset,
//    which is what every publish used to do.
//
// The counters exist for the same reason DecoderChannelPresentationCache has
// one: a publish that changed nothing must not move them, and asserting that
// is the only way to state the claim without timing anything.
class DecoderChannelMarkerModel final : public QAbstractListModel {
 public:
  explicit DecoderChannelMarkerModel(QObject* parent = nullptr);
  [[nodiscard]] int rowCount(
      const QModelIndex& parent = QModelIndex{}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index,
                              int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  // `channels` is the full annotated channel list, exactly as
  // ReplayController publishes it; the projection to marker rows happens
  // here so there is one statement of what a marker is drawn from.
  void replace(const QVariantList& channels);

  [[nodiscard]] qulonglong resets() const noexcept { return resets_; }
  [[nodiscard]] qulonglong rowChanges() const noexcept { return row_changes_; }
  [[nodiscard]] qulonglong insertions() const noexcept { return insertions_; }
  [[nodiscard]] qulonglong removals() const noexcept { return removals_; }

 private:
  static constexpr int kModelDataRole = Qt::UserRole + 1;
  QVariantList rows_;
  QList<qulonglong> ids_;
  qulonglong resets_{0};
  qulonglong row_changes_{0};
  qulonglong insertions_{0};
  qulonglong removals_{0};
};

// Aggregate pre-verification pipeline diagnostics (candidate/morse-likely
// counts and rejection-reason tally) for operator troubleshooting. Never
// includes automatically discovered candidate identity, frequency, or overlay
// data. An explicit operator-selected manual probe is the sole exception in
// the channel model; it remains neutral and redacts text/callsign evidence
// until the ordinary verification gates pass.
[[nodiscard]] QVariantMap verificationDiagnosticsModel(
    const cwassistant::core::CwVerificationDiagnostics& diagnostics);

}  // namespace cwassistant::desktop
