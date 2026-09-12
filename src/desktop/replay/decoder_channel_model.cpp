#include "decoder_channel_model.hpp"

#include <QLatin1Char>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <random>
#include <utility>

namespace cwassistant::desktop {
namespace {

// The color a track wears before it has been verified as Morse. Neutral on
// purpose: an unverified track is a carrier the decoder has not vouched for,
// and giving it an identity color would present it as a station.
constexpr const char* kUnverifiedChannelColor = "#8d9aaa";

// 360 degrees divided by the square of the golden ratio. Successive multiples
// of it fill the hue circle without ever repeating, and every prefix of the
// sequence is close to evenly spread -- which is the property needed here,
// because how many tracks will be drawn is not known when index 0 is chosen.
constexpr double kGoldenAngleDegrees = 137.507'764'050'037'85;

// The lightness and chroma the generated colors are allowed to take, in CIE
// L*a*b*. Measured from the 24 hand-written colors these replace, which
// occupied L* 55.6 to 94.1 (mean 71.8) and chroma 6.2 to 68.8 (mean 43.0):
// mid-lightness, moderately saturated pastels that read against a near-black
// waterfall without glaring. The ladders sit inside that range rather than
// spanning it, because its extremes were two greys and a near-white that were
// hard to tell apart from each other and from the unverified color above.
//
// The two ladder lengths are chosen against the golden angle, not for looks.
// Sorting index distances by how close the rotation brings their hues, the
// worst offenders inside a 128-color run are distances 89 (1.8 degrees apart),
// 55 (2.9), 34 (4.7), 110 (5.9), 123 (6.6), 21 (7.7), 68 (9.5) and 76 (10.6).
// A pair only shares both tiers when its distance is a multiple of 12, and no
// distance in that list is; the first multiple of 12 that collides in hue is
// 144, past the largest run the bank can produce. Three tiers alone would fail
// at distance 21 and five at distance 55, and both were measured doing exactly
// that.
constexpr std::array<double, 4> kLightnessLadder{62.0, 70.0, 76.0, 82.0};
constexpr std::array<double, 3> kChromaLadder{34.0, 47.0, 60.0};

// SplitMix64. Hand-rolled rather than taken from <random> because the tests
// pin a seed and compare colors, and the standard distributions are not
// specified to produce the same numbers on two implementations -- so a palette
// built through them would be reproducible on this machine and not on the
// Linux and Windows builders.
[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept {
  state += 0x9e37'79b9'7f4a'7c15ULL;
  std::uint64_t value = state;
  value = (value ^ (value >> 30)) * 0xbf58'476d'1ce4'e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31);
}

[[nodiscard]] double nextUnitInterval(std::uint64_t& state) noexcept {
  // 53 bits is the whole mantissa, so every representable value in [0, 1) is
  // reachable and the result does not depend on the platform's long double.
  return static_cast<double>(nextRandom(state) >> 11) * 0x1.0p-53;
}

template <std::size_t Count>
void shuffle(std::array<double, Count>& ladder, std::uint64_t& state) noexcept {
  for (std::size_t index = Count - 1; index > 0; --index) {
    const auto target =
        static_cast<std::size_t>(nextRandom(state) % (index + 1));
    std::swap(ladder[index], ladder[target]);
  }
}

struct LinearRgb {
  double red{0.0};
  double green{0.0};
  double blue{0.0};
};

// CIE L*a*b* (D65) to linear sRGB. The inverse companding and the matrix are
// the standard ones; they are written out rather than pulled from QColor
// because QColor has no L*a*b* entry point.
[[nodiscard]] LinearRgb labToLinearRgb(const double lightness, const double a,
                                       const double b) noexcept {
  const double fy = (lightness + 16.0) / 116.0;
  const double fx = fy + a / 500.0;
  const double fz = fy - b / 200.0;
  const auto expand = [](const double value) {
    const double cube = value * value * value;
    return cube > 216.0 / 24'389.0 ? cube
                                   : (108.0 / 841.0) * (value - 4.0 / 29.0);
  };
  const double x = expand(fx) * 0.950'47;
  const double y = expand(fy);
  const double z = expand(fz) * 1.088'83;
  return {3.240'4542 * x - 1.537'1385 * y - 0.498'5314 * z,
          -0.969'2660 * x + 1.876'0108 * y + 0.041'5560 * z,
          0.055'6434 * x - 0.204'0259 * y + 1.057'2252 * z};
}

[[nodiscard]] bool withinGamut(const LinearRgb& color) noexcept {
  // A rounding tolerance, not a slack: a component landing a ten-thousandth
  // outside is the same color once it is quantized to eight bits.
  constexpr double tolerance = 0.000'5;
  return std::min({color.red, color.green, color.blue}) >= -tolerance &&
         std::max({color.red, color.green, color.blue}) <= 1.0 + tolerance;
}

[[nodiscard]] int encodeChannel(const double linear) noexcept {
  const double clamped = std::clamp(linear, 0.0, 1.0);
  const double encoded = clamped <= 0.003'1308
                             ? 12.92 * clamped
                             : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
  return static_cast<int>(std::lround(encoded * 255.0));
}

QString localDecoderStateName(const LocalDecoderPresentationState state) {
  switch (state) {
    case LocalDecoderPresentationState::Unavailable:
      return QStringLiteral("unavailable");
    case LocalDecoderPresentationState::Disabled:
      return QStringLiteral("disabled");
    case LocalDecoderPresentationState::Loading:
      return QStringLiteral("loading");
    case LocalDecoderPresentationState::Ready:
      return QStringLiteral("ready");
    case LocalDecoderPresentationState::Error:
      return QStringLiteral("error");
  }
  return QStringLiteral("unavailable");
}

QString localDecoderDefaultStatus(const LocalDecoderPresentationState state) {
  switch (state) {
    case LocalDecoderPresentationState::Unavailable:
      return QStringLiteral("Unavailable in this build");
    case LocalDecoderPresentationState::Disabled:
      return QStringLiteral("Disabled");
    case LocalDecoderPresentationState::Loading:
      return QStringLiteral("Loading local decoder");
    case LocalDecoderPresentationState::Ready:
      return QStringLiteral("Listening for stable text");
    case LocalDecoderPresentationState::Error:
      return QStringLiteral("Local decoder error");
  }
  return QStringLiteral("Unavailable in this build");
}

}  // namespace

ChannelColorPalette::ChannelColorPalette(const std::uint64_t seed) noexcept
    : seed_(seed), lightness_(kLightnessLadder), chroma_(kChromaLadder) {
  std::uint64_t state = seed;
  // Randomize where on the hue circle the sequence starts, and which tier each
  // residue class gets, and nothing else. Those two draws are what make each
  // launch look different; the golden-angle step between successive indices is
  // never randomized, because it is the whole reason no two indices collide.
  hue_offset_degrees_ = nextUnitInterval(state) * 360.0;
  shuffle(lightness_, state);
  shuffle(chroma_, state);
}

QString ChannelColorPalette::color(const std::uint32_t index) const {
  const double hue = std::fmod(hue_offset_degrees_ +
                                   static_cast<double>(index) *
                                       kGoldenAngleDegrees,
                               360.0);
  const double lightness = lightness_[index % kLightnessTiers];
  const double radians = hue * std::numbers::pi / 180.0;
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);

  // Walk the chroma down until the color exists in sRGB. Lightness is never
  // touched: it is what keeps the marker readable over the waterfall, while
  // chroma is only how far the hue is pushed, and a yellow that has to give up
  // a third of its chroma at L* 82 is still plainly a yellow. Twenty-four
  // steps of six percent reach a fourteenth of the starting chroma, which is
  // inside the gamut for every hue at every lightness in the ladder.
  double chroma = chroma_[index % kChromaTiers];
  LinearRgb color = labToLinearRgb(lightness, chroma * cosine, chroma * sine);
  for (int attempt = 0; attempt < 24 && !withinGamut(color); ++attempt) {
    chroma *= 0.94;
    color = labToLinearRgb(lightness, chroma * cosine, chroma * sine);
  }

  return QStringLiteral("#%1%2%3")
      .arg(encodeChannel(color.red), 2, 16, QLatin1Char('0'))
      .arg(encodeChannel(color.green), 2, 16, QLatin1Char('0'))
      .arg(encodeChannel(color.blue), 2, 16, QLatin1Char('0'));
}

const ChannelColorPalette& sessionChannelColorPalette() {
  // Two independent sources, because std::random_device is allowed to be a
  // fixed sequence -- it is on at least one toolchain this project builds
  // with -- and a palette that was "random" but identical on every launch
  // would silently be the fixed table again.
  static const ChannelColorPalette palette{[] {
    std::random_device device;
    const auto drawn = (static_cast<std::uint64_t>(device()) << 32) ^
                       static_cast<std::uint64_t>(device());
    const auto clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t state = drawn ^ (clock * 0x9e37'79b9'7f4a'7c15ULL);
    return nextRandom(state);
  }()};
  return palette;
}

namespace {

// Rounds a continuously varying measurement to what an operator can read.
//
// The session list compares each row with the one it already holds and only
// tells the view about rows that differ. Signal-to-noise, speed and the
// confidences move a little on every single update, so every row differed
// every time, every row was reported changed, and the view re-evaluated an
// entire decoded card -- transcript text layout included -- for each of them.
// With several signals decoding that is hundreds of full card rebuilds a
// second on the one thread that draws, which is felt as the application
// becoming jerky while the processor is plainly not busy.
//
// A tenth of a decibel or of a word per minute is already below what the card
// displays, so rounding costs the operator nothing and lets a row that has not
// meaningfully changed compare equal and stay quiet.
[[nodiscard]] double readable(const double value, const double step = 0.1) {
  return std::isfinite(value) ? std::round(value / step) * step : value;
}

}  // namespace

QVariantList decoderChannelModel(
    const std::span<const cwassistant::core::CwChannelSnapshot> channels,
    const std::span<const LocalDecoderChannelPresentation> local_decoder,
    const ChannelColorPalette& palette) {
  QVariantList model;
  model.reserve(static_cast<qsizetype>(channels.size()));
  for (const auto& channel : channels) {
    QVariantMap item;
    const bool expose_verified_content = channel.verified_cw;
    item.insert(QStringLiteral("id"),
                QVariant::fromValue<qulonglong>(channel.id));
    item.insert(QStringLiteral("frequencyHz"), readable(channel.frequency_hz));
    item.insert(QStringLiteral("presentationFrequencyHz"),
                readable(channel.presentation_frequency_hz));
    item.insert(QStringLiteral("driftHzPerSecond"),
                readable(channel.drift_hz_per_second));
    item.insert(QStringLiteral("filterWidthHz"),
                readable(channel.filter_width_hz, 1.0));
    item.insert(QStringLiteral("snrDb"), readable(channel.snr_db));
    item.insert(QStringLiteral("wpm"), readable(channel.wpm));
    item.insert(QStringLiteral("confidence"),
                readable(channel.confidence, 0.01));
    item.insert(QStringLiteral("keyProbability"),
                readable(channel.key_down_probability, 0.01));
    item.insert(QStringLiteral("keyDown"), channel.key_down);
    item.insert(QStringLiteral("active"), channel.active);
    item.insert(QStringLiteral("verifiedCw"), channel.verified_cw);
    item.insert(QStringLiteral("operatorSelected"),
                channel.operator_selected);
    item.insert(QStringLiteral("verificationState"), QString::fromLatin1(
        cwassistant::core::cwTrackStateName(channel.verification_state)));
    item.insert(QStringLiteral("verificationReason"), QString::fromLatin1(
        cwassistant::core::cwVerificationReasonName(
            channel.verification_reason)));
    item.insert(QStringLiteral("verificationConfidence"),
                readable(channel.verification_confidence, 0.01));
    item.insert(QStringLiteral("verificationCadenceQuality"),
                readable(channel.verification_cadence_quality, 0.01));
    item.insert(QStringLiteral("verificationTimingQuality"),
                readable(channel.verification_timing_quality, 0.01));
    item.insert(QStringLiteral("verificationCharacterConfidence"),
                readable(channel.verification_character_confidence, 0.01));
    item.insert(QStringLiteral("cadenceQuality"),
                readable(channel.cadence_quality, 0.01));
    item.insert(QStringLiteral("meanCharacterConfidence"),
                readable(channel.mean_character_confidence, 0.01));
    item.insert(QStringLiteral("narrowbandCoherence"),
                readable(channel.narrowband_coherence, 0.01));
    item.insert(QStringLiteral("keyTransitions"),
                QVariant::fromValue<qulonglong>(channel.key_transitions));
    QVariantList character_evidence;
    character_evidence.reserve(
        static_cast<qsizetype>(channel.characters.size()));
    for (const auto& character : channel.characters) {
      if (!expose_verified_content) break;
      QVariantMap evidence;
      evidence.insert(QStringLiteral("symbol"),
                      QString::fromStdString(character.symbol));
      evidence.insert(QStringLiteral("confidence"),
                      readable(character.confidence, 0.01));
      evidence.insert(QStringLiteral("timingQuality"),
                      readable(character.timing_quality, 0.01));
      evidence.insert(QStringLiteral("known"), character.known);
      character_evidence.push_back(evidence);
    }
    item.insert(QStringLiteral("characterEvidence"), character_evidence);
    item.insert(QStringLiteral("text"),
                expose_verified_content
                    ? QString::fromStdString(channel.text) : QString{});
    item.insert(QStringLiteral("refinedText"),
                expose_verified_content
                    ? QString::fromStdString(channel.refined_text)
                    : QString{});
    QVariantList acoustic_alternatives;
    if (expose_verified_content) {
      acoustic_alternatives.reserve(static_cast<qsizetype>(
          channel.acoustic_alternatives.size()));
      for (const auto& alternative : channel.acoustic_alternatives) {
        QVariantMap candidate;
        candidate.insert(QStringLiteral("text"),
                         QString::fromStdString(alternative.text));
        candidate.insert(QStringLiteral("elements"),
                         QString::fromStdString(
                             alternative.provisional_elements));
        candidate.insert(QStringLiteral("wpm"), readable(alternative.wpm));
        candidate.insert(QStringLiteral("cost"),
                         readable(alternative.acoustic_cost));
        candidate.insert(QStringLiteral("confidence"),
                         readable(alternative.evidence_confidence, 0.01));
        candidate.insert(QStringLiteral("firstObservationId"),
                         QVariant::fromValue<qulonglong>(
                             alternative.first_observation_id));
        candidate.insert(QStringLiteral("lastObservationId"),
                         QVariant::fromValue<qulonglong>(
                             alternative.last_observation_id));
        acoustic_alternatives.push_back(candidate);
      }
    }
    item.insert(QStringLiteral("acousticAlternatives"),
                acoustic_alternatives);
    item.insert(QStringLiteral("provisionalText"),
                expose_verified_content
                    ? QString::fromStdString(channel.provisional_text)
                    : QString{});
    item.insert(QStringLiteral("elements"),
                expose_verified_content
                    ? QString::fromStdString(channel.pending_elements)
                    : QString{});
    QVariantList transmissions;
    QVariantList sender_cadences;
    if (expose_verified_content) {
      transmissions.reserve(static_cast<qsizetype>(
          channel.transmissions.size()));
      for (const auto& transmission : channel.transmissions) {
        QVariantMap turn;
        turn.insert(QStringLiteral("sequence"),
                    QVariant::fromValue<qulonglong>(transmission.sequence));
        turn.insert(QStringLiteral("text"),
                    QString::fromStdString(transmission.text));
        turn.insert(QStringLiteral("sender"),
                    QString::fromStdString(transmission.sender_callsign));
        turn.insert(QStringLiteral("wpm"), readable(transmission.wpm));
        turn.insert(QStringLiteral("cadenceConfidence"),
                    readable(transmission.cadence_confidence, 0.01));
        transmissions.push_back(turn);
      }
      sender_cadences.reserve(static_cast<qsizetype>(
          channel.sender_cadences.size()));
      for (const auto& cadence : channel.sender_cadences) {
        QVariantMap item_cadence;
        item_cadence.insert(QStringLiteral("callsign"),
                            QString::fromStdString(cadence.callsign));
        item_cadence.insert(QStringLiteral("wpm"), readable(cadence.wpm));
        item_cadence.insert(QStringLiteral("confidence"),
                            readable(cadence.confidence, 0.01));
        item_cadence.insert(QStringLiteral("turns"), cadence.observed_turns);
        sender_cadences.push_back(item_cadence);
      }
    }
    item.insert(QStringLiteral("transmissions"), transmissions);
    item.insert(QStringLiteral("senderCadences"), sender_cadences);
    item.insert(QStringLiteral("activeTransmissionSequence"),
                QVariant::fromValue<qulonglong>(
                    channel.active_transmission_sequence));
    item.insert(QStringLiteral("currentSenderCallsign"),
                expose_verified_content
                    ? QString::fromStdString(channel.current_sender_callsign)
                    : QString{});
    item.insert(QStringLiteral("currentSenderWpm"),
                expose_verified_content ? readable(channel.current_sender_wpm)
                                        : 0.0);
    item.insert(QStringLiteral("contextualText"),
                expose_verified_content
                    ? QString::fromStdString(channel.contextual_text)
                    : QString{});
    item.insert(QStringLiteral("callsign"),
                expose_verified_content
                    ? QString::fromStdString(channel.callsign) : QString{});
    QVariantList qso_participants;
    if (expose_verified_content) {
      qso_participants.reserve(
          static_cast<qsizetype>(channel.qso_participants.size()));
      for (const auto& participant : channel.qso_participants)
        qso_participants.push_back(QString::fromStdString(participant));
    }
    item.insert(QStringLiteral("qsoParticipants"), qso_participants);
    const auto local = std::find_if(
        local_decoder.begin(), local_decoder.end(),
        [&channel](const LocalDecoderChannelPresentation& candidate) {
          return candidate.channel_id == channel.id;
        });
    const auto local_state = local == local_decoder.end()
        ? LocalDecoderPresentationState::Unavailable : local->state;
    item.insert(QStringLiteral("localModelState"),
                localDecoderStateName(local_state));
    item.insert(QStringLiteral("localModelStatus"),
                local != local_decoder.end() && !local->status.isEmpty()
                    ? local->status : localDecoderDefaultStatus(local_state));
    item.insert(QStringLiteral("localModelText"),
                expose_verified_content && local != local_decoder.end()
                    ? local->stable_text : QString{});
    item.insert(QStringLiteral("localModelCallsign"), QString{});
    // No modulo: the generator has a color for every index the core can hand
    // out, which is what removed the palette from the chain that used to cap
    // how many stations could be followed.
    item.insert(QStringLiteral("color"),
                expose_verified_content
                    ? palette.color(channel.color_index)
                    : QString::fromLatin1(kUnverifiedChannelColor));
    model.push_back(item);
  }
  return model;
}

QVariantMap verificationDiagnosticsModel(
    const cwassistant::core::CwVerificationDiagnostics& diagnostics) {
  QVariantMap model;
  model.insert(QStringLiteral("candidateTracks"),
               static_cast<qulonglong>(diagnostics.candidate_tracks));
  model.insert(QStringLiteral("morseLikelyTracks"),
               static_cast<qulonglong>(diagnostics.morse_likely_tracks));
  model.insert(QStringLiteral("verifiedTracks"),
               static_cast<qulonglong>(diagnostics.verified_tracks));
  model.insert(QStringLiteral("verifiedTransitions"),
               static_cast<qulonglong>(diagnostics.verified_transitions));
  model.insert(QStringLiteral("expiredUnverifiedTracks"),
               static_cast<qulonglong>(diagnostics.expired_unverified_tracks));
  model.insert(QStringLiteral("maxDecodedSymbols"),
               diagnostics.maximum_decoded_symbols);
  model.insert(QStringLiteral("maxKeyTransitions"),
               diagnostics.maximum_key_transitions);
  model.insert(QStringLiteral("bestTimingQuality"),
               diagnostics.best_timing_quality);
  model.insert(QStringLiteral("bestCadenceQuality"),
               diagnostics.best_cadence_quality);
  model.insert(QStringLiteral("bestNarrowbandCoherence"),
               diagnostics.best_narrowband_coherence);
  QVariantMap reason_counts;
  for (std::size_t reason = 0; reason < diagnostics.current_reason_counts.size();
       ++reason) {
    const auto count = diagnostics.current_reason_counts[reason];
    if (count == 0) {
      continue;
    }
    reason_counts.insert(
        QString::fromLatin1(cwassistant::core::cwVerificationReasonName(
            static_cast<cwassistant::core::CwVerificationReason>(reason))),
        static_cast<qulonglong>(count));
  }
  model.insert(QStringLiteral("reasonCounts"), reason_counts);
  return model;
}

}  // namespace cwassistant::desktop
