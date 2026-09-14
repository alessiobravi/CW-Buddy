#include "replay_controller.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QIODevice>
#include <QMetaObject>
#include <QMediaDevices>
#include <QPermissions>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QUrl>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cwassistant/core/callsign_evidence.hpp"
#include "cwassistant/core/spectrum_analyzer.hpp"
#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/frequency_plan.hpp"
#include "cwassistant/core/wav_replay_source.hpp"
#include "decoder_channel_model.hpp"
#include "../dxcluster/dx_cluster_client.hpp"
#include "../sdr/sdr_receiver.hpp"
#include "../decoder/local_character_decoder.hpp"
#include "../sdr/sdr_capture_worker.hpp"
#include "live_audio_worker.hpp"

namespace cwassistant::desktop {
namespace {

// Nanoseconds since the Unix epoch.
//
// Every timestamp that reaches CwSpotRegistry -- the observation times parsed
// out of a feed and the "now" passed to expire() and nearFrequency() -- has to
// be read from one clock, or a spot's age is the difference between two
// unrelated origins and the retention window means nothing. A steady clock
// cannot be used: an observation time arrives from a remote station as a
// wall-clock instant, and only a wall clock can be compared with it.
[[nodiscard]] std::uint64_t currentUnixTimeNs() noexcept {
  const qint64 epoch_ms = QDateTime::currentMSecsSinceEpoch();
  return epoch_ms <= 0 ? 0ULL
                       : static_cast<std::uint64_t>(epoch_ms) * 1'000'000ULL;
}

QString encodedAudioDeviceId(const QAudioDevice& device) {
  return QString::fromLatin1(
      device.id().toBase64(QByteArray::Base64UrlEncoding |
                           QByteArray::OmitTrailingEquals));
}

QAudioDevice monitorOutputDevice(const QString& requested_id) {
  if (requested_id.isEmpty()) return QMediaDevices::defaultAudioOutput();
  for (const auto& device : QMediaDevices::audioOutputs()) {
    if (encodedAudioDeviceId(device) == requested_id) return device;
  }
  return {};
}

QByteArray monitorBytes(const std::vector<float>& samples) {
  if (samples.empty()) return {};
  return QByteArray(reinterpret_cast<const char*>(samples.data()),
                    static_cast<qsizetype>(samples.size() * sizeof(float)));
}

bool sameDecoderSessionIdentity(const QVariantMap& previous,
                                const QVariantMap& current) {
  const auto previous_id =
      previous.value(QStringLiteral("id")).toULongLong();
  const auto current_id = current.value(QStringLiteral("id")).toULongLong();
  if (previous_id != 0U && previous_id == current_id) return true;

  // A retained visual stream can be reacquired under a new low-level tracker
  // ID. Keep the existing QML delegate alive in that case: destroying it can
  // swallow a pointer press on one of the card controls. Colour reservations
  // are frequency-stable, and the controller uses this same 35 Hz tolerance
  // when it reconciles an open session with a reacquired stream.
  if (previous.value(QStringLiteral("color")).toString().isEmpty() ||
      previous.value(QStringLiteral("color")) !=
          current.value(QStringLiteral("color"))) {
    return false;
  }
  const auto identity_frequency = [](const QVariantMap& item) {
    const QVariant presentation =
        item.value(QStringLiteral("presentationFrequencyHz"));
    return presentation.isValid()
        ? presentation.toDouble()
        : item.value(QStringLiteral("audioFrequencyHz")).toDouble();
  };
  return std::abs(identity_frequency(previous) - identity_frequency(current)) <=
         35.0;
}

}  // namespace

DecoderSessionListModel::DecoderSessionListModel(QObject* parent)
    : QAbstractListModel(parent) {}

int DecoderSessionListModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(sessions_.size());
}

QVariant DecoderSessionListModel::data(const QModelIndex& index,
                                       const int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= sessions_.size() ||
      (role != kModelDataRole && role != Qt::DisplayRole)) {
    return {};
  }
  return sessions_.at(index.row());
}

QHash<int, QByteArray> DecoderSessionListModel::roleNames() const {
  return {{kModelDataRole, QByteArrayLiteral("modelData")}};
}

void DecoderSessionListModel::replace(const QVariantList& sessions) {
  bool same_identity_order = sessions_.size() == sessions.size();
  if (same_identity_order) {
    for (qsizetype row = 0; row < sessions.size(); ++row) {
      if (!sameDecoderSessionIdentity(sessions_.at(row).toMap(),
                                      sessions.at(row).toMap())) {
        same_identity_order = false;
        break;
      }
    }
  }
  if (!same_identity_order) {
    beginResetModel();
    sessions_ = sessions;
    endResetModel();
    return;
  }
  for (qsizetype row = 0; row < sessions.size(); ++row) {
    if (sessions_.at(row) == sessions.at(row)) continue;
    sessions_[row] = sessions.at(row);
    const QModelIndex changed = index(static_cast<int>(row), 0);
    emit dataChanged(changed, changed, {kModelDataRole});
  }
}

QList<qulonglong> reconcileDecoderSessionOrder(
    const QList<qulonglong>& requested_order,
    const QVariantList& previous_sessions,
    const QVariantList& current_channels) {
  const auto identity_frequency = [](const QVariantMap& item) {
    const QVariant presentation =
        item.value(QStringLiteral("presentationFrequencyHz"));
    return presentation.isValid()
        ? presentation.toDouble()
        : item.value(QStringLiteral("audioFrequencyHz")).toDouble();
  };
  QHash<qulonglong, QVariantMap> current_by_id;
  QHash<qulonglong, QVariantMap> previous_by_id;
  for (const QVariant& value : current_channels) {
    const QVariantMap channel = value.toMap();
    current_by_id.insert(
        channel.value(QStringLiteral("id")).toULongLong(), channel);
  }
  for (const QVariant& value : previous_sessions) {
    const QVariantMap session = value.toMap();
    previous_by_id.insert(
        session.value(QStringLiteral("id")).toULongLong(), session);
  }

  QList<qulonglong> reconciled;
  reconciled.reserve(requested_order.size());
  for (const qulonglong requested_id : requested_order) {
    if (current_by_id.contains(requested_id)) {
      if (!reconciled.contains(requested_id)) reconciled.push_back(requested_id);
      continue;
    }
    const QVariantMap previous = previous_by_id.value(requested_id);
    if (previous.isEmpty()) continue;
    const QString previous_color = previous.value(QStringLiteral("color")).toString();
    const double previous_frequency = identity_frequency(previous);
    qulonglong replacement_id = 0;
    double nearest_distance = 35.0;
    for (auto current = current_by_id.cbegin();
         current != current_by_id.cend(); ++current) {
      const QVariantMap& channel = current.value();
      if (channel.value(QStringLiteral("color")).toString() != previous_color)
        continue;
      const double distance = std::abs(
          identity_frequency(channel) - previous_frequency);
      if (distance <= nearest_distance && !reconciled.contains(current.key())) {
        nearest_distance = distance;
        replacement_id = current.key();
      }
    }
    if (replacement_id != 0) reconciled.push_back(replacement_id);
  }
  return reconciled;
}

std::optional<std::string> freshCharacterRefinementCallEvidence(
    const std::string_view stable_text,
    const std::size_t previous_stable_size) {
  if (stable_text.size() <= previous_stable_size) return std::nullopt;

  std::optional<std::string> fresh_call;
  std::size_t token_start = 0U;
  while (token_start < stable_text.size()) {
    while (token_start < stable_text.size()) {
      const auto character =
          static_cast<unsigned char>(stable_text[token_start]);
      if (std::isalnum(character) != 0 || character == '/') break;
      ++token_start;
    }
    if (token_start == stable_text.size()) break;
    std::size_t token_end = token_start;
    while (token_end < stable_text.size()) {
      const auto character = static_cast<unsigned char>(stable_text[token_end]);
      if (std::isalnum(character) == 0 && character != '/') break;
      ++token_end;
    }

    const std::string_view token =
        stable_text.substr(token_start, token_end - token_start);
    const auto candidate =
        cwassistant::core::CallsignPolicy::latest_in_text(token);
    if (candidate && token_end > previous_stable_size) {
      const std::size_t previous_token_length = previous_stable_size > token_start
          ? std::min(previous_stable_size, token_end) - token_start
          : 0U;
      const bool was_already_a_complete_call = previous_token_length > 0U &&
          cwassistant::core::CallsignPolicy::latest_in_text(
              token.substr(0U, previous_token_length)).has_value();
      if (!was_already_a_complete_call) fresh_call = std::string(token);
    }
    token_start = token_end;
  }
  return fresh_call;
}

std::optional<AdvisoryCallsignPresentation> advisoryCallsignPresentation(
    const QVariantMap& channel,
    const cwassistant::core::OfflineCallsignDatabase& database,
    QString* diagnostic_reason, const bool allow_database_correction) {
  const auto reject = [diagnostic_reason](const char* reason) {
    if (diagnostic_reason != nullptr)
      *diagnostic_reason = QString::fromLatin1(reason);
    return std::optional<AdvisoryCallsignPresentation>{};
  };
  if (!channel.value(QStringLiteral("verifiedCw")).toBool())
    return reject("stream-not-verified");
  if (!channel.value(QStringLiteral("callsign")).toString().isEmpty())
    return reject("callsign-already-confirmed");

  const auto latest_mask = [](const QString& source) {
    const std::string text = source.right(2'048).toStdString();
    std::optional<std::string> result;
    std::string token;
    for (const unsigned char character : text) {
      if (std::isalnum(character) != 0 || character == '?' ||
          character == '/') {
        token.push_back(static_cast<char>(character));
      } else if (!token.empty()) {
        if (token.find('?') != std::string::npos &&
            cwassistant::core::is_callsign_like_span(token, 2U)) {
          result = token;
        }
        token.clear();
      }
    }
    // Deliberately do not accept the trailing token: without a right-hand
    // boundary it can still grow into a different callsign.
    return result;
  };
  auto mask = latest_mask(
      channel.value(QStringLiteral("refinedText")).toString());
  if (!mask) {
    mask = latest_mask(channel.value(QStringLiteral("text")).toString());
  }
  if (!mask) return reject("no-completed-uncertain-span");
  std::ranges::transform(*mask, mask->begin(), [](const unsigned char value) {
    return static_cast<char>(std::toupper(value));
  });

  // Compare the completed transcript span with a current acoustic candidate,
  // not with character positions in the cumulative transcript. A wildcard is
  // free, while a substitution/insertion/deletion consumes one of the two
  // bounded edits. This admits the observed "SV2?L?" -> "SV2HQL" and
  // short boundary-loss cases without turning the directory into a decoder.
  const auto mask_distance = [&mask](const std::string& candidate) {
    constexpr std::size_t kMaximumDistance = 2U;
    if (std::max(mask->size(), candidate.size()) -
            std::min(mask->size(), candidate.size()) >
        kMaximumDistance) {
      return kMaximumDistance + 1U;
    }
    std::vector<std::size_t> previous(candidate.size() + 1U);
    std::vector<std::size_t> current(candidate.size() + 1U);
    for (std::size_t index = 0; index <= candidate.size(); ++index) {
      previous[index] = index;
    }
    for (std::size_t left = 1; left <= mask->size(); ++left) {
      current[0] = left;
      for (std::size_t right = 1; right <= candidate.size(); ++right) {
        const bool same = (*mask)[left - 1U] == '?' ||
            (*mask)[left - 1U] == candidate[right - 1U];
        current[right] = std::min({
            previous[right - 1U] + (same ? 0U : 1U),
            previous[right] + 1U,
            current[right - 1U] + 1U,
        });
      }
      previous.swap(current);
    }
    return previous.back();
  };

  const QVariantList alternatives =
      channel.value(QStringLiteral("acousticAlternatives")).toList();
  if (alternatives.size() < 2)
    return reject("fewer-than-two-acoustic-alternatives");
  std::uint64_t newest_observation_id = 0;
  for (const QVariant& value : alternatives) {
    const auto alternative = value.toMap();
    const auto first = alternative.value(
        QStringLiteral("firstObservationId")).toULongLong();
    const auto last = alternative.value(
        QStringLiteral("lastObservationId")).toULongLong();
    if (first != 0U && last >= first) newest_observation_id = std::max(
        newest_observation_id, static_cast<std::uint64_t>(last));
  }
  if (newest_observation_id == 0U)
    return reject("missing-acoustic-observation-identity");
  double best_cost = std::numeric_limits<double>::infinity();
  for (const QVariant& value : alternatives) {
    const auto alternative = value.toMap();
    const auto first = alternative.value(
        QStringLiteral("firstObservationId")).toULongLong();
    const auto last = alternative.value(
        QStringLiteral("lastObservationId")).toULongLong();
    const double cost = alternative.value(QStringLiteral("cost")).toDouble();
    if (first != 0U && last >= first && last == newest_observation_id &&
        std::isfinite(cost)) {
      best_cost = std::min(best_cost, cost);
    }
  }
  if (!std::isfinite(best_cost))
    return reject("missing-current-acoustic-cost");

  struct CandidateEvidence {
    int alternatives{0};
    float support{0.0F};
    float relative_cost{std::numeric_limits<float>::infinity()};
  };
  std::map<std::string, CandidateEvidence> evidence;
  for (const QVariant& value : alternatives) {
    const QVariantMap alternative = value.toMap();
    const auto first = alternative.value(
        QStringLiteral("firstObservationId")).toULongLong();
    const auto last = alternative.value(
        QStringLiteral("lastObservationId")).toULongLong();
    const double cost = alternative.value(QStringLiteral("cost")).toDouble();
    const double confidence =
        alternative.value(QStringLiteral("confidence")).toDouble();
    if (first == 0U || last < first || last != newest_observation_id ||
        !std::isfinite(cost) || !std::isfinite(confidence) ||
        cost > best_cost + 1.0 || confidence < 0.30) {
      continue;
    }
    const std::string text =
        alternative.value(QStringLiteral("text")).toString().toStdString();
    std::vector<std::string> matches;
    std::string token;
    const auto consider_token = [&]() {
      if (token.empty()) return;
      std::ranges::transform(token, token.begin(), [](const unsigned char item) {
        return static_cast<char>(std::toupper(item));
      });
      if (cwassistant::core::is_callsign_like_span(token, 0U) &&
          mask_distance(token) <= 2U) {
        matches.push_back(token);
      }
      token.clear();
    };
    for (const unsigned char character : text) {
      if (std::isalnum(character) != 0 || character == '/') {
        token.push_back(static_cast<char>(character));
      } else {
        consider_token();
      }
    }
    consider_token();
    std::ranges::sort(matches);
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    if (matches.size() != 1U) continue;
    auto& item = evidence[matches.front()];
    ++item.alternatives;
    item.support = std::max(item.support, static_cast<float>(confidence));
    item.relative_cost = std::min(
        item.relative_cost, static_cast<float>(cost - best_cost));
  }

  // The acoustics choose the candidate. The directory may only annotate the
  // acoustic winner; it cannot promote a weaker database entry over a better
  // non-directory hypothesis. Requiring two current paths retains the prior
  // conservative evidence floor.
  const auto ranks_before = [](const auto& left, const auto& right) {
    if (left.second.alternatives != right.second.alternatives) {
      return left.second.alternatives > right.second.alternatives;
    }
    const float left_score = left.second.support -
        0.20F * left.second.relative_cost;
    const float right_score = right.second.support -
        0.20F * right.second.relative_cost;
    if (left_score != right_score) return left_score > right_score;
    return left.first < right.first;
  };
  std::vector<std::pair<std::string, CandidateEvidence>> eligible;
  for (const auto& item : evidence) {
    if (item.second.alternatives >= 2) eligible.push_back(item);
  }
  if (eligible.empty())
    return reject("insufficient-current-acoustic-agreement");
  std::ranges::sort(eligible, ranks_before);
  const auto& winner = eligible.front();
  if (eligible.size() > 1U) {
    const auto& runner_up = eligible[1];
    const float winner_score = winner.second.support -
        0.20F * winner.second.relative_cost;
    const float runner_up_score = runner_up.second.support -
        0.20F * runner_up.second.relative_cost;
    if (winner.second.alternatives == runner_up.second.alternatives &&
        std::abs(winner_score - runner_up_score) < 0.02F) {
      return reject("ambiguous-acoustic-winner");
    }
  }
  bool database_match = database.contains(winner.first);
  std::string presented = winner.first;
  bool database_corrected = false;
  if (!database_match && allow_database_correction) {
    // A verified acoustic winner that is one or two edits from a directory
    // entry is usually that entry misread, not a different station: the
    // acoustic path routinely loses the opening characters of a transmission
    // because element boundaries must be committed before any speed estimate
    // exists. Correct it only when a single entry is strictly closest --
    // an ambiguous neighbourhood corrects nothing, because presenting the
    // wrong station is worse than presenting an incomplete one.
    const auto matches = database.query(winner.first, 2U, 8U);
    const bool strictly_closest = matches.size() == 1U ||
        (matches.size() > 1U &&
         matches.front().edit_distance < matches[1].edit_distance);
    if (!matches.empty() && strictly_closest &&
        matches.front().edit_distance > 0U) {
      presented = matches.front().callsign;
      database_match = true;
      database_corrected = true;
    }
  }
  const std::vector<cwassistant::core::CallsignRawHypothesis> hypotheses{{
      .raw_span = *mask,
      .candidate = presented,
      .acoustic_support = winner.second.support,
      .acoustic_edit_cost = winner.second.relative_cost,
  }};
  std::vector<cwassistant::core::CallsignProviderEvidence> providers;
  if (database_match) {
    providers.push_back({
        .candidate = presented,
        .provider_id = "offline-directory",
        .provider_label = "Offline callsign list",
        .kind = cwassistant::core::CallsignEvidenceKind::DirectoryListing,
        .requested_weight = 0.06F,
        .retrieved_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{1}},
        .rationale = database_corrected
            ? "Nearest local-list entry to the acoustic winner"
            : "Exact local-list entry supporting the acoustic winner",
    });
  }
  const auto ranked = cwassistant::core::rank_callsign_suggestions(
      *mask, hypotheses, providers,
      {.maximum_span_edit_distance = 2U});
  if (ranked.empty())
    return reject("bounded-ranker-rejected");
  if (diagnostic_reason != nullptr)
    *diagnostic_reason = database_corrected
        ? QStringLiteral("suggested-database-corrected")
        : (database_match ? QStringLiteral("suggested-database")
                          : QStringLiteral("suggested-acoustic"));
  return AdvisoryCallsignPresentation{
      .callsign = QString::fromStdString(ranked.front().candidate),
      .raw_span = QString::fromStdString(*mask),
      .database_match = database_match,
      .database_corrected = database_corrected,
      .agreeing_alternatives = winner.second.alternatives,
      .acoustic_support = ranked.front().acoustic_support,
      .relative_cost = ranked.front().acoustic_edit_cost,
  };
}

