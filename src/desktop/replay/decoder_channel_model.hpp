#pragma once

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

// Aggregate pre-verification pipeline diagnostics (candidate/morse-likely
// counts and rejection-reason tally) for operator troubleshooting. Never
// includes automatically discovered candidate identity, frequency, or overlay
// data. An explicit operator-selected manual probe is the sole exception in
// the channel model; it remains neutral and redacts text/callsign evidence
// until the ordinary verification gates pass.
[[nodiscard]] QVariantMap verificationDiagnosticsModel(
    const cwassistant::core::CwVerificationDiagnostics& diagnostics);

}  // namespace cwassistant::desktop