namespace {

// Whether `text` contains `own_callsign` as a whole token, a token being a
// maximal run of letters, digits and '/'.
//
// This asks the question the published transcript scan has always asked. It
// used to be asked as
//
//   heard.toUpper().split(QRegularExpression("[^A-Z0-9/]+"), SkipEmptyParts)
//
// which answers it by uppercasing a copy of the entire cumulative transcript,
// compiling a pattern, and allocating one QString per token of a string that
// grows for the life of the stream. What costs is the walk, not the pattern:
// constructing the QRegularExpression measures 7 us against the 1,300 us the
// split itself costs on an 8,000-character transcript, so hoisting it out of
// the loop -- the obvious fix -- would have recovered half a percent.
//
// The expense is that the walk is proportional to a transcript with no bound
// on it, which is what turned a working application into a frozen one after an
// hour on a busy band. The scan below allocates nothing and reads each
// character once. Measured in an optimized build over 48 transcripts of 32,000
// characters, the split costs 1.5 ms each and this costs 41 us, and
// spectrum_waterfall_startup_test.cpp holds that ratio to the two
// implementations rather than to a duration.
//
// Token boundaries are identical to the pattern's for every input the decoder
// can produce. The one divergence is a character whose Unicode uppercase
// expands to more than one ASCII character -- 'ß' uppercases to "SS", which
// the split would have treated as two token characters and this treats as a
// separator. Neither side can contain one: the transcript is assembled from
// the Morse alphabet, which is ASCII, and own_callsign_ is a callsign.
[[nodiscard]] bool mentionsCallsignToken(const QStringView text,
                                         const QStringView own_callsign)
    noexcept {
  if (own_callsign.isEmpty()) return false;
  qsizetype matched = 0;
  // The token being read has already diverged from the callsign, so the rest
  // of it need not be compared -- only its end matters.
  bool diverged = false;
  for (const QChar character : text) {
    const char16_t value = character.unicode();
    const char16_t upper = (value >= u'a' && value <= u'z')
        ? static_cast<char16_t>(value - u'a' + u'A')
        : value;
    const bool inside_token = (upper >= u'A' && upper <= u'Z') ||
                              (upper >= u'0' && upper <= u'9') ||
                              upper == u'/';
    if (!inside_token) {
      if (!diverged && matched == own_callsign.size()) return true;
      matched = 0;
      diverged = false;
      continue;
    }
    if (diverged) continue;
    if (matched < own_callsign.size() &&
        own_callsign[matched].unicode() == upper) {
      ++matched;
    } else {
      diverged = true;
    }
  }
  return !diverged && matched == own_callsign.size();
}

}  // namespace

void DecoderChannelPresentationCache::setContext(
    const QString& own_callsign, const bool allow_database_correction) {
  if (own_callsign_ == own_callsign &&
      allow_database_correction_ == allow_database_correction) {
    return;
  }
  own_callsign_ = own_callsign;
  allow_database_correction_ = allow_database_correction;
  entries_.clear();
}

void DecoderChannelPresentationCache::invalidate() noexcept {
  entries_.clear();
}

const DecoderChannelPresentationCache::Derived&
DecoderChannelPresentationCache::derive(
    const qulonglong channel_id, const QVariantMap& channel,
    const cwassistant::core::OfflineCallsignDatabase& database) {
  const QString text = channel.value(QStringLiteral("text")).toString();
  const QString refined_text =
      channel.value(QStringLiteral("refinedText")).toString();
  const QString callsign = channel.value(QStringLiteral("callsign")).toString();
  const QVariantList acoustic_alternatives =
      channel.value(QStringLiteral("acousticAlternatives")).toList();
  const bool verified_cw = channel.value(QStringLiteral("verifiedCw")).toBool();

  Entry& entry = entries_[channel_id];
  entry.publish = publish_;
  // Compared in the order most likely to differ first, so a stream that is
  // actively decoding is recognised as changed without comparing the rest.
  // Equal QStrings still cost a compare of their contents, which is the price
  // of an exact answer: a memo keyed on lengths or observation identifiers
  // would miss a refinement that rewrites a transcript without changing its
  // length, and would then show the operator a suggestion drawn from evidence
  // that no longer exists.
  if (entry.valid && entry.text == text && entry.refined_text == refined_text &&
      entry.verified_cw == verified_cw && entry.callsign == callsign &&
      entry.acoustic_alternatives == acoustic_alternatives) {
    return entry.derived;
  }

  ++computations_;
  entry.text = text;
  entry.refined_text = refined_text;
  entry.callsign = callsign;
  entry.acoustic_alternatives = acoustic_alternatives;
  entry.verified_cw = verified_cw;
  entry.valid = true;
  entry.derived = Derived{};

  // Somebody is calling the operator on this stream. Detected on the spectrum
  // model rather than only on an opened card, because the operator has to
  // notice it before deciding which stream to open -- an alert that only
  // appears once the card is already open cannot draw attention to a call the
  // operator has not found yet.
  //
  // The two transcripts are scanned separately rather than concatenated: a
  // token cannot span the separator the concatenation inserted, so the answer
  // is the same and the copy is not made.
  entry.derived.calling_own_station =
      !own_callsign_.isEmpty() &&
      (mentionsCallsignToken(text, own_callsign_) ||
       mentionsCallsignToken(refined_text, own_callsign_));

  // Whether a confirmed callsign appears in the operator's offline list. The
  // operator needs to tell a callsign the directory corroborates from one that
  // was only heard; both are legitimate, and an unlisted station is common, so
  // this reports corroboration rather than correctness.
  entry.derived.callsign_in_database =
      !callsign.isEmpty() && database.size() > 0U &&
      database.contains(callsign.toStdString());

  entry.derived.suggestion_diagnostic = QStringLiteral("not-evaluated");
  if (!callsign.isEmpty()) {
    entry.derived.suggestion_diagnostic =
        QStringLiteral("callsign-already-confirmed");
    return entry.derived;
  }
  if (const auto suggestion = advisoryCallsignPresentation(
          channel, database, &entry.derived.suggestion_diagnostic,
          allow_database_correction_)) {
    entry.derived.suggestion = suggestion->callsign;
    entry.derived.suggestion_raw_span = suggestion->raw_span;
    entry.derived.suggestion_source = suggestion->database_match
        ? QStringLiteral("offline-directory")
        : QStringLiteral("acoustic-consensus");
    entry.derived.suggestion_agreeing_alternatives =
        suggestion->agreeing_alternatives;
    entry.derived.suggestion_support = suggestion->acoustic_support;
    entry.derived.suggestion_relative_cost = suggestion->relative_cost;
  }
  return entry.derived;
}

void DecoderChannelPresentationCache::endPublish() noexcept {
  std::erase_if(entries_, [this](const auto& item) {
    return item.second.publish != publish_;
  });
  ++publish_;
}

class ReplayWorker final : public QObject {
  Q_OBJECT

 public:
  explicit ReplayWorker(QObject* parent = nullptr)
      : QObject(parent), timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &ReplayWorker::readNextBlock);
  }

 public slots:
  void openPath(const QString& path) {
    timer_->stop();
    source_.stop();
    analyzer_.reset();
    decoder_.reset();
    character_frontends_.reset();
    started_ = false;
    opened_ = source_.open(path.toStdString(), {});
    if (!opened_) {
      emit failed(QString::fromStdString(source_.last_error()));
      return;
    }
    emit opened(QFileInfo(path).fileName(),
                source_.stream_descriptor().sample_rate_hz,
                source_.duration_seconds());
  }

  void play() {
    if (!opened_) {
      emit failed(QStringLiteral("Choose a valid WAV recording first."));
      return;
    }
    if (!started_) {
      if (!source_.start()) {
        emit failed(QString::fromStdString(source_.last_error()));
        return;
      }
      analyzer_.reset();
      decoder_.reset();
      character_frontends_.reset();
      started_ = true;
      emit progress(0.0);
    }
    if (!timer_->isActive()) {
      emit playbackChanged(true);
      timer_->start(0);
    }
  }

  void pause() {
    timer_->stop();
    emit playbackChanged(false);
  }

  void stop() {
    timer_->stop();
    source_.stop();
    analyzer_.reset();
    decoder_.reset();
    character_frontends_.reset();
    started_ = false;
    emit playbackChanged(false);
    emit progress(0.0);
    emit diagnosticsProduced(
        verificationDiagnosticsModel(decoder_.verificationDiagnostics()));
  }

  void shutdown() {
    timer_->stop();
    source_.stop();
  }

  void configure(const int averaging_frames, const int frame_rate_hz,
                 const bool dc_rejection,
                 const bool automatic_gain, const double gain_db,
                 const double automatic_gain_target_dbfs,
                 const bool automatic_bandwidth,
                 const double lower_frequency_hz,
                 const double upper_frequency_hz) {
    const auto previous = analyzer_.config();
    auto config = previous;
    config.averaging_frames = static_cast<std::uint8_t>(
        std::clamp(averaging_frames, 1, 32));
    config.frame_rate_hz = static_cast<std::uint16_t>(
        std::clamp(frame_rate_hz, 1, 120));
    config.audio_dc_rejection = dc_rejection;
    config.audio_automatic_gain = automatic_gain;
    config.audio_gain_db =
        static_cast<float>(std::clamp(gain_db, -40.0, 40.0));
    config.audio_automatic_gain_target_dbfs = static_cast<float>(
        std::clamp(automatic_gain_target_dbfs, -40.0, -1.0));
    config.audio_automatic_bandwidth = automatic_bandwidth;
    config.audio_lower_frequency_hz =
        std::clamp(lower_frequency_hz, 0.0, 95'999.0);
    config.audio_upper_frequency_hz =
        std::clamp(upper_frequency_hz,
                   config.audio_lower_frequency_hz + 1.0, 96'000.0);
    // Presentation settings (averaging, frame rate) must not discard decoder
    // state; only a change to the audio the detector actually receives may.
    const bool signal_path_changed =
        config.audio_dc_rejection != previous.audio_dc_rejection ||
        config.audio_automatic_gain != previous.audio_automatic_gain ||
        config.audio_gain_db != previous.audio_gain_db ||
        config.audio_automatic_gain_target_dbfs !=
            previous.audio_automatic_gain_target_dbfs ||
        config.audio_automatic_bandwidth !=
            previous.audio_automatic_bandwidth ||
        config.audio_lower_frequency_hz != previous.audio_lower_frequency_hz ||
        config.audio_upper_frequency_hz != previous.audio_upper_frequency_hz;
    static_cast<void>(analyzer_.configure(config));
    if (signal_path_changed) {
      decoder_.reset();
      character_frontends_.reset();
    }
  }

  void setOwnCallsign(const QString& callsign) {
    decoder_.setOwnCallsign(callsign.trimmed().toStdString());
  }

  void setKeyingModel(const QString& model) {
    decoder_.setKeyingModel(
        cwassistant::core::cwKeyingModelFromName(model.toStdString()));
  }

  void setOperatorRole(const QString& role) {
    decoder_.setOperatorRole(
        cwassistant::core::cwOperatorRoleFromName(role.toStdString()));
  }

  void setDecodedSignalTimeoutSeconds(const int seconds) {
    decoder_.configure({.decoded_track_retention_seconds =
                            static_cast<double>(std::clamp(seconds, 5, 120))});
  }

  // No reset here, deliberately, for the same reason the live worker does not
  // reset: this setting changes which of the already tracked signals are worth
  // decoding, never the audio the detector receives, so the bank applies it on
  // its next update without losing a track, transcript or confirmed callsign.
  void setWeakSignalDecoding(const bool enabled,
                             const double minimum_decode_snr_db) {
    decoder_.setWeakSignalDecoding(enabled,
                                   static_cast<float>(minimum_decode_snr_db));
  }

  void setLocalCharacterFrontendEnabled(const bool enabled) {
    character_frontends_.setEnabled(enabled);
  }

  void setMonitor(const int mode, const QVariantList& channel_ids,
                  const double reference_tone_hz) {
    const auto selected_mode = mode == 1
        ? cwassistant::core::CwMonitorMode::FullReceiver
        : mode == 2 ? cwassistant::core::CwMonitorMode::SelectedTrack
                    : cwassistant::core::CwMonitorMode::Off;
    std::vector<std::uint64_t> ids;
    ids.reserve(static_cast<std::size_t>(channel_ids.size()));
    for (const QVariant& value : channel_ids)
      ids.push_back(static_cast<std::uint64_t>(value.toULongLong()));
    decoder_.setMonitorTracks(selected_mode, ids, reference_tone_hz);
  }

  void acceptCharacterRefinement(const qulonglong channel_id,
                                 const QString& stable_text,
                                 const qulonglong evidence_timestamp_ns) {
    if (decoder_.acceptCharacterRefinement(
            static_cast<std::uint64_t>(channel_id), stable_text.toStdString(),
            static_cast<std::uint64_t>(evidence_timestamp_ns))) {
      emit decoderProduced(decoderChannelModel(decoder_.channels()));
    }
  }

  void selectDecoderFrequency(const double audio_frequency_hz) {
    const std::uint64_t channel_id =
        decoder_.selectFrequency(audio_frequency_hz);
    if (channel_id == 0) return;
    emit decoderProduced(decoderChannelModel(decoder_.channels()));
    emit manualDecoderSelected(static_cast<qulonglong>(channel_id));
  }

 signals:
  void opened(const QString& name, double sample_rate, double duration);
  void failed(const QString& message);
  void playbackChanged(bool playing);
  void progress(double seconds);
  void frameProduced(const cwassistant::desktop::SpectrumFrame& frame);
  void decoderProduced(const QVariantList& channels);
  void diagnosticsProduced(const QVariantMap& diagnostics);
  void manualDecoderSelected(qulonglong channel_id);
  void characterWindowProduced(
      int source_mode,
      cwassistant::desktop::CwCharacterFeatureWindowPtr window);
  void monitorAudioProduced(const QByteArray& float_mono_audio,
                            double sample_rate_hz);
  void ended();

 private slots:
  void readNextBlock() {
    using namespace std::chrono_literals;
    cwassistant::core::RealtimeSampleBlock block;
    if (!source_.read(block, 0ms)) {
      started_ = false;
      emit playbackChanged(false);
      emit progress(source_.duration_seconds());
      emit ended();
      return;
    }

    auto snapshots = analyzer_.process(block);
    for (const auto& snapshot : snapshots) {
      // Detection consumes the unaveraged bins and applies its own fixed-time
      // smoothing, so the operator's display averaging cannot change which
      // signals are discovered or how quickly they qualify.
      static_cast<void>(decoder_.updateSpectrum(
          snapshot.timestamp_ns, snapshot.lower_frequency_hz,
          snapshot.upper_frequency_hz, snapshot.instantaneous_bins_dbfs,
          false));
    }
    const auto& decoder_channels = decoder_.processSamples(block);
    const QByteArray monitor_audio = monitorBytes(decoder_.monitorAudio());
    if (!monitor_audio.isEmpty())
      emit monitorAudioProduced(monitor_audio, block.stream.sample_rate_hz);
    const auto& character_tracks = decoder_.characterRefinementTracks();
    for (auto& window : character_frontends_.process(block, character_tracks))
      emit characterWindowProduced(1, std::move(window));
    for (auto& snapshot : snapshots) {
      QVector<float> bins(static_cast<qsizetype>(snapshot.bins_dbfs.size()));
      std::copy(snapshot.bins_dbfs.cbegin(), snapshot.bins_dbfs.cend(),
                bins.begin());
      QVector<float> instantaneous_bins(
          static_cast<qsizetype>(snapshot.instantaneous_bins_dbfs.size()));
      std::copy(snapshot.instantaneous_bins_dbfs.cbegin(),
                snapshot.instantaneous_bins_dbfs.cend(),
                instantaneous_bins.begin());
      SpectrumFrame frame{
          .bins_dbfs = std::move(bins),
          .sequence = snapshot.sequence,
          .timestamp_ns = snapshot.timestamp_ns,
          .lower_frequency_hz = snapshot.lower_frequency_hz,
          .upper_frequency_hz = snapshot.upper_frequency_hz,
          .instantaneous_bins_dbfs = std::move(instantaneous_bins),
      };
      emit frameProduced(frame);
    }
    emit decoderProduced(decoderChannelModel(decoder_channels));
    emit diagnosticsProduced(
        verificationDiagnosticsModel(decoder_.verificationDiagnostics()));

    const double sample_rate = source_.stream_descriptor().sample_rate_hz;
    emit progress(static_cast<double>(source_.position_frames()) / sample_rate);
    const int block_duration_ms = std::max(
        1, static_cast<int>(std::lround(
               static_cast<double>(block.sample_count) * 1'000.0 / sample_rate)));
    timer_->start(block_duration_ms);
  }

 private:
  QTimer* timer_;
  cwassistant::core::WavReplaySource source_;
  cwassistant::core::SpectrumAnalyzer analyzer_;
  cwassistant::core::CwChannelBank decoder_;
  LocalCharacterFrontendBank character_frontends_;
  bool opened_{false};
  bool started_{false};
};

ReplayController::ReplayController(QObject* parent) : QObject(parent) {
  qRegisterMetaType<SpectrumFrame>();
  // A spot ages whether or not anything new arrives, so the published model is
  // rebuilt on a slow cadence as well as on receipt. Five seconds is finer
  // than anything this drives -- spot ages are read in minutes -- and the
  // timer only runs while the operator has the feature switched on.
  dx_spot_expiry_timer_.setSingleShot(false);
  dx_spot_expiry_timer_.setInterval(5'000);
  connect(&dx_spot_expiry_timer_, &QTimer::timeout, this,
          [this] { rebuildDxSpotModel(); });
  // The GUI thread's own heartbeat. Precise rather than coarse: Qt allows a
  // coarse timer up to five percent of slack, which at this interval is
  // several milliseconds of deliberate drift that would be reported as
  // lateness the event loop never actually suffered.
  gui_heartbeat_timer_.setSingleShot(false);
  gui_heartbeat_timer_.setTimerType(Qt::PreciseTimer);
  gui_heartbeat_timer_.setInterval(
      LiveAudioDspWorker::kGuiHeartbeatIntervalMs);
  connect(&gui_heartbeat_timer_, &QTimer::timeout, this,
          &ReplayController::publishGuiHeartbeat);
  qRegisterMetaType<CwCharacterFeatureWindowPtr>();
  qRegisterMetaType<CwCharacterHypothesisPtr>();
  auto* character_worker = new LocalCharacterInferenceWorker;
  character_inference_worker_ = character_worker;
  character_worker->moveToThread(&character_inference_thread_);
  connect(&character_inference_thread_, &QThread::finished, character_worker,
          &QObject::deleteLater);
  connect(this, &ReplayController::localCharacterDecoderConfigureRequested,
          character_worker, &LocalCharacterInferenceWorker::configure);
  connect(this, &ReplayController::localCharacterResetRequested,
          character_worker, &LocalCharacterInferenceWorker::discardPending,
          Qt::DirectConnection);
  connect(character_worker, &LocalCharacterInferenceWorker::statusChanged,
          this, [this](const QString& state, const QString& detail) {
            local_character_state_ = state;
            local_character_status_ = detail;
            const bool ready = state == QStringLiteral("ready");
            if (!ready) local_character_consensus_.clear();
            emit replayCharacterFrontendEnabledRequested(ready);
            emit liveCharacterFrontendEnabledRequested(ready);
            rebuildDecoderModels();
          });
  connect(character_worker, &LocalCharacterInferenceWorker::resultReady,
          this, [this](const int source_mode,
                       const CwCharacterHypothesisPtr& hypothesis) {
            if (!hypothesis || source_mode != source_mode_ ||
                local_character_state_ != QStringLiteral("ready")) return;
            const auto id = hypothesis->track.track_id;
            auto [entry, inserted] = local_character_consensus_.try_emplace(id);
            static_cast<void>(inserted);
            const std::size_t previous_stable_size =
                entry->second.stableText().size();
            const auto update = entry->second.process(*hypothesis);
            const auto fresh_evidence = freshCharacterRefinementCallEvidence(
                entry->second.stableText(), previous_stable_size);
            if (fresh_evidence) {
              const QString stable_text =
                  QString::fromStdString(*fresh_evidence);
              if (source_mode != 1) {
                emit liveCharacterRefinementRequested(
                    static_cast<qulonglong>(id), stable_text,
                    static_cast<qulonglong>(hypothesis->window_ended_ns));
              } else {
                emit replayCharacterRefinementRequested(
                    static_cast<qulonglong>(id), stable_text,
                    static_cast<qulonglong>(hypothesis->window_ended_ns));
              }
            }
            if (update.changed) rebuildDecoderModels();
          });
  character_inference_thread_.setObjectName(
      QStringLiteral("Local character inference"));
  character_inference_thread_.start();
  connect(this, &ReplayController::sourceReset, this,
          &ReplayController::resetDecoder);
  auto* worker = new ReplayWorker;
  worker_ = worker;
  worker->moveToThread(&worker_thread_);
  connect(&worker_thread_, &QThread::finished, worker, &QObject::deleteLater);
  connect(this, &ReplayController::openRequested, worker,
          &ReplayWorker::openPath);
  connect(this, &ReplayController::playRequested, worker, &ReplayWorker::play);
  connect(this, &ReplayController::pauseRequested, worker,
          &ReplayWorker::pause);
  connect(this, &ReplayController::stopRequested, worker, &ReplayWorker::stop);
  connect(this, &ReplayController::configureRequested, worker,
          &ReplayWorker::configure);
  connect(this, &ReplayController::decodedSignalTimeoutRequested, worker,
          &ReplayWorker::setDecodedSignalTimeoutSeconds);
  connect(this, &ReplayController::weakSignalDecodingRequested, worker,
          &ReplayWorker::setWeakSignalDecoding);
  connect(this, &ReplayController::ownCallsignRequested, worker,
          &ReplayWorker::setOwnCallsign);
  connect(this, &ReplayController::keyingModelRequested, worker,
          &ReplayWorker::setKeyingModel);
  connect(this, &ReplayController::operatorRoleRequested, worker,
          &ReplayWorker::setOperatorRole);
  connect(this, &ReplayController::monitorConfigureRequested, worker,
          &ReplayWorker::setMonitor);
  connect(this, &ReplayController::replayCharacterFrontendEnabledRequested,
          worker, &ReplayWorker::setLocalCharacterFrontendEnabled);
  connect(this, &ReplayController::replayCharacterRefinementRequested,
          worker, &ReplayWorker::acceptCharacterRefinement);
  connect(worker, &ReplayWorker::characterWindowProduced, character_worker,
          &LocalCharacterInferenceWorker::submit, Qt::DirectConnection);
  connect(this, &ReplayController::manualDecoderFrequencyRequested, worker,
          &ReplayWorker::selectDecoderFrequency);
  connect(worker, &ReplayWorker::opened, this,
          [this](const QString& name, const double sample_rate,
                 const double duration) {
            source_name_ = name;
            sample_rate_ = sample_rate;
            duration_seconds_ = duration;
            position_seconds_ = 0.0;
            source_loaded_ = true;
            playing_ = false;
            blocking_error_.clear();
            status_text_ = QStringLiteral("Ready: %1 • %2 Hz • %3 s")
                               .arg(name)
                               .arg(sample_rate, 0, 'f', 0)
                               .arg(duration, 0, 'f', 2);
            emit sourceReset();
            emit stateChanged();
          });
  connect(worker, &ReplayWorker::failed, this, [this](const QString& message) {
    source_loaded_ = false;
    playing_ = false;
    setBlockingError(QStringLiteral("WAV replay error: %1").arg(message));
  });
  connect(worker, &ReplayWorker::playbackChanged, this,
          [this](const bool playing) {
            if (playing_ != playing) {
              playing_ = playing;
              emit stateChanged();
            }
          });
  connect(worker, &ReplayWorker::progress, this, [this](const double seconds) {
    position_seconds_ = seconds;
    emit stateChanged();
  });
  connect(worker, &ReplayWorker::frameProduced, this,
          &ReplayController::frameReady);
  connect(worker, &ReplayWorker::decoderProduced, this,
          &ReplayController::acceptDecoderChannels);
  connect(worker, &ReplayWorker::manualDecoderSelected, this,
          &ReplayController::openDecoderSession);
  connect(worker, &ReplayWorker::diagnosticsProduced, this,
          [this](const QVariantMap& diagnostics) {
            verification_diagnostics_ = diagnostics;
            emit decoderChanged();
          });
  connect(worker, &ReplayWorker::monitorAudioProduced, this,
          &ReplayController::writeMonitorAudio);
  connect(worker, &ReplayWorker::ended, this, [this] {
    playing_ = false;
    status_text_ = QStringLiteral("Replay complete");
    emit stateChanged();
  });
  worker_thread_.setObjectName(QStringLiteral("WAV replay DSP"));
  worker_thread_.start();

  auto pipe = std::make_shared<LiveAudioPipe>();
  auto* capture_worker = new LiveAudioCaptureWorker(pipe);
  auto* sdr_worker = new SdrCaptureWorker(pipe);
  auto* dsp_worker = new LiveAudioDspWorker(std::move(pipe));
  audio_capture_worker_ = capture_worker;
  sdr_capture_worker_ = sdr_worker;
  audio_dsp_worker_ = dsp_worker;
  capture_worker->moveToThread(&audio_capture_thread_);
  // LiveAudioPipe is deliberately SPSC. Keep both mutually-exclusive capture
  // producers on one thread so source-switch commands are serialized and the
  // ring can never briefly become a two-producer queue.
  sdr_worker->moveToThread(&audio_capture_thread_);
  dsp_worker->moveToThread(&audio_dsp_thread_);
  connect(&audio_capture_thread_, &QThread::finished, capture_worker,
          &QObject::deleteLater);
  connect(&audio_capture_thread_, &QThread::finished, sdr_worker,
          &QObject::deleteLater);
  connect(&audio_dsp_thread_, &QThread::finished, dsp_worker,
          &QObject::deleteLater);
  connect(this, &ReplayController::liveStartRequested, capture_worker,
          &LiveAudioCaptureWorker::start);
  connect(this, &ReplayController::liveStopRequested, capture_worker,
          &LiveAudioCaptureWorker::stop);
  connect(this, &ReplayController::sdrStartRequested, sdr_worker,
          &SdrCaptureWorker::start);
  connect(this, &ReplayController::sdrStopRequested, sdr_worker,
          &SdrCaptureWorker::stop);
  connect(this, &ReplayController::sdrRetuneRequested, sdr_worker,
          &SdrCaptureWorker::requestRetune);
  connect(this, &ReplayController::liveDspStartRequested, dsp_worker,
          &LiveAudioDspWorker::start);
  connect(this, &ReplayController::liveDspStopRequested, dsp_worker,
          &LiveAudioDspWorker::stop);
  connect(this, &ReplayController::liveDspConfigureRequested, dsp_worker,
          &LiveAudioDspWorker::configure);
  connect(this, &ReplayController::liveDecodedSignalTimeoutRequested,
          dsp_worker, &LiveAudioDspWorker::setDecodedSignalTimeoutSeconds);
  connect(this, &ReplayController::liveWeakSignalDecodingRequested, dsp_worker,
          &LiveAudioDspWorker::setWeakSignalDecoding);
  connect(this, &ReplayController::liveOwnCallsignRequested, dsp_worker,
          &LiveAudioDspWorker::setOwnCallsign);
  connect(this, &ReplayController::liveKeyingModelRequested, dsp_worker,
          &LiveAudioDspWorker::setKeyingModel);
  connect(this, &ReplayController::liveDebugCaptureMaximumSecondsRequested,
          dsp_worker, &LiveAudioDspWorker::setDebugCaptureMaximumSeconds);
  connect(this, &ReplayController::liveOperatorRoleRequested, dsp_worker,
          &LiveAudioDspWorker::setOperatorRole);
  connect(this, &ReplayController::liveMonitorConfigureRequested, dsp_worker,
          &LiveAudioDspWorker::setMonitor);
  connect(this, &ReplayController::liveRemoteAudioSubscribedRequested,
          dsp_worker, &LiveAudioDspWorker::setRemoteAudioSubscribed);
  connect(this, &ReplayController::liveSdrDecoderWindowRequested, dsp_worker,
          &LiveAudioDspWorker::setSdrDecoderWindow);
  connect(this, &ReplayController::liveCharacterFrontendEnabledRequested,
          dsp_worker, &LiveAudioDspWorker::setLocalCharacterFrontendEnabled);
  connect(this, &ReplayController::liveCharacterRefinementRequested,
          dsp_worker, &LiveAudioDspWorker::acceptCharacterRefinement);
  connect(dsp_worker, &LiveAudioDspWorker::characterWindowProduced,
          character_worker, &LocalCharacterInferenceWorker::submit,
          Qt::DirectConnection);
  connect(this, &ReplayController::liveFrequencyShiftRequested, dsp_worker,
          &LiveAudioDspWorker::shiftTrackedFrequencies);
  connect(this, &ReplayController::liveManualDecoderFrequencyRequested,
          dsp_worker, &LiveAudioDspWorker::selectDecoderFrequency);
  connect(this, &ReplayController::liveRadioFrequencyContextRequested,
          dsp_worker, &LiveAudioDspWorker::setRadioFrequencyContext);
  connect(this, &ReplayController::liveSdrCaptureContextRequested, dsp_worker,
          &LiveAudioDspWorker::setSdrCaptureContext);
  connect(this, &ReplayController::liveDebugCaptureStartRequested, dsp_worker,
          &LiveAudioDspWorker::startDebugCapture);
  connect(this, &ReplayController::liveDebugCaptureStopRequested, dsp_worker,
          &LiveAudioDspWorker::stopDebugCapture);
  connect(this, &ReplayController::livePresentationDiagnosticsRequested,
          dsp_worker, &LiveAudioDspWorker::setPresentationDiagnostics);
  // Queued, like every other setting sent to this worker. A heartbeat that
  // cannot be delivered is not a lost measurement: the delay shows up as
  // lateness on the following fire, and as a growing gap in the record until
  // one arrives.
  connect(this, &ReplayController::liveGuiHeartbeatRequested, dsp_worker,
          &LiveAudioDspWorker::acceptGuiHeartbeat);
  connect(dsp_worker, &LiveAudioDspWorker::debugCaptureStateChanged, this,
          [this](const bool active, const QString& path,
                 const double elapsed_seconds, const QString& note) {
            debug_capture_active_ = active;
            debug_capture_path_ = path;
            debug_capture_elapsed_seconds_ = elapsed_seconds;
            debug_capture_note_ = note;
            emit debugCaptureChanged();
            if (!active) {
              setStatus(QStringLiteral("Debug capture: %1 (%2)")
                            .arg(note, path));
            }
          });
  connect(capture_worker, &LiveAudioCaptureWorker::started, this,
          [this](const QString& name, const double sample_rate,
                 const int channel_count) {
            source_name_ = name;
            sample_rate_ = sample_rate;
            duration_seconds_ = 0.0;
            position_seconds_ = 0.0;
            input_overruns_ = 0;
            live_capturing_ = true;
            rebuildDecoderModels();
            blocking_error_.clear();
            status_text_ =
                QStringLiteral("Live RX: %1 • %2 Hz • %3 channel(s)")
                    .arg(name)
                    .arg(sample_rate, 0, 'f', 0)
                    .arg(channel_count);
            // A recovery is not an error, so it must not block; but it must
            // not vanish either. The live-RX summary is written the instant
            // capture begins and would otherwise erase the only sentence that
            // says the application followed a name rather than the hardware.
            if (!audio_input_recovery_notice_.isEmpty()) {
              status_text_ += QStringLiteral(" • ") +
                              audio_input_recovery_notice_;
            }
            emit stateChanged();
          });
  connect(capture_worker, &LiveAudioCaptureWorker::inputRecovered, this,
          [this](const QString& adopted_id, const QString& message) {
            audio_input_recovery_notice_ = message;
            // Adopt the identifier here first. Settings echoes the change back
            // through setAudioInputSelection, and a selection that still
            // looked different would restart the capture that just started.
            audio_input_id_ = adopted_id;
            emit audioInputRecovered(adopted_id);
          });
  connect(capture_worker, &LiveAudioCaptureWorker::stopped, this, [this] {
    if (live_capturing_ && source_mode_ == 0) {
      live_capturing_ = false;
      rebuildDecoderModels();
      status_text_ = QStringLiteral("Live audio stopped");
      emit stateChanged();
    }
  });
  connect(capture_worker, &LiveAudioCaptureWorker::failed, this,
          [this](const QString& message) {
            live_capturing_ = false;
            rebuildDecoderModels();
            emit liveDspStopRequested();
            setBlockingError(
                QStringLiteral("Live audio error: %1").arg(message));
          });
  connect(capture_worker, &LiveAudioCaptureWorker::overrunCountChanged, this,
          [this](const qulonglong count) {
            input_overruns_ = count;
            emit stateChanged();
          });
  connect(sdr_worker, &SdrCaptureWorker::started, this,
          [this](const QString&, const double center_frequency_hz,
                 const double sample_rate_hz, const double bandwidth_hz,
                 const bool automatic_gain, const double gain_db) {
            // Forward the gain the hardware actually applied, not the value
            // that was requested, so a later IQ capture documents the real
            // front-end state.
            emit liveSdrCaptureContextRequested(
                sdr_device_name_, sdr_antenna_, automatic_gain, gain_db,
                static_cast<double>(sdr_center_frequency_hz_),
                static_cast<double>(sdr_sample_rate_hz_));
            source_name_ = sdr_device_name_;
            sample_rate_ = sample_rate_hz;
            duration_seconds_ = 0.0;
            position_seconds_ = 0.0;
            input_overruns_ = 0;
            live_capturing_ = true;
            rebuildDecoderModels();
            blocking_error_.clear();
            status_text_ = QStringLiteral(
                "Live SDR: %1 • center %2 Hz • %3 S/s • %4 Hz RF • %5")
                .arg(source_name_)
                .arg(center_frequency_hz, 0, 'f', 0)
                .arg(sample_rate_hz, 0, 'f', 0)
                .arg(bandwidth_hz, 0, 'f', 0)
                .arg(automatic_gain
                         ? QStringLiteral("AGC")
                         : QStringLiteral("%1 dB gain").arg(gain_db, 0, 'f', 1));
            emit stateChanged();
          });
  connect(sdr_worker, &SdrCaptureWorker::stopped, this, [this] {
    if (live_capturing_ && source_mode_ == 2) {
      live_capturing_ = false;
      rebuildDecoderModels();
      status_text_ = QStringLiteral("Live SDR stopped");
      emit stateChanged();
    }
  });
  connect(sdr_worker, &SdrCaptureWorker::retuned, this,
          [this](const double center_frequency_hz) {
            status_text_ = QStringLiteral("Live SDR retuned to %1 Hz")
                               .arg(center_frequency_hz, 0, 'f', 0);
            emit stateChanged();
          });
  connect(sdr_worker, &SdrCaptureWorker::retuneFailed, this,
          [this](const QString& message) {
            setStatus(QStringLiteral("Live SDR retune error: %1").arg(message));
          });
  connect(sdr_worker, &SdrCaptureWorker::failed, this,
          [this](const QString& message) {
            live_capturing_ = false;
            rebuildDecoderModels();
            emit liveDspStopRequested();
            setBlockingError(
                QStringLiteral("Live SDR error: %1").arg(message));
          });
  connect(sdr_worker, &SdrCaptureWorker::diagnosticsChanged, this,
          [this](const qulonglong source_overruns,
                 const qulonglong device_overflows,
                 const qulonglong read_timeouts,
                 const qulonglong read_errors) {
            input_overruns_ = source_overruns + device_overflows;
            if (read_errors > 0) {
              status_text_ = QStringLiteral(
                  "SDR read errors: %1 • device overflows: %2 • timeouts: %3")
                  .arg(read_errors).arg(device_overflows).arg(read_timeouts);
            }
            emit stateChanged();
          });
  // Acknowledged as it is taken, so the worker can tell whether the display is
  // keeping up. Without this the in-flight count only ever rises and the bound
  // would stop the spectrum dead after two frames; with it, frames are dropped
  // only while the thread that draws is actually behind.
  connect(dsp_worker, &LiveAudioDspWorker::frameProduced, this,
          [this, dsp_worker](const SpectrumFrame& frame) {
            emit frameReady(frame);
            dsp_worker->noteSpectrumFrameConsumed();
          });
  connect(dsp_worker, &LiveAudioDspWorker::decoderProduced, this,
          &ReplayController::acceptDecoderChannels);
  connect(dsp_worker, &LiveAudioDspWorker::manualDecoderSelected, this,
          &ReplayController::openDecoderSession);
  connect(dsp_worker, &LiveAudioDspWorker::diagnosticsProduced, this,
          [this](const QVariantMap& diagnostics) {
            verification_diagnostics_ = diagnostics;
            emit decoderChanged();
          });
  // Acknowledged as it is played, exactly as a spectrum frame is acknowledged
  // as it is drawn. Region audio is the one monitor mode whose producer is
  // bounded, and without this the in-flight count only rises: the bound would
  // stop region listening dead after eight buffers instead of only while the
  // thread that plays is actually behind.
  connect(dsp_worker, &LiveAudioDspWorker::monitorAudioProduced, this,
          [this, dsp_worker](const QByteArray& audio,
                             const double sample_rate_hz) {
            writeMonitorAudio(audio, sample_rate_hz);
            dsp_worker->noteMonitorAudioConsumed();
          });
  // Signal to signal and DIRECT, for the reason given on the declaration: the
  // sender at the far end bounds itself, and a queued relay here would put an
  // unbounded queue in front of that bound and route it through the GUI thread
  // on the way.
  connect(dsp_worker, &LiveAudioDspWorker::receiveAudioProduced, this,
          &ReplayController::receiveAudioProduced, Qt::DirectConnection);
  // Signal to signal, and deliberately DIRECT, unlike every other relay here.
  //
  // An automatic connection would be queued, because the worker lives on
  // `audio_dsp_thread_`, and the record would be rebuilt on this thread before
  // the application forwarded it. That routes the diagnostics stream through
  // the GUI thread -- so a GUI thread blocked long enough to be worth
  // reporting would also stop the stream that is supposed to report it,
  // exactly when a remote reader most needs a record. Direct means this
  // controller's signal is emitted on the worker's thread and the record never
  // has to touch the GUI thread at all; the receiver decides where it runs
  // (a separate change moves the diagnostics server off this thread, which is
  // the other half of keeping the path clear of it).
  //
  // Safe as a pure relay: the slot side is another signal, the payload is a
  // QJsonObject passed by const reference and copied on write, and nothing on
  // this path reads or mutates controller state.
  connect(dsp_worker, &LiveAudioDspWorker::diagnosticsRecordProduced, this,
          &ReplayController::diagnosticsRecordProduced, Qt::DirectConnection);
  audio_capture_thread_.setObjectName(QStringLiteral("Live receiver capture"));
  audio_dsp_thread_.setObjectName(QStringLiteral("Live audio DSP"));
  audio_capture_thread_.start();
  audio_dsp_thread_.start();
  // Started last, once the worker it reports to exists and is connected.
  gui_heartbeat_clock_.start();
  gui_heartbeat_timer_.start();
}

void ReplayController::publishGuiHeartbeat() {
  // Elapsed since the previous delivery, less the interval that was asked
  // for. On an event loop with room to breathe this is a fraction of a
  // millisecond; it is the time the loop spent unable to reach this timer,
  // which is the same time it spent unable to repaint.
  const qint64 elapsed_ms = gui_heartbeat_clock_.restart();
  const double lateness_ms = static_cast<double>(elapsed_ms) -
                             static_cast<double>(
                                 LiveAudioDspWorker::kGuiHeartbeatIntervalMs);
  emit liveGuiHeartbeatRequested(lateness_ms > 0.0 ? lateness_ms : 0.0);
}

ReplayController::~ReplayController() {
  stopMonitorOutput();
  if (audio_capture_worker_ != nullptr && audio_capture_thread_.isRunning()) {
    QMetaObject::invokeMethod(audio_capture_worker_, "stop",
                              Qt::BlockingQueuedConnection);
  }
  if (audio_dsp_worker_ != nullptr && audio_dsp_thread_.isRunning()) {
    QMetaObject::invokeMethod(audio_dsp_worker_, "stop",
                              Qt::BlockingQueuedConnection);
  }
  if (sdr_capture_worker_ != nullptr && audio_capture_thread_.isRunning()) {
    QMetaObject::invokeMethod(sdr_capture_worker_, "stop",
                              Qt::BlockingQueuedConnection);
  }
  audio_capture_thread_.quit();
  audio_dsp_thread_.quit();
  audio_capture_thread_.wait();
  audio_dsp_thread_.wait();
  if (worker_ != nullptr && worker_thread_.isRunning()) {
    QMetaObject::invokeMethod(worker_, "shutdown", Qt::BlockingQueuedConnection);
  }
  worker_thread_.quit();
  worker_thread_.wait();
  emit localCharacterResetRequested();
  character_inference_thread_.quit();
  character_inference_thread_.wait();
}

const QString& ReplayController::sourceName() const noexcept {
  return source_name_;
}
const QString& ReplayController::statusText() const noexcept {
  return status_text_;
}
bool ReplayController::sourceLoaded() const noexcept { return source_loaded_; }
bool ReplayController::playing() const noexcept { return playing_; }
int ReplayController::sourceMode() const noexcept { return source_mode_; }
bool ReplayController::liveCapturing() const noexcept {
  return live_capturing_;
}
bool ReplayController::activeSource() const noexcept {
  return source_mode_ == 1 ? source_loaded_ : live_capturing_;
}
qulonglong ReplayController::inputOverruns() const noexcept {
  return input_overruns_;
}
double ReplayController::sampleRate() const noexcept { return sample_rate_; }
double ReplayController::durationSeconds() const noexcept {
  return duration_seconds_;
}
double ReplayController::positionSeconds() const noexcept {
  return position_seconds_;
}
int ReplayController::averagingFrames() const noexcept {
  return averaging_frames_;
}
const QVariantList& ReplayController::decoderChannels() const noexcept {
  return decoder_channels_;
}
int ReplayController::decoderChannelCount() const noexcept {
  return static_cast<int>(std::count_if(
      decoder_channels_.cbegin(), decoder_channels_.cend(),
      [](const QVariant& value) {
        return value.toMap().value(QStringLiteral("verifiedCw")).toBool();
      }));
}
const QVariantList& ReplayController::decoderSessions() const noexcept {
  return decoder_sessions_;
}
QAbstractItemModel* ReplayController::decoderSessionModel() noexcept {
  return &decoder_session_model_;
}
int ReplayController::decoderSessionCount() const noexcept {
  return static_cast<int>(decoder_sessions_.size());
}
const QVariantMap& ReplayController::verificationDiagnostics() const noexcept {
  return verification_diagnostics_;
}
const QString& ReplayController::localCharacterState() const noexcept {
  return local_character_state_;
}
const QString& ReplayController::localCharacterStatus() const noexcept {
  return local_character_status_;
}
void ReplayController::setCallsignDatabaseCorrectionEnabled(const bool value) {
  callsign_database_correction_enabled_ = value;
}

const QString& ReplayController::offlineCallsignDatabaseState() const noexcept {
  return offline_callsign_database_state_;
}
const QString& ReplayController::offlineCallsignDatabaseStatus() const noexcept {
  return offline_callsign_database_status_;
}
int ReplayController::offlineCallsignDatabaseEntries() const noexcept {
  return static_cast<int>(std::min<std::size_t>(
      offline_callsign_database_.size(),
      static_cast<std::size_t>(std::numeric_limits<int>::max())));
}
bool ReplayController::debugCaptureActive() const noexcept {
  return debug_capture_active_;
}
const QString& ReplayController::debugCapturePath() const noexcept {
  return debug_capture_path_;
}
double ReplayController::debugCaptureElapsedSeconds() const noexcept {
  return debug_capture_elapsed_seconds_;
}
const QString& ReplayController::debugCaptureNote() const noexcept {
  return debug_capture_note_;
}
bool ReplayController::radioFrequencyAvailable() const noexcept {
  return radio_frequency_available_;
}
qulonglong ReplayController::radioRxFrequencyHz() const noexcept {
  return radio_rx_rf_hz_;
}
qulonglong ReplayController::radioTxFrequencyHz() const noexcept {
  return radio_tx_rf_hz_;
}
bool ReplayController::radioSplitActive() const noexcept {
  return radio_split_active_;
}
int ReplayController::monitorMode() const noexcept { return monitor_mode_; }
qulonglong ReplayController::monitoredChannelId() const noexcept {
  return monitored_channel_ids_.isEmpty() ? 0U
                                          : monitored_channel_ids_.front();
}
QVariantList ReplayController::monitoredChannelIds() const {
  QVariantList result;
  result.reserve(monitored_channel_ids_.size());
  for (const qulonglong id : monitored_channel_ids_) result.push_back(id);
  return result;
}
const QString& ReplayController::monitorStatus() const noexcept {
  return monitor_status_;
}
double ReplayController::monitorLevel() const noexcept {
  return monitor_level_;
}

void ReplayController::setAveragingFrames(const int value) {
  const int clamped = std::clamp(value, 1, 32);
  if (averaging_frames_ == clamped) {
    return;
  }
  averaging_frames_ = clamped;
  emit averagingFramesChanged();
  publishSpectrumConfiguration();
}

void ReplayController::configureLocalCharacterDecoder(
    const bool enabled, const QString& model_path,
    const QString& metadata_path) {
  emit replayCharacterFrontendEnabledRequested(false);
  emit liveCharacterFrontendEnabledRequested(false);
  emit localCharacterResetRequested();
  local_character_consensus_.clear();
  // Enabled without both files selected is not an error, it is an unfinished
  // setup, and it must not be reported as one. Loading was attempted anyway,
  // and an empty path fails the metadata check as though the file were the
  // wrong kind or too large: the card showed "metadata must be a regular JSON
  // file no larger than 64 KiB" for a model the operator had never chosen.
  const bool configured =
      !model_path.trimmed().isEmpty() && !metadata_path.trimmed().isEmpty();
  if (enabled && !configured) {
    local_character_state_ = QStringLiteral("unconfigured");
    local_character_status_ = QStringLiteral(
        "Select a model file and its metadata under Settings, Decoder. "
        "Deterministic decoding continues meanwhile.");
    rebuildDecoderModels();
    emit localCharacterDecoderConfigureRequested(false, QString{}, QString{});
    return;
  }
  local_character_state_ = enabled ? QStringLiteral("loading")
                                   : QStringLiteral("disabled");
  local_character_status_ = enabled
      ? QStringLiteral("Loading and validating the local model…")
      : QStringLiteral("Local model disabled.");
  rebuildDecoderModels();
  emit localCharacterDecoderConfigureRequested(enabled, model_path,
                                                metadata_path);
}

void ReplayController::configureOfflineCallsignDatabase(
    const bool enabled, const QString& database_path) {
  offline_callsign_database_.clear();
  // Every retained suggestion was derived against the directory that is about
  // to be replaced, so none of them are answers to the current question.
  channel_presentation_.invalidate();
  if (!enabled) {
    offline_callsign_database_state_ = QStringLiteral("disabled");
    offline_callsign_database_status_ =
        QStringLiteral("Offline callsign suggestions disabled.");
    rebuildDecoderModels();
    return;
  }

  if (database_path.isEmpty()) {
    offline_callsign_database_state_ = QStringLiteral("empty");
    offline_callsign_database_status_ =
        QStringLiteral("Select a local master.scp or Call History text file.");
    rebuildDecoderModels();
    return;
  }

  QFile file(database_path);
  constexpr qint64 maximum_bytes = 32LL * 1'024LL * 1'024LL;
  if (!file.exists() || file.size() <= 0 ||
      file.size() > maximum_bytes || !file.open(QIODevice::ReadOnly)) {
    offline_callsign_database_state_ = QStringLiteral("error");
    offline_callsign_database_status_ = QStringLiteral(
        "The selected file is missing, unreadable, empty, or larger than 32 MiB.");
    rebuildDecoderModels();
    return;
  }
  const QByteArray contents = file.readAll();
  if (file.error() != QFileDevice::NoError) {
    offline_callsign_database_state_ = QStringLiteral("error");
    offline_callsign_database_status_ = QStringLiteral(
        "The selected local callsign-list file could not be read completely.");
    rebuildDecoderModels();
    return;
  }
  const auto result = offline_callsign_database_.importText(std::string_view(
      contents.constData(), static_cast<std::size_t>(contents.size())));
  if (!result.accepted || result.inserted_records == 0U) {
    offline_callsign_database_.clear();
    offline_callsign_database_state_ = QStringLiteral("error");
    offline_callsign_database_status_ = !result.accepted
        ? QStringLiteral("The selected file exceeds the bounded local-list import limits.")
        : QStringLiteral("No structurally plausible callsign records were found in the selected text file.");
    rebuildDecoderModels();
    return;
  }
  offline_callsign_database_state_ = QStringLiteral("ready");
  offline_callsign_database_status_ = QStringLiteral(
      "%1 unique local record(s) loaded; suggestions remain separate from decoded text.")
                                          .arg(result.inserted_records);
  rebuildDecoderModels();
}

void ReplayController::setSpectrumProcessing(
    const bool dc_rejection, const bool automatic_gain, const double gain_db,
    const double automatic_gain_target_dbfs, const bool automatic_bandwidth,
    const double lower_frequency_hz, const double upper_frequency_hz,
    const int frame_rate_hz) {
  const double clamped_gain_db = std::clamp(gain_db, -40.0, 40.0);
  const double clamped_target_dbfs =
      std::clamp(automatic_gain_target_dbfs, -40.0, -1.0);
  const double clamped_lower_hz =
      std::clamp(lower_frequency_hz, 0.0, 95'999.0);
  const double clamped_upper_hz =
      std::clamp(upper_frequency_hz, clamped_lower_hz + 1.0, 96'000.0);
  const int clamped_frame_rate_hz = std::clamp(frame_rate_hz, 1, 120);
  if (spectrum_processing_configured_ &&
      audio_dc_rejection_ == dc_rejection &&
      audio_automatic_gain_ == automatic_gain &&
      audio_gain_db_ == clamped_gain_db &&
      audio_automatic_gain_target_dbfs_ == clamped_target_dbfs &&
      audio_automatic_bandwidth_ == automatic_bandwidth &&
      audio_lower_frequency_hz_ == clamped_lower_hz &&
      audio_upper_frequency_hz_ == clamped_upper_hz &&
      spectrum_frame_rate_hz_ == clamped_frame_rate_hz) {
    return;
  }
  const bool decoder_passband_changed =
      spectrum_processing_configured_ &&
      (audio_automatic_bandwidth_ != automatic_bandwidth ||
       audio_lower_frequency_hz_ != clamped_lower_hz ||
       audio_upper_frequency_hz_ != clamped_upper_hz);
  audio_dc_rejection_ = dc_rejection;
  audio_automatic_gain_ = automatic_gain;
  audio_gain_db_ = clamped_gain_db;
  audio_automatic_gain_target_dbfs_ = clamped_target_dbfs;
  audio_automatic_bandwidth_ = automatic_bandwidth;
  audio_lower_frequency_hz_ = clamped_lower_hz;
  audio_upper_frequency_hz_ = clamped_upper_hz;
  spectrum_frame_rate_hz_ = clamped_frame_rate_hz;
  spectrum_processing_configured_ = true;
  if (decoder_passband_changed) resetDecoder();
  publishSpectrumConfiguration();
}

void ReplayController::publishSpectrumConfiguration() {
  emit configureRequested(
      averaging_frames_, spectrum_frame_rate_hz_, audio_dc_rejection_,
      audio_automatic_gain_,
      audio_gain_db_, audio_automatic_gain_target_dbfs_,
      audio_automatic_bandwidth_, audio_lower_frequency_hz_,
      audio_upper_frequency_hz_);
  emit liveDspConfigureRequested(
      averaging_frames_, spectrum_frame_rate_hz_, audio_dc_rejection_,
      audio_automatic_gain_,
      audio_gain_db_, audio_automatic_gain_target_dbfs_,
      audio_automatic_bandwidth_, audio_lower_frequency_hz_,
      audio_upper_frequency_hz_);
}

void ReplayController::publishSdrDecoderWindow() {
  // IqSubbandDecimator::process() refuses a block unless
  //   |decoder centre - capture centre| + decoder bandwidth / 2 < Nyquist,
  // and drain() publishes the wide overview spectrum *before* consulting the
  // decimator. A window left outside the current capture therefore produces
  // the exact fault an operator cannot diagnose: signals painting normally on
  // the spectrum and waterfall while not one sample ever reaches detection.
  //
  // The stored window defaults to 14.050 MHz and only ever moved when the
  // operator dragged a selection in the zoomed view, so every receiver tuned
  // away from 20 m decoded nothing at all.
  const auto capture_center = static_cast<double>(sdr_center_frequency_hz_);
  const auto bandwidth = static_cast<double>(sdr_decoder_bandwidth_hz_);
  // The same rule the LO-offset clamp uses, read from the same definition.
  // Two spellings of one margin was the defect underneath this function: the
  // settings would place the window exactly a kilohertz beyond what this test
  // accepted, so a configured LO offset large enough to be clamped landed
  // here as "out of reach" and the window was moved onto the acquisition
  // centre -- which is the operator's frequency plus the LO offset, so every
  // reported frequency carried that offset twice.
  const double reach = sdrDecoderWindowReachHz(
      static_cast<double>(sdr_sample_rate_hz_), bandwidth);
  auto center = static_cast<double>(sdr_decoder_center_frequency_hz_);
  const double offset_from_capture_hz = center - capture_center;
  if (reach <= 0.0 ||
      std::abs(offset_from_capture_hz) >
          static_cast<double>(sdr_sample_rate_hz_) * 0.5) {
    // Not a window that overhangs the margin: one that is not in the acquired
    // passband at all, such as a stored 20 m default against a receiver on
    // 40 m. Nothing about it can be salvaged, so fall back to the centre of
    // what is actually being received.
    center = capture_center;
    sdr_decoder_center_frequency_hz_ =
        static_cast<qulonglong>(std::llround(center));
  } else if (std::abs(offset_from_capture_hz) > reach) {
    // Merely overhanging -- a drag that widened the window past the margin,
    // for instance. Pull it just inside instead of discarding where the
    // operator is listening, which is what moving it to the capture centre
    // amounts to.
    center = std::max(
        1.0, capture_center + std::clamp(offset_from_capture_hz, -reach, reach));
    sdr_decoder_center_frequency_hz_ =
        static_cast<qulonglong>(std::llround(center));
  }
  emit liveSdrDecoderWindowRequested(center, bandwidth);
  // The decoder window is where an SDR operator is listening, so the cluster
  // band filter and the spot model both follow it.
  publishDxClusterBandFilter();
  rebuildDxSpotModel();
}

void ReplayController::acceptDecoderChannels(const QVariantList& channels) {
  raw_decoder_channels_ = channels;
  rebuildDecoderModels();
}

void ReplayController::rebuildDecoderModels() {
  const QVariantList previous_sessions = decoder_sessions_;
  channel_presentation_.setContext(own_callsign_,
                                   callsign_database_correction_enabled_);
  decoder_channels_.clear();
  QHash<qulonglong, QVariantMap> by_id;
  const bool mapped_audio_rf = radio_frequency_available_ &&
                               source_mode_ == 0 && live_capturing_;
  const bool direct_iq_rf = source_mode_ == 2 && live_capturing_;
  for (const QVariant& value : raw_decoder_channels_) {
    QVariantMap item = value.toMap();
    const auto id = item.value(QStringLiteral("id")).toULongLong();
    const double tracked_audio_hz =
        item.value(QStringLiteral("frequencyHz")).toDouble();
    const QVariant presentation =
        item.value(QStringLiteral("presentationFrequencyHz"));
    const double audio_hz = presentation.isValid()
        ? presentation.toDouble() : tracked_audio_hz;
    item.insert(QStringLiteral("audioFrequencyHz"), audio_hz);
    item.insert(QStringLiteral("trackedAudioFrequencyHz"), tracked_audio_hz);
    if (direct_iq_rf) {
      const auto rf_hz = static_cast<qulonglong>(std::llround(audio_hz));
      item.insert(QStringLiteral("displayFrequencyHz"),
                  QVariant::fromValue<qulonglong>(rf_hz));
      item.insert(QStringLiteral("frequencyKind"), QStringLiteral("RF"));
      item.insert(QStringLiteral("frequencyLabel"),
                  QStringLiteral("%1 Hz RF").arg(rf_hz));
    } else if (mapped_audio_rf) {
      const auto resolved_rf = cwassistant::core::resolve_audio_tone_rf(
          radio_rx_rf_hz_, audio_hz, cw_reference_tone_hz_,
          cw_sideband_index_ == 0);
      if (resolved_rf) {
        const auto rf_hz = static_cast<qulonglong>(*resolved_rf);
        item.insert(QStringLiteral("displayFrequencyHz"),
                    QVariant::fromValue<qulonglong>(rf_hz));
        item.insert(QStringLiteral("frequencyKind"), QStringLiteral("RF"));
        item.insert(QStringLiteral("frequencyLabel"),
                    QStringLiteral("%1 Hz RF").arg(rf_hz));
      }
    }
    if (!item.contains(QStringLiteral("frequencyLabel"))) {
      item.insert(QStringLiteral("displayFrequencyHz"), audio_hz);
      item.insert(QStringLiteral("frequencyKind"), QStringLiteral("AF"));
      item.insert(QStringLiteral("frequencyLabel"),
                  QStringLiteral("%1 Hz AF").arg(audio_hz, 0, 'f', 0));
    }
    item.insert(QStringLiteral("sessionOpen"),
                decoder_session_order_.contains(id));
    item.insert(QStringLiteral("localModelState"), local_character_state_);
    item.insert(QStringLiteral("localModelStatus"), local_character_status_);
    const auto local = local_character_consensus_.find(id);
    if (item.value(QStringLiteral("verifiedCw")).toBool() &&
        local != local_character_consensus_.end()) {
      const auto& stable_text = local->second.stableText();
      item.insert(QStringLiteral("localModelText"),
                  QString::fromStdString(stable_text));
      const auto suggested_callsign =
          cwassistant::core::CallsignPolicy::latest_in_text(stable_text);
      item.insert(QStringLiteral("localModelCallsign"),
                  suggested_callsign
                      ? QString::fromStdString(*suggested_callsign)
                      : QString{});
    } else {
      item.insert(QStringLiteral("localModelText"), QString{});
      item.insert(QStringLiteral("localModelCallsign"), QString{});
    }

    // The own-callsign scan and the advisory suggestion both derive from this
    // stream's transcripts, its confirmed callsign and its acoustic
    // alternatives, and nothing else. Deriving them here, on the thread that
    // draws, for every stream on every publish, is what blocked the GUI event
    // loop for up to 2.1 seconds on a busy band. The cache returns the same
    // values from the same evidence and recomputes only when the evidence
    // moves.
    const auto& derived = channel_presentation_.derive(
        id, item, offline_callsign_database_);
    item.insert(QStringLiteral("callsignInDatabase"),
                derived.callsign_in_database);
    item.insert(QStringLiteral("callsignDatabaseLoaded"),
                offline_callsign_database_.size() > 0U);
    item.insert(QStringLiteral("callingOwnStation"),
                derived.calling_own_station);
    item.insert(QStringLiteral("callsignSuggestion"), derived.suggestion);
    item.insert(QStringLiteral("callsignSuggestionRawSpan"),
                derived.suggestion_raw_span);
    item.insert(QStringLiteral("callsignSuggestionSource"),
                derived.suggestion_source);
    item.insert(QStringLiteral("callsignSuggestionAgreeingAlternatives"),
                derived.suggestion_agreeing_alternatives);
    item.insert(QStringLiteral("callsignSuggestionSupport"),
                derived.suggestion_support);
    item.insert(QStringLiteral("callsignSuggestionRelativeCost"),
                derived.suggestion_relative_cost);
    item.insert(QStringLiteral("callsignSuggestionDiagnostic"),
                derived.suggestion_diagnostic);
    decoder_channels_.push_back(item);
    by_id.insert(id, item);
  }
  channel_presentation_.endPublish();
  decoder_session_order_ = reconcileDecoderSessionOrder(
      decoder_session_order_, previous_sessions, decoder_channels_);
  for (QVariant& value : decoder_channels_) {
    QVariantMap item = value.toMap();
    item.insert(QStringLiteral("sessionOpen"),
                decoder_session_order_.contains(
                    item.value(QStringLiteral("id")).toULongLong()));
    value = item;
    by_id.insert(item.value(QStringLiteral("id")).toULongLong(), item);
  }
  decoder_sessions_.clear();
  for (const auto id : decoder_session_order_) {
    auto item = by_id.value(id);
    item.insert(QStringLiteral("sessionOpen"), true);
    decoder_sessions_.push_back(item);
  }
  decoder_session_model_.replace(decoder_sessions_);
  if (monitor_mode_ == 2 && !monitored_channel_ids_.isEmpty()) {
    const QList<qulonglong> reconciled_monitor = reconcileDecoderSessionOrder(
        monitored_channel_ids_, previous_sessions, decoder_channels_);
    if (reconciled_monitor != monitored_channel_ids_) {
      monitored_channel_ids_ = reconciled_monitor;
      stopMonitorOutput();
      if (monitored_channel_ids_.isEmpty()) {
        monitor_mode_ = 0;
        monitor_status_ = QStringLiteral("Monitor off");
      } else {
        monitor_status_ = monitored_channel_ids_.size() == 1
            ? QStringLiteral("Monitoring reacquired stream")
            : QStringLiteral("Monitoring %1 streams")
                  .arg(monitored_channel_ids_.size());
      }
      publishMonitorConfiguration();
      emit monitorChanged();
    }
  }
  publishLivePresentationDiagnostics(false);
  emit decoderChanged();
}

void ReplayController::publishLivePresentationDiagnostics(const bool force) {
  if (!force && !debug_capture_active_) return;

  QVariantList presentation_channels;
  presentation_channels.reserve(decoder_channels_.size());
  for (const QVariant& value : decoder_channels_) {
    const QVariantMap channel = value.toMap();
    presentation_channels.push_back(QVariantMap{
        {QStringLiteral("id"), channel.value(QStringLiteral("id"))},
        {QStringLiteral("callsign"),
         channel.value(QStringLiteral("callsign"))},
        {QStringLiteral("callsignInDatabase"),
         channel.value(QStringLiteral("callsignInDatabase"))},
        {QStringLiteral("callingOwnStation"),
         channel.value(QStringLiteral("callingOwnStation"))},
        {QStringLiteral("callsignDatabaseLoaded"),
         channel.value(QStringLiteral("callsignDatabaseLoaded"))},
        {QStringLiteral("callsignSuggestion"),
         channel.value(QStringLiteral("callsignSuggestion"))},
        {QStringLiteral("callsignSuggestionRawSpan"),
         channel.value(QStringLiteral("callsignSuggestionRawSpan"))},
        {QStringLiteral("callsignSuggestionSource"),
         channel.value(QStringLiteral("callsignSuggestionSource"))},
        {QStringLiteral("callsignSuggestionAgreeingAlternatives"),
         channel.value(QStringLiteral("callsignSuggestionAgreeingAlternatives"))},
        {QStringLiteral("callsignSuggestionSupport"),
         channel.value(QStringLiteral("callsignSuggestionSupport"))},
        {QStringLiteral("callsignSuggestionRelativeCost"),
         channel.value(QStringLiteral("callsignSuggestionRelativeCost"))},
        {QStringLiteral("callsignSuggestionDiagnostic"),
         channel.value(QStringLiteral("callsignSuggestionDiagnostic"))},
    });
  }
  const QVariantMap presentation_diagnostics{
      {QStringLiteral("offlineCallsignDatabaseState"),
       offline_callsign_database_state_},
      {QStringLiteral("offlineCallsignDatabaseEntries"),
       static_cast<qulonglong>(offline_callsign_database_.size())},
      {QStringLiteral("localCharacterModelState"), local_character_state_},
      {QStringLiteral("channels"), presentation_channels},
  };
  if (presentation_diagnostics != last_live_presentation_diagnostics_) {
    last_live_presentation_diagnostics_ = presentation_diagnostics;
    emit livePresentationDiagnosticsRequested(presentation_diagnostics);
  }
}

void ReplayController::setRadioFrequencyContext(
    const bool available, const qulonglong rx_rf_hz,
    const qulonglong tx_rf_hz, const bool split_active,
    const int sideband_index, const double reference_tone_hz) {
  const int sideband = std::clamp(sideband_index, 0, 1);
  const double reference = std::clamp(reference_tone_hz, 0.0, 96'000.0);
  if (radio_frequency_available_ == available &&
      radio_rx_rf_hz_ == rx_rf_hz && radio_tx_rf_hz_ == tx_rf_hz &&
      radio_split_active_ == split_active &&
      cw_sideband_index_ == sideband && cw_reference_tone_hz_ == reference) {
    return;
  }
  // A retune while already linked (not the initial link-up, and not a live
  // audio source) shifts every currently tracked signal's audio frequency
  // by the same amount the RX dial moved, so identified signals keep their
  // identity across the retune instead of being lost and re-acquired.
  // What matters is that a previous frequency is known and the dial has moved,
  // not that the radio was continuously readable in between. Requiring the
  // previous report to have been available meant a momentary gap -- which a
  // polled radio produces while it is busy retuning -- silently swallowed the
  // move that followed it. The tracks then stayed at the old audio frequency
  // while the signal moved away, so an identified stream was lost and
  // re-acquired as a new one a moment later.
  if (source_mode_ == 0 && available && radio_rx_rf_hz_ != 0U &&
      radio_rx_rf_hz_ != rx_rf_hz) {
    const double delta_rf_hz =
        static_cast<double>(rx_rf_hz) - static_cast<double>(radio_rx_rf_hz_);
    const double delta_audio_hz = sideband == 0 ? -delta_rf_hz : delta_rf_hz;
    emit liveFrequencyShiftRequested(delta_audio_hz);
  }
  radio_frequency_available_ = available;
  radio_rx_rf_hz_ = rx_rf_hz;
  radio_tx_rf_hz_ = tx_rf_hz;
  radio_split_active_ = split_active;
  cw_sideband_index_ = sideband;
  cw_reference_tone_hz_ = reference;
  emit liveRadioFrequencyContextRequested(radio_frequency_available_,
                                          radio_rx_rf_hz_, radio_tx_rf_hz_,
                                          radio_split_active_);
  emit radioFrequencyChanged();
  rebuildDecoderModels();
  // The cluster filter follows the radio: a node that accepts filter commands
  // is told the new band without the link being dropped and rebuilt.
  publishDxClusterBandFilter();
  rebuildDxSpotModel();
}

double ReplayController::rfFrequencyToDisplayHz(
    const qulonglong rf_frequency_hz) const noexcept {
  if (rf_frequency_hz == 0U) return std::numeric_limits<double>::quiet_NaN();
  if (source_mode_ == 2) return static_cast<double>(rf_frequency_hz);
  if (source_mode_ != 0 || !radio_frequency_available_ ||
      radio_rx_rf_hz_ == 0U) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double delta_rf_hz = static_cast<double>(rf_frequency_hz) -
                             static_cast<double>(radio_rx_rf_hz_);
  const double audio_hz = cw_reference_tone_hz_ +
      (cw_sideband_index_ == 0 ? delta_rf_hz : -delta_rf_hz);
  return std::isfinite(audio_hz) && audio_hz >= 0.0
      ? audio_hz : std::numeric_limits<double>::quiet_NaN();
}

qulonglong ReplayController::displayFrequencyToRfHz(
    const double display_frequency_hz) const noexcept {
  if (!std::isfinite(display_frequency_hz) || display_frequency_hz < 0.0)
    return 0U;
  if (source_mode_ == 2) {
    if (display_frequency_hz > 99'000'000'000.0) return 0U;
    return static_cast<qulonglong>(std::llround(display_frequency_hz));
  }
  if (source_mode_ != 0 || !radio_frequency_available_ ||
      radio_rx_rf_hz_ == 0U) {
    return 0U;
  }
  const auto resolved = cwassistant::core::resolve_audio_tone_rf(
      radio_rx_rf_hz_, display_frequency_hz, cw_reference_tone_hz_,
      cw_sideband_index_ == 0);
  return resolved && *resolved <= 99'000'000'000ULL
      ? static_cast<qulonglong>(*resolved) : 0U;
}

bool ReplayController::axisShowsRf() const noexcept {
  if (source_mode_ == 2) return true;
  return source_mode_ == 0 && radio_frequency_available_ &&
         radio_rx_rf_hz_ != 0U;
}

double ReplayController::axisFrequencyHz(const double axis_hz) const noexcept {
  if (!std::isfinite(axis_hz)) return axis_hz;
  // Direct IQ frames are already described in absolute RF, so the axis is
  // right as it stands.
  if (source_mode_ == 2) return axis_hz;
  const qulonglong rf_hz = displayFrequencyToRfHz(axis_hz);
  // Zero means the mapping could not be made -- no radio, no dial reading, or
  // a point that lands outside the spectrum entirely. Print the audio
  // frequency rather than nothing: a ruler with gaps in it is harder to read
  // than one that admits it is showing the passband.
  return rf_hz == 0U ? axis_hz : static_cast<double>(rf_hz);
}

const QVariantList& ReplayController::dxSpots() const noexcept {
  return dx_spots_;
}

const QString& ReplayController::dxSpotsStatus() const noexcept {
  return dx_spots_status_;
}

bool ReplayController::dxClusterConnected() const noexcept {
  // Not "enabled" and not "configured": connected. The indicator exists to
  // answer whether the link is up this moment, and a client that is retrying a
  // refused server is enabled the whole time it is failing.
  return dx_cluster_enabled_ && dx_cluster_client_ != nullptr &&
         dx_cluster_client_->connected();
}

QString ReplayController::dxClusterStatus() const {
  if (!dx_cluster_enabled_ || dx_cluster_client_ == nullptr) {
    return QStringLiteral("DX cluster is off.");
  }
  return dx_cluster_client_->statusMessage();
}

qulonglong ReplayController::receiveRfHz() const noexcept {
  // The SDR decodes inside a window of its own; that window, not the capture
  // centre, is where the operator is actually listening.
  if (source_mode_ == 2) {
    return sdr_decoder_center_frequency_hz_ != 0U
               ? sdr_decoder_center_frequency_hz_
               : sdr_center_frequency_hz_;
  }
  return radio_frequency_available_ ? radio_rx_rf_hz_ : 0U;
}

bool ReplayController::isSpotWithinReceivedBand(
    const double frequency_hz) const noexcept {
  // Why this exists, so it is not later removed as redundant with the
  // registry's own bounds. The reverse-beacon telnet feed was measured at six
  // spots a second -- the server announced "Spot rate: 6/s (21,998/h)" --
  // worldwide, across every band and mode. The registry deduplicates per
  // source and callsign and holds 4096 entries for fifteen minutes, which
  // collapses twenty skimmers hearing one CQ into one entry but does nothing
  // about the other twenty-three bands: an operator on 20 m would have their
  // store filled with 160 m and 10 m stations they cannot hear, and every
  // arriving spot costs a linear scan of up to 4096 entries to find that out.
  //
  // This is arrival filtering, and it is not the same thing as the band filter
  // pushed to the server in publishDxClusterBandFilter(). Both are needed:
  // the reverse beacon network accepts no filter commands on its telnet port,
  // so its feed can only be narrowed here, while a cluster that does accept
  // them should not be made to send what is going to be discarded anyway.
  const qulonglong rx_rf_hz = receiveRfHz();
  // Nothing known about where the receiver is pointed. Keeping everything is
  // the safe failure: a filter that silently discarded every spot because it
  // could not place the radio would be worse than no filter at all.
  if (rx_rf_hz == 0U) return true;
  if (!std::isfinite(frequency_hz) || frequency_hz <= 0.0) return false;
  const auto receiver_band = cwassistant::core::adif_band_from_frequency(
      static_cast<std::uint64_t>(rx_rf_hz));
  // A receiver sitting outside every amateur band -- a transverter's
  // intermediate frequency, a general-coverage tune -- gives nothing to
  // compare against, so nothing is rejected.
  if (receiver_band.empty()) return true;
  // Deliberately the whole band rather than the visible span. An operator
  // retunes constantly, and a spot 40 kHz away is exactly the one worth
  // showing as a marker to tune towards.
  return cwassistant::core::adif_band_from_frequency(
             static_cast<std::uint64_t>(std::llround(frequency_hz))) ==
         receiver_band;
}

void ReplayController::publishDxSpotsStatus() {
  const QString composed = dx_cluster_client_ != nullptr
                               ? dx_cluster_client_->statusMessage()
                               : QString();
  // dxClusterStateChanged is emitted unconditionally, not only when the text
  // moves: dxClusterConnected can change while the status line does not, and
  // an indicator gated on the string would keep showing a link that has
  // already dropped.
  emit dxClusterStateChanged();
  if (dx_spots_status_ == composed) return;
  dx_spots_status_ = composed;
  emit dxSpotsChanged();
}

void ReplayController::publishDxClusterBandFilter() {
  if (dx_cluster_client_ == nullptr) return;
  const qulonglong rx_rf_hz = receiveRfHz();
  // Empty when the receive frequency is unknown. The client reads that as
  // "unknown" and withholds the band-dependent commands rather than sending a
  // malformed one, so a receiver with no frequency asks the server for
  // everything instead of asking it for nothing.
  const std::string_view band =
      rx_rf_hz == 0U ? std::string_view{}
                     : cwassistant::core::adif_band_from_frequency(
                           static_cast<std::uint64_t>(rx_rf_hz));
  dx_cluster_client_->setBandFilter(
      QString::fromUtf8(band.data(), static_cast<qsizetype>(band.size())));
}

void ReplayController::ensureDxClusterClient() {
  if (dx_cluster_client_ != nullptr) return;
  dx_cluster_client_ = new DxClusterClient(this);
  connect(dx_cluster_client_, &DxClusterClient::spotsReceived, this,
          &ReplayController::acceptDxSpots);
  connect(dx_cluster_client_, &DxClusterClient::stateChanged, this,
          [this] { publishDxSpotsStatus(); });
}

void ReplayController::configureDxCluster(const bool enabled,
                                          const int server_index,
                                          const QString& custom_host,
                                          const int custom_port,
                                          const QString& login_callsign,
                                          const int retention_minutes,
                                          const int tolerance_hz) {
  const bool was_enabled = dx_cluster_enabled_;
  const int bounded_retention_minutes = std::clamp(retention_minutes, 1, 60);
  const int bounded_tolerance_hz = std::clamp(tolerance_hz, 50, 1'000);
  if (bounded_retention_minutes != dx_spots_retention_minutes_ ||
      bounded_tolerance_hz != dx_spots_tolerance_hz_) {
    dx_spots_retention_minutes_ = bounded_retention_minutes;
    dx_spots_tolerance_hz_ = bounded_tolerance_hz;
    // The registry fixes its bounds at construction, so a changed retention
    // window or match tolerance means a new one. What it currently holds is
    // discarded rather than reinterpreted: those spots were admitted under the
    // previous bounds, and a live cluster refills the store within minutes.
    cwassistant::core::CwSpotRegistry::Limits limits;
    limits.retention_ns =
        static_cast<std::uint64_t>(dx_spots_retention_minutes_) * 60ULL *
        1'000'000'000ULL;
    limits.match_tolerance_hz = static_cast<double>(dx_spots_tolerance_hz_);
    dx_spot_registry_ = cwassistant::core::CwSpotRegistry(limits);
  }
  // A cluster cannot be joined anonymously, and inventing a callsign would put
  // a false identity on somebody else's machine. Without one this application
  // will actually send, the link simply stays off.
  const bool joinable =
      enabled && DxClusterClient::isAcceptableLoginCallsign(login_callsign);

  // Read once per process. The server list is file data shared by every
  // profile, and a settings change must not re-read it from disk.
  static const DxClusterServerList kServers = DxClusterServerList::load();

  DxClusterServer server;
  if (joinable) {
    const auto& servers = kServers.servers();
    if (server_index >= 0 &&
        server_index < static_cast<int>(servers.size())) {
      server = servers[static_cast<std::size_t>(server_index)];
    } else {
      // The custom entry. Described as a cluster rather than a reverse-beacon
      // feed because the two are weighed differently and nothing here can tell
      // which one a typed-in host is; claiming the stronger of the two for an
      // unknown node would overstate what its spots are worth.
      server.host = custom_host.trimmed();
      server.name = server.host;
      server.port = static_cast<std::uint16_t>(std::clamp(custom_port, 1, 65'535));
      server.source = cwassistant::core::CwSpotSource::Cluster;
    }
  }

  dx_cluster_enabled_ = joinable && server.isValid();
  if (!dx_cluster_enabled_) {
    if (dx_cluster_client_ != nullptr) dx_cluster_client_->setEnabled(false);
    dx_cluster_reverse_beacon_ = false;
    if (was_enabled) {
      dx_spot_expiry_timer_.stop();
      dx_spot_registry_.clear();
    }
    rebuildDxSpotModel();
    publishDxSpotsStatus();
    return;
  }

  dx_cluster_reverse_beacon_ =
      server.source == cwassistant::core::CwSpotSource::ReverseBeacon;
  ensureDxClusterClient();
  dx_cluster_client_->setServer(server);
  dx_cluster_client_->setLoginCallsign(login_callsign);
  // Before enabling, so the first login already carries the operator's band
  // rather than opening on the whole planet and narrowing a moment later.
  publishDxClusterBandFilter();
  dx_cluster_client_->setEnabled(true);
  if (!dx_spot_expiry_timer_.isActive()) dx_spot_expiry_timer_.start();
  rebuildDxSpotModel();
  publishDxSpotsStatus();
}

void ReplayController::acceptDxSpots(
    const std::vector<cwassistant::core::CwSpot>& spots) {
  if (!dx_cluster_enabled_ || spots.empty()) return;
  const std::uint64_t now_ns = currentUnixTimeNs();
  bool stored_any = false;
  for (const auto& spot : spots) {
    // Filtered before the store, not after. A spot from a band the receiver
    // cannot hear would occupy one of 4096 slots and cost a linear scan on
    // every later arrival; see isSpotWithinReceivedBand().
    if (!isSpotWithinReceivedBand(spot.frequency_hz)) continue;
    // The registry decides what it will hold. A spot it refuses is simply not
    // stored; nothing here retries, repairs, or works around that refusal.
    static_cast<void>(dx_spot_registry_.add(spot, now_ns));
    stored_any = true;
  }
  if (!stored_any) return;
  rebuildDxSpotModel();
}

void ReplayController::rebuildDxSpotModel() {
  QVariantList spots;
  if (dx_cluster_enabled_) {
    // Joining a node is itself a request to see what that node sends, so the
    // kind it supplies is what is displayed; there is no second source list to
    // tick it in.
    const bool show_reverse_beacon = dx_cluster_reverse_beacon_;
    const bool show_cluster = !dx_cluster_reverse_beacon_;
    const std::uint64_t now_ns = currentUnixTimeNs();
    dx_spot_registry_.expire(now_ns);
    for (const auto& match : dx_spot_registry_.all(now_ns)) {
      // The two switches decide what is shown, not what is true. A station
      // reported by both a skimmer and a person keeps both marks even when
      // only one of the two kinds is being displayed, because hiding half of
      // what corroborates a callsign would misrepresent the evidence.
      if (!(match.reverse_beacon && show_reverse_beacon) &&
          !(match.cluster && show_cluster)) {
        continue;
      }
      // Applied again on the way out, not only on arrival: retuning to another
      // band must drop the previous band's markers at once rather than leave
      // them standing until they expire.
      if (!isSpotWithinReceivedBand(match.frequency_hz)) continue;
      QVariantMap entry;
      entry.insert(QStringLiteral("callsign"),
                   QString::fromStdString(match.callsign));
      entry.insert(QStringLiteral("frequencyHz"), match.frequency_hz);
      const auto rf_hz =
          std::isfinite(match.frequency_hz) && match.frequency_hz > 0.0
              ? static_cast<qulonglong>(std::llround(match.frequency_hz))
              : 0ULL;
      // Not a number whenever the spot cannot be placed on the axis the
      // operator is looking at -- no radio frequency is known, or the source
      // is a recording. A spot drawn at a guessed position would be worse than
      // one that is not drawn.
      entry.insert(QStringLiteral("displayFrequencyHz"),
                   rfFrequencyToDisplayHz(rf_hz));
      entry.insert(QStringLiteral("reverseBeacon"), match.reverse_beacon);
      entry.insert(QStringLiteral("cluster"), match.cluster);
      const std::uint64_t age_ns = now_ns > match.newest_observation_ns
                                       ? now_ns - match.newest_observation_ns
                                       : 0ULL;
      entry.insert(QStringLiteral("ageSeconds"),
                   static_cast<int>(age_ns / 1'000'000'000ULL));
      entry.insert(QStringLiteral("observations"),
                   static_cast<int>(match.observations));
      spots.append(entry);
    }
  }
  if (dx_spots_ == spots) return;
  dx_spots_ = std::move(spots);
  emit dxSpotsChanged();
}

QString ReplayController::monitorStatusText() const {
  switch (monitor_mode_) {
    case 1:
      return QStringLiteral("Monitoring full receiver window");
    case 3:
      // Says what the operator is hearing and, just as importantly, what they
      // are not: this is the whole window, so every signal in it arrives at
      // once, at the pitch its own offset from the window centre gives it.
      return QStringLiteral("Listening to the whole %1 kHz decode region")
          .arg(sdr_decoder_bandwidth_hz_ / 1'000.0, 0, 'g', 3);
    case 2:
      return monitored_channel_ids_.isEmpty()
                 ? QStringLiteral("Use a decoder-card speaker to listen")
                 : (monitored_channel_ids_.size() == 1
                        ? QStringLiteral("Monitoring 1 stream")
                        : QStringLiteral("Monitoring %1 streams")
                              .arg(monitored_channel_ids_.size()));
    default:
      return QStringLiteral("Monitor off");
  }
}

void ReplayController::setMonitorMode(const int mode) {
  const int sanitized = std::clamp(mode, 0, 3);
  if (source_mode_ == 2 && sanitized == 1) {
    monitor_status_ = QStringLiteral(
        "Whole-IQ listening is unavailable; select one or more CW streams.");
    emit monitorChanged();
    return;
  }
  if (source_mode_ != 2 && sanitized == 3) {
    // There is no decode region without a complex receiver to carve one out
    // of. Refused rather than silently accepted, because a listen button that
    // engages and then plays nothing is indistinguishable from a broken one.
    monitor_status_ = QStringLiteral(
        "Region listening needs direct SDR reception.");
    emit monitorChanged();
    return;
  }
  if (monitor_mode_ == sanitized) return;
  monitor_mode_ = sanitized;
  if (monitor_mode_ != 2) monitored_channel_ids_.clear();
  if (monitor_mode_ == 0) stopMonitorOutput();
  monitor_status_ = monitorStatusText();
  publishMonitorConfiguration();
  emit monitorChanged();
}

bool ReplayController::isMonitorChannelEnabled(
    const qulonglong channel_id) const noexcept {
  return monitor_mode_ == 2 && channel_id != 0U &&
         monitored_channel_ids_.contains(channel_id);
}

void ReplayController::toggleMonitorChannel(const qulonglong channel_id) {
  const bool exists = std::any_of(
      decoder_channels_.cbegin(), decoder_channels_.cend(),
      [channel_id](const QVariant& value) {
        return value.toMap().value(QStringLiteral("id")).toULongLong() ==
               channel_id;
      });
  if (!exists || channel_id == 0U) return;

  const qsizetype existing_index = monitored_channel_ids_.indexOf(channel_id);
  if (monitor_mode_ == 2 && existing_index >= 0) {
    monitored_channel_ids_.removeAt(existing_index);
    if (monitored_channel_ids_.isEmpty()) {
      monitor_mode_ = 0;
      monitor_status_ = QStringLiteral("Monitor off");
      stopMonitorOutput();
    }
  } else {
    if (monitor_mode_ != 2) monitored_channel_ids_.clear();
    monitor_mode_ = 2;
    if (!monitored_channel_ids_.contains(channel_id))
      monitored_channel_ids_.push_back(channel_id);
  }
  if (monitor_mode_ == 2) {
    monitor_status_ = monitored_channel_ids_.size() == 1
        ? QStringLiteral("Monitoring 1 stream")
        : QStringLiteral("Monitoring %1 streams")
              .arg(monitored_channel_ids_.size());
  }
  stopMonitorOutput();
  publishMonitorConfiguration();
  emit monitorChanged();
}

void ReplayController::setRemoteAudioSubscribed(const bool subscribed) {
  emit liveRemoteAudioSubscribedRequested(subscribed);
}

void ReplayController::setMonitorLevel(const double level) {
  const double sanitized = std::clamp(level, 0.0, 1.0);
  if (monitor_level_ == sanitized) return;
  monitor_level_ = sanitized;
  if (monitor_audio_sink_) monitor_audio_sink_->setVolume(monitor_level_);
  emit monitorChanged();
}

void ReplayController::setMonitorOutputSelection(QString encoded_device_id) {
  if (monitor_output_device_id_ == encoded_device_id) return;
  monitor_output_device_id_ = std::move(encoded_device_id);
  stopMonitorOutput();
  if (monitor_mode_ != 0) monitor_status_ = monitorStatusText();
  emit monitorChanged();
}

void ReplayController::publishMonitorConfiguration() {
  emit monitorConfigureRequested(monitor_mode_, monitoredChannelIds(),
                                 cw_reference_tone_hz_);
  emit liveMonitorConfigureRequested(monitor_mode_, monitoredChannelIds(),
                                     cw_reference_tone_hz_);
}

void ReplayController::stopMonitorOutput() {
  monitor_audio_device_ = nullptr;
  if (monitor_audio_sink_) monitor_audio_sink_->stop();
  monitor_audio_sink_.reset();
  monitor_audio_sample_rate_ = 0;
}

void ReplayController::writeMonitorAudio(const QByteArray& float_mono_audio,
                                         const double sample_rate_hz) {
  if (monitor_mode_ == 0 || float_mono_audio.isEmpty() ||
      !std::isfinite(sample_rate_hz) || sample_rate_hz < 8'000.0 ||
      sample_rate_hz > 192'000.0) {
    return;
  }
  const int requested_rate = static_cast<int>(std::lround(sample_rate_hz));
  if (!monitor_audio_sink_ || monitor_audio_sample_rate_ != requested_rate) {
    stopMonitorOutput();
    const QAudioDevice device = monitorOutputDevice(monitor_output_device_id_);
    QAudioFormat format;
    format.setSampleRate(requested_rate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Float);
    if (device.isNull() || !device.isFormatSupported(format)) {
      monitor_status_ = QStringLiteral(
          "Selected monitor output does not support %1 Hz mono float audio")
                            .arg(requested_rate);
      emit monitorChanged();
      return;
    }
    monitor_audio_sink_ = std::make_unique<QAudioSink>(device, format, this);
    monitor_audio_sink_->setBufferSize(requested_rate *
                                       static_cast<int>(sizeof(float)) / 4);
    monitor_audio_sink_->setVolume(monitor_level_);
    monitor_audio_device_ = monitor_audio_sink_->start();
    monitor_audio_sample_rate_ = requested_rate;
    if (monitor_audio_device_ == nullptr) {
      monitor_status_ = QStringLiteral("Could not start the monitor output");
      stopMonitorOutput();
      emit monitorChanged();
      return;
    }
  }
  if (monitor_audio_sink_->bytesFree() < float_mono_audio.size()) return;
  if (monitor_audio_device_->write(float_mono_audio) < 0) {
    monitor_status_ = QStringLiteral("Monitor output write failed");
    stopMonitorOutput();
    emit monitorChanged();
  }
}

void ReplayController::openDecoderSession(const qulonglong channel_id) {
  const bool exists = std::any_of(
      decoder_channels_.cbegin(), decoder_channels_.cend(),
      [channel_id](const QVariant& value) {
        return value.toMap().value(QStringLiteral("id")).toULongLong() ==
               channel_id;
      });
  if (!exists) return;
  if (decoder_session_order_.contains(channel_id)) return;
  decoder_session_order_.push_back(channel_id);
  rebuildDecoderModels();
}

void ReplayController::openManualDecoderSession(
    const double audio_frequency_hz) {
  if (!activeSource() || !std::isfinite(audio_frequency_hz)) return;
  if (source_mode_ != 1) {
    emit liveManualDecoderFrequencyRequested(audio_frequency_hz);
  } else {
    emit manualDecoderFrequencyRequested(audio_frequency_hz);
  }
}

void ReplayController::closeDecoderSession(const qulonglong channel_id) {
  if (decoder_session_order_.removeAll(channel_id) == 0) return;
  if (monitored_channel_ids_.removeAll(channel_id) > 0) {
    if (monitored_channel_ids_.isEmpty()) {
      monitor_mode_ = 0;
      monitor_status_ = QStringLiteral("Monitor off");
      stopMonitorOutput();
    } else {
      monitor_status_ = monitored_channel_ids_.size() == 1
          ? QStringLiteral("Monitoring 1 stream")
          : QStringLiteral("Monitoring %1 streams")
                .arg(monitored_channel_ids_.size());
    }
    publishMonitorConfiguration();
    emit monitorChanged();
  }
  rebuildDecoderModels();
}

void ReplayController::moveDecoderSession(const qulonglong channel_id,
                                          const int new_index) {
  const int old_index = decoder_session_order_.indexOf(channel_id);
  if (old_index < 0 || decoder_session_order_.size() < 2) return;
  const int target = std::clamp(
      new_index, 0, static_cast<int>(decoder_session_order_.size()) - 1);
  if (old_index == target) return;
  decoder_session_order_.move(old_index, target);
  rebuildDecoderModels();
}

void ReplayController::resetDecoder() {
  raw_decoder_channels_.clear();
  decoder_channels_.clear();
  decoder_sessions_.clear();
  decoder_session_model_.replace(decoder_sessions_);
  decoder_session_order_.clear();
  verification_diagnostics_.clear();
  local_character_consensus_.clear();
  emit localCharacterResetRequested();
  emit decoderChanged();
}

void ReplayController::setSourceMode(const int value) {
  const int clamped = std::clamp(value, 0, 2);
  if (source_mode_ == clamped) {
    return;
  }
  if (clamped != 1) {
    emit stopRequested();
    playing_ = false;
  }
  emit liveStopRequested();
  emit sdrStopRequested();
  emit liveDspStopRequested();
  live_capturing_ = false;
  source_mode_ = clamped;
  if (source_mode_ == 2 && monitor_mode_ == 1) setMonitorMode(0);
  // The mirror of the line above. Region listening exists only while a complex
  // receiver is defining a region, so switching away from direct SDR must end
  // it rather than leave a live listen button over a source that has none.
  if (source_mode_ != 2 && monitor_mode_ == 3) setMonitorMode(0);
  rebuildDecoderModels();
  // Changing source changes which frequency counts as the receive frequency.
  publishDxClusterBandFilter();
  rebuildDxSpotModel();
  emit sourceReset();
  // The axis mapping differs between a direct IQ source, which already carries
  // absolute RF, and an audio card, which needs a dial to map against.
  emit radioFrequencyChanged();
  emit stateChanged();
}

void ReplayController::setDecodedSignalTimeoutSeconds(const int seconds) {
  emit decodedSignalTimeoutRequested(seconds);
  emit liveDecodedSignalTimeoutRequested(seconds);
}

bool ReplayController::decodeWeakSignals() const noexcept {
  return decode_weak_signals_;
}

double ReplayController::minimumDecodeSnrDb() const noexcept {
  return minimum_decode_snr_db_;
}

void ReplayController::setWeakSignalDecoding(
    const bool enabled, const double minimum_decode_snr_db) {
  const bool changed = decode_weak_signals_ != enabled ||
                       minimum_decode_snr_db_ != minimum_decode_snr_db;
  decode_weak_signals_ = enabled;
  minimum_decode_snr_db_ = minimum_decode_snr_db;
  // Both decode paths are told, so the choice holds whether the operator is on
  // live audio or replaying a capture. Sending only the live one left WAV
  // replay gating on the channel bank's built-in defaults, which is exactly
  // where a weak recording is most likely to be examined.
  //
  // Resent even when nothing changed. The decoded-signal timeout reaches the
  // channel bank through configure(), which replaces the whole configuration
  // with a fresh one, so any later timeout update would otherwise silently
  // restore the bank's built-in weak-signal defaults over the operator's
  // choice. Resending costs nothing and keeps the gate authoritative.
  emit weakSignalDecodingRequested(decode_weak_signals_,
                                   minimum_decode_snr_db_);
  emit liveWeakSignalDecodingRequested(decode_weak_signals_,
                                       minimum_decode_snr_db_);
  if (changed) emit weakSignalDecodingChanged();
}

void ReplayController::openDebugCaptureFolder() {
  if (debug_capture_path_.isEmpty()) return;
  const QFileInfo info(debug_capture_path_);
  // The recorded path may name the folder itself or a file inside it,
  // depending on how far the capture got; either way the operator wants the
  // folder, so that a capture stopped early still opens somewhere useful.
  const QString folder = info.isDir() ? info.absoluteFilePath()
                                      : info.absolutePath();
  if (folder.isEmpty() || !QFileInfo::exists(folder)) return;
  static_cast<void>(
      QDesktopServices::openUrl(QUrl::fromLocalFile(folder)));
}

void ReplayController::setDebugCaptureMaximumSeconds(const int seconds) {
  emit liveDebugCaptureMaximumSecondsRequested(
      static_cast<double>(std::clamp(seconds, 30, 1'800)));
}

void ReplayController::setOperatorRole(const QString& role) {
  emit operatorRoleRequested(role);
  emit liveOperatorRoleRequested(role);
}

void ReplayController::setKeyingModel(const QString& model) {
  // Both decode paths are told, so the choice holds whether the operator is on
  // live audio or replaying a capture.
  keying_model_ = model;
  emit keyingModelRequested(keying_model_);
  emit liveKeyingModelRequested(keying_model_);
  emit keyingModelChanged();
}

const QString& ReplayController::keyingModel() const noexcept {
  return keying_model_;
}

void ReplayController::setOwnCallsign(const QString& callsign) {
  own_callsign_ = callsign.trimmed().toUpper();
  emit ownCallsignRequested(own_callsign_);
  emit liveOwnCallsignRequested(own_callsign_);
}

void ReplayController::setAudioInputSelection(QString encoded_id,
                                               QString display_name,
                                               QString device_name) {
  const bool changed = audio_input_id_ != encoded_id;
  audio_input_id_ = std::move(encoded_id);
  audio_input_name_ = std::move(display_name);
  audio_input_device_name_ = std::move(device_name);
  if (changed && live_capturing_ && source_mode_ == 0) {
    beginLiveAudioCapture();
  }
}

void ReplayController::setSdrInputSelection(
    QString device_id, QString display_name,
    const qulonglong center_frequency_hz, const int sample_rate_hz,
    const int bandwidth_hz, QString antenna, const bool automatic_gain,
    const double gain_db,
    const qulonglong decoder_center_frequency_hz,
    const int decoder_bandwidth_hz) {
  const bool center_changed = sdr_center_frequency_hz_ != center_frequency_hz;
  const bool restart = live_capturing_ && source_mode_ == 2 &&
      (sdr_device_id_ != device_id ||
       sdr_sample_rate_hz_ != sample_rate_hz ||
       sdr_bandwidth_hz_ != bandwidth_hz ||
       sdr_antenna_ != antenna ||
       sdr_automatic_gain_ != automatic_gain || sdr_gain_db_ != gain_db);
  sdr_device_id_ = std::move(device_id);
  sdr_device_name_ = std::move(display_name);
  sdr_center_frequency_hz_ = center_frequency_hz;
  sdr_sample_rate_hz_ = sample_rate_hz;
  sdr_bandwidth_hz_ = bandwidth_hz;
  sdr_antenna_ = std::move(antenna);
  sdr_automatic_gain_ = automatic_gain;
  sdr_gain_db_ = gain_db;
  const bool decoder_window_changed =
      sdr_decoder_center_frequency_hz_ != decoder_center_frequency_hz ||
      sdr_decoder_bandwidth_hz_ != decoder_bandwidth_hz;
  sdr_decoder_center_frequency_hz_ = decoder_center_frequency_hz;
  sdr_decoder_bandwidth_hz_ = decoder_bandwidth_hz;
  if (decoder_window_changed || center_changed || !live_capturing_) {
    // Always through the clamping publisher. Emitting the stored window raw
    // was the whole defect: the preference is expressed in absolute RF and
    // means nothing until it is checked against the passband actually being
    // acquired, so a receiver configured for any band but the stored one was
    // handed a window the decimator refuses.
    publishSdrDecoderWindow();
  }
  if (restart) {
    beginLiveSdrCapture();
  } else if (center_changed && live_capturing_ && source_mode_ == 2) {
    // Retuning moves the passband out from under the decoder window; the
    // publication above has already re-centred it, so the receiver and the
    // decoder move together instead of the tracks vanishing on the first
    // click of the dial.
    emit sdrRetuneRequested(static_cast<double>(sdr_center_frequency_hz_));
    // And restate the acquisition the worker is to compare its block
    // descriptors against. A retune does not restart the receiver, so the
    // `started` signal does not fire again; without this the record's
    // requested acquisition would stay at the frequency the session opened on
    // and every later retune would read as a receiver that had gone astray.
    emit liveSdrCaptureContextRequested(
        sdr_device_name_, sdr_antenna_, sdr_automatic_gain_, sdr_gain_db_,
        static_cast<double>(sdr_center_frequency_hz_),
        static_cast<double>(sdr_sample_rate_hz_));
  }
}

void ReplayController::openFile(const QUrl& url) {
  setSourceMode(1);
  const QString path = url.isLocalFile() ? url.toLocalFile() : QString{};
  clearBlockingError();
  if (path.isEmpty()) {
    setBlockingError(QStringLiteral("Select a local WAV file."));
    return;
  }
  source_loaded_ = false;
  playing_ = false;
  source_name_.clear();
  position_seconds_ = 0.0;
  setStatus(QStringLiteral("Opening WAV recording…"));
  emit sourceReset();
  emit openRequested(path);
}

void ReplayController::play() {
  setSourceMode(1);
  emit playRequested();
}
void ReplayController::pause() { emit pauseRequested(); }
void ReplayController::stop() {
  emit stopRequested();
  emit sourceReset();
}

void ReplayController::startLiveAudio() {
  setSourceMode(0);
  QMicrophonePermission permission;
  auto* application = QCoreApplication::instance();
  switch (application->checkPermission(permission)) {
    case Qt::PermissionStatus::Undetermined:
      setStatus(QStringLiteral("Waiting for microphone/audio-input permission…"));
      application->requestPermission(
          permission, this,
          [this](const QPermission&) { startLiveAudio(); });
      return;
    case Qt::PermissionStatus::Denied:
      setBlockingError(QStringLiteral(
          "Audio-input permission was denied. Enable microphone access in the operating-system privacy settings."));
      return;
    case Qt::PermissionStatus::Granted:
      beginLiveAudioCapture();
      return;
  }
}

void ReplayController::startLiveSdr() {
  setSourceMode(2);
  beginLiveSdrCapture();
}

void ReplayController::beginLiveAudioCapture() {
  emit sdrStopRequested();
  emit stopRequested();
  playing_ = false;
  source_loaded_ = false;
  input_overruns_ = 0;
  emit sourceReset();
  // Before clearBlockingError(), which decoder_display_separation_test.cpp
  // pins to the status line that follows it: a recovery notice is about the
  // start now beginning, so a previous one must not survive into it.
  audio_input_recovery_notice_.clear();
  clearBlockingError();
  setStatus(QStringLiteral("Starting live audio from %1…").arg(audio_input_name_));
  publishSpectrumConfiguration();
  emit liveDspStartRequested();
  // After liveDspStartRequested(), for the reason given on the SDR path: the
  // monitor is a demand the worker has to be holding, and a start is where it
  // must be restated.
  // Only where there is a demand to restate. Mode 0 is "no monitor", which
  // is what the worker holds immediately after a start in any case, so
  // publishing it states nothing and still crosses a thread boundary during
  // startup -- on a machine with no audio device at all that is a cost for
  // no statement. A real demand, which is the case this restatement exists
  // for, is unaffected.
  if (monitor_mode_ != 0) publishMonitorConfiguration();
  emit liveStartRequested(audio_input_id_, audio_input_device_name_);
}

void ReplayController::beginLiveSdrCapture() {
  if (sdr_device_id_.isEmpty()) {
    setBlockingError(QStringLiteral(
        "Select a discovered SDR device in Settings before starting RX."));
    return;
  }
  emit liveStopRequested();
  emit stopRequested();
  playing_ = false;
  source_loaded_ = false;
  live_capturing_ = false;
  input_overruns_ = 0;
  emit sourceReset();
  clearBlockingError();
  setStatus(QStringLiteral("Starting live SDR from %1…").arg(sdr_device_name_));
  publishSpectrumConfiguration();
  emit liveDspStartRequested();
  // After liveDspStartRequested(), because LiveAudioDspWorker::start() resets
  // the decimator's stream state; the window has to be the first thing the
  // freshly started worker is told.
  publishSdrDecoderWindow();
  // And the monitor with it. Every other setting the DSP worker needs is
  // restated here -- the spectrum configuration above, the decode window on
  // the line before -- and the monitor was the one that was not, so the only
  // thing that ever put a monitor mode into that worker was the operator
  // happening to press a listen control after it had started. Nothing
  // reconciled the two copies at a start, which is precisely where they can
  // part company: start() clears the region demodulator, the in-flight count
  // and the sample counters, so whatever demand the controller is holding has
  // to be said again on the other side of it. Cheap -- one queued call
  // carrying three scalars -- and it makes the controller, not the order in
  // which an operator happened to press things, the authority on what this
  // station is listening to.
  // Only where there is a demand to restate. Mode 0 is "no monitor", which
  // is what the worker holds immediately after a start in any case, so
  // publishing it states nothing and still crosses a thread boundary during
  // startup -- on a machine with no audio device at all that is a cost for
  // no statement. A real demand, which is the case this restatement exists
  // for, is unaffected.
  if (monitor_mode_ != 0) publishMonitorConfiguration();
  emit sdrStartRequested(sdr_device_id_,
                         static_cast<double>(sdr_center_frequency_hz_),
                         static_cast<double>(sdr_sample_rate_hz_),
                         static_cast<double>(sdr_bandwidth_hz_),
                         sdr_antenna_,
                         sdr_automatic_gain_, sdr_gain_db_);
}

void ReplayController::startDebugCapture() {
  if (!live_capturing_) {
    setStatus(QStringLiteral(
        "Debug capture requires live RX to be running."));
    return;
  }
  const QString base = QStandardPaths::writableLocation(
      QStandardPaths::AppDataLocation);
  const QString directory = QDir(base).filePath(QStringLiteral("diagnostics"));
  // Queued delivery to the DSP worker preserves signal order, so refresh the
  // presentation context immediately before recording starts. The first JSON
  // snapshot can then explain database/model availability and every advisory
  // callsign decision without resetting the live decoder.
  publishLivePresentationDiagnostics(true);
  emit liveDebugCaptureStartRequested(directory);
  // Name the files that are about to appear. Direct SDR reception records true
  // complex IQ as a SigMF pair; the audio path is unchanged. The worker picks
  // the recorder from the first block's descriptor, so this reflects the
  // selected source rather than deciding it.
  setStatus(source_mode_ == 2
                ? QStringLiteral(
                      "Debug capture: recording SDR IQ to iq.sigmf-data with "
                      "an iq.sigmf-meta sidecar…")
                : QStringLiteral(
                      "Debug capture: recording receiver audio to audio.wav…"));
}

void ReplayController::stopDebugCapture() {
  emit liveDebugCaptureStopRequested();
}

void ReplayController::stopLiveAudio() {
  emit liveStopRequested();
  emit sdrStopRequested();
  emit liveDspStopRequested();
  if (live_capturing_) {
    live_capturing_ = false;
    rebuildDecoderModels();
    setStatus(QStringLiteral("Live audio stopped"));
    emit sourceReset();
  }
}

void ReplayController::setStatus(QString status) {
  status_text_ = std::move(status);
  emit stateChanged();
}

const QString& ReplayController::blockingError() const noexcept {
  return blocking_error_;
}

void ReplayController::setBlockingError(QString message) {
  blocking_error_ = message;
  setStatus(std::move(message));
}

void ReplayController::clearBlockingError() {
  if (blocking_error_.isEmpty()) return;
  blocking_error_.clear();
  emit stateChanged();
}

void ReplayController::dismissBlockingError() { clearBlockingError(); }

}  // namespace cwassistant::desktop

#include "replay_controller.moc"
