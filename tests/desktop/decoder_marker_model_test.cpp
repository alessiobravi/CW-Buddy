// The spectrum-marker presentation model.
//
// Three things are asserted here, and they are three different kinds of
// claim.
//
// What a marker is drawn from. The model publishes a reduced channel, so the
// delegate in Main.qml must not read a key the reduction drops: that would
// not fail a build, it would reach QML as `undefined` and draw the marker
// wrong. The delegate is therefore scanned and held to the list.
//
// What the model reports. Counters, in the shape
// DecoderChannelPresentationCache already uses, so that "a publish that
// changed nothing changes nothing" can be stated exactly instead of timed.
//
// What Qt Quick actually does with it. The claim this work exists for is
// about delegates, not about models, so the last section builds a real
// Repeater over a real QQmlEngine and counts delegate creations -- once over
// the QVariantList the markers used to be driven by, and once over the model,
// with the same publishes going into both.

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QModelIndex>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "replay/decoder_channel_model.hpp"

namespace {

using cwassistant::desktop::DecoderChannelMarkerModel;
using cwassistant::desktop::decoderChannelMarkerKeys;
using cwassistant::desktop::decoderChannelMarkerRow;

// A published channel as ReplayController annotates it: the decoder's own
// keys, the frequency keys it adds, and the callsign keys the presentation
// cache derives. Only the parts a marker could possibly care about are
// filled with distinguishable values; the rest stand in for the bulk a real
// channel carries, because the point of several of them is that they move on
// every publish and must not reach the model.
[[nodiscard]] QVariantMap makeChannel(const qulonglong id, const double hz,
                                      const bool verified = true) {
  QVariantMap channel;
  channel.insert(QStringLiteral("id"), QVariant::fromValue(id));
  channel.insert(QStringLiteral("presentationFrequencyHz"), hz);
  channel.insert(QStringLiteral("frequencyHz"), hz);
  channel.insert(QStringLiteral("audioFrequencyHz"), hz);
  channel.insert(QStringLiteral("trackedAudioFrequencyHz"), hz);
  channel.insert(QStringLiteral("displayFrequencyHz"), hz);
  channel.insert(QStringLiteral("frequencyKind"), QStringLiteral("AF"));
  channel.insert(QStringLiteral("frequencyLabel"), QStringLiteral("700 Hz AF"));
  channel.insert(QStringLiteral("driftHzPerSecond"), 0.2);
  channel.insert(QStringLiteral("filterWidthHz"), 120.0);
  channel.insert(QStringLiteral("verifiedCw"), verified);
  channel.insert(QStringLiteral("operatorSelected"), false);
  channel.insert(QStringLiteral("active"), true);
  channel.insert(QStringLiteral("keyDown"), false);
  channel.insert(QStringLiteral("color"),
                 verified ? QStringLiteral("#7fc8ff")
                          : QStringLiteral("#8d9aaa"));
  channel.insert(QStringLiteral("callingOwnStation"), false);
  channel.insert(QStringLiteral("callsign"),
                 verified ? QStringLiteral("SV2HQL") : QString{});
  channel.insert(QStringLiteral("callsignInDatabase"), verified);
  channel.insert(QStringLiteral("callsignSuggestion"), QString{});
  channel.insert(QStringLiteral("callsignSuggestionSource"), QString{});
  channel.insert(QStringLiteral("callsignSuggestionRawSpan"), QString{});
  // The parts that move on nearly every publish and that no marker shows.
  channel.insert(QStringLiteral("snrDb"), 18.4);
  channel.insert(QStringLiteral("wpm"), 22.0);
  channel.insert(QStringLiteral("confidence"), 0.82);
  channel.insert(QStringLiteral("keyProbability"), 0.11);
  channel.insert(QStringLiteral("text"),
                 verified ? QStringLiteral("CQ CQ DE SV2HQL") : QString{});
  channel.insert(QStringLiteral("refinedText"), QString{});
  channel.insert(QStringLiteral("provisionalText"), QString{});
  channel.insert(QStringLiteral("contextualText"), QString{});
  channel.insert(QStringLiteral("acousticAlternatives"), QVariantList{});
  channel.insert(QStringLiteral("transmissions"), QVariantList{});
  channel.insert(QStringLiteral("characterEvidence"), QVariantList{});
  channel.insert(QStringLiteral("sessionOpen"), false);
  return channel;
}

[[nodiscard]] QVariantList makeChannels(const int count) {
  QVariantList channels;
  for (int index = 0; index < count; ++index) {
    channels.push_back(makeChannel(static_cast<qulonglong>(index + 1),
                                   600.0 + 50.0 * index));
  }
  return channels;
}

void setIn(QVariantList& channels, const int row, const QString& key,
           const QVariant& value) {
  QVariantMap channel = channels.at(row).toMap();
  channel.insert(key, value);
  channels[row] = channel;
}

// ---------------------------------------------------------------------------
// What a marker is drawn from.
// ---------------------------------------------------------------------------

[[nodiscard]] bool markerKeysContain(const std::string& name) {
  for (const char* const key : decoderChannelMarkerKeys()) {
    if (name == key) return true;
  }
  return false;
}

// Every `<object>.<identifier>` in `block` whose object is `object`.
[[nodiscard]] std::vector<std::string> readsOf(const std::string& block,
                                               const std::string& object) {
  const std::string prefix = object + ".";
  std::vector<std::string> names;
  std::size_t offset = 0;
  while ((offset = block.find(prefix, offset)) != std::string::npos) {
    // Not a read of this object when something identifier-like runs into it,
    // which is what `channelMarker.modelData` would be.
    const bool joined =
        offset > 0 && (std::isalnum(static_cast<unsigned char>(
                           block[offset - 1])) != 0 ||
                       block[offset - 1] == '_' || block[offset - 1] == '.');
    offset += prefix.size();
    if (joined) continue;
    std::size_t end = offset;
    while (end < block.size() &&
           (std::isalnum(static_cast<unsigned char>(block[end])) != 0 ||
            block[end] == '_')) {
      ++end;
    }
    if (end == offset) continue;
    names.push_back(block.substr(offset, end - offset));
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

int testDelegateReadsNothingTheRowDoesNotCarry() {
  std::ifstream source(CWA_MAIN_QML_PATH, std::ios::binary);
  if (!source) return 10;
  std::string qml{std::istreambuf_iterator<char>{source},
                  std::istreambuf_iterator<char>{}};
  qml.erase(std::remove(qml.begin(), qml.end(), '\r'), qml.end());

  const std::size_t marker_start = qml.find("id: channelMarker");
  const std::size_t marker_end = qml.find("id: dxSpotOverlay", marker_start);
  const std::size_t model_line =
      qml.find("model: replayController.decoderMarkerModel");
  if (marker_start == std::string::npos || marker_end == std::string::npos ||
      model_line == std::string::npos || model_line >= marker_start) {
    return 11;
  }
  const std::string delegate =
      qml.substr(marker_start, marker_end - marker_start);

  // Every key the delegate names on its row.
  for (const std::string& name : readsOf(delegate, "modelData")) {
    if (markerKeysContain(name)) continue;
    std::fprintf(stderr,
                 "marker delegate reads modelData.%s, which the marker row "
                 "does not carry\n",
                 name.c_str());
    return 12;
  }

  // And every key the one helper it passes its whole row to names on it.
  if (delegate.find("window.formatStreamFrequency(modelData)") ==
      std::string::npos) {
    return 13;
  }
  const std::size_t format_start =
      qml.find("function formatStreamFrequency(channel) {");
  if (format_start == std::string::npos) return 14;
  const std::size_t format_end = qml.find("\n    function ", format_start);
  if (format_end == std::string::npos) return 15;
  const std::string format_body =
      qml.substr(format_start, format_end - format_start);
  for (const std::string& name : readsOf(format_body, "channel")) {
    if (markerKeysContain(name)) continue;
    std::fprintf(stderr,
                 "formatStreamFrequency reads channel.%s, which the marker "
                 "row does not carry\n",
                 name.c_str());
    return 16;
  }
  return 0;
}

int testRowCarriesTheMarkerKeysAndNothingHeavy() {
  const QVariantMap channel = makeChannel(7, 712.5);
  const QVariantMap row = decoderChannelMarkerRow(channel);

  if (row.size() != static_cast<qsizetype>(decoderChannelMarkerKeys().size())) {
    return 20;
  }
  // Every marker key is carried through unchanged, so a marker drawn from the
  // row is drawn from exactly the values it was drawn from before.
  for (const char* const key : decoderChannelMarkerKeys()) {
    const QString name = QString::fromLatin1(key);
    if (!row.contains(name)) return 21;
    if (row.value(name) != channel.value(name)) return 22;
  }
  // The bulk stays out. These are the keys that move on nearly every publish
  // and are the reason the old whole-list assignment was never a no-op.
  for (const char* const heavy :
       {"text", "refinedText", "provisionalText", "contextualText",
        "acousticAlternatives", "transmissions", "characterEvidence",
        "snrDb", "wpm", "confidence", "keyProbability"}) {
    if (row.contains(QString::fromLatin1(heavy))) return 23;
  }
  return 0;
}

int testUnverifiedRowNamesNoStation() {
  const QVariantMap row =
      decoderChannelMarkerRow(makeChannel(3, 980.0, /*verified=*/false));
  if (row.value(QStringLiteral("verifiedCw")).toBool()) return 30;
  if (!row.value(QStringLiteral("callsign")).toString().isEmpty()) return 31;
  if (!row.value(QStringLiteral("callsignSuggestion")).toString().isEmpty()) {
    return 32;
  }
  if (!row.value(QStringLiteral("callsignSuggestionRawSpan"))
           .toString()
           .isEmpty()) {
    return 33;
  }
  if (row.value(QStringLiteral("callingOwnStation")).toBool()) return 34;
  if (row.value(QStringLiteral("color")).toString() !=
      QStringLiteral("#8d9aaa")) {
    return 35;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// What the model reports.
// ---------------------------------------------------------------------------

int testUnchangedPublishReportsNothing() {
  DecoderChannelMarkerModel model;
  QVariantList channels = makeChannels(6);

  // An empty model filling for the first time is six arrivals, not a reset:
  // there is nothing yet to destroy, so the two are the same thing and the
  // arrival path is the one that runs.
  model.replace(channels);
  if (model.rowCount() != 6) return 40;
  if (model.resets() != 0U || model.insertions() != 6U) return 41;
  if (model.rowChanges() != 0U) return 42;
  if (model.data(model.index(2, 0), Qt::UserRole + 1).toMap() !=
      decoderChannelMarkerRow(channels.at(2).toMap())) {
    return 43;
  }
  if (model.roleNames().value(Qt::UserRole + 1) !=
      QByteArrayLiteral("modelData")) {
    return 44;
  }

  // The same publish again, five times over. This is the claim.
  for (int publish = 0; publish < 5; ++publish) model.replace(channels);
  if (model.resets() != 0U || model.rowChanges() != 0U ||
      model.insertions() != 6U || model.removals() != 0U) {
    return 45;
  }

  // A publish in which the levels and the transcript moved on every stream,
  // which is what an ordinary publish on a busy band is. None of it is drawn
  // on a marker, so none of it reaches the view.
  for (int row = 0; row < channels.size(); ++row) {
    setIn(channels, row, QStringLiteral("snrDb"), 18.4 + 0.1 * row);
    setIn(channels, row, QStringLiteral("confidence"), 0.83);
    setIn(channels, row, QStringLiteral("text"),
          QStringLiteral("CQ CQ DE SV2HQL K"));
  }
  model.replace(channels);
  if (model.resets() != 0U || model.rowChanges() != 0U ||
      model.insertions() != 6U || model.removals() != 0U) {
    return 46;
  }
  return 0;
}

int testOneTrackChangeTouchesOneRow() {
  DecoderChannelMarkerModel model;
  QVariantList channels = makeChannels(6);
  model.replace(channels);

  int changed_rows = 0;
  int changed_row = -1;
  QObject::connect(&model, &DecoderChannelMarkerModel::dataChanged,
                   [&](const QModelIndex& first, const QModelIndex& last,
                       const QList<int>&) {
                     changed_rows += last.row() - first.row() + 1;
                     changed_row = first.row();
                   });

  setIn(channels, 3, QStringLiteral("presentationFrequencyHz"), 812.4);
  model.replace(channels);
  if (changed_rows != 1 || changed_row != 3) return 50;
  if (model.rowChanges() != 1U) return 51;
  if (model.resets() != 0U || model.insertions() != 6U ||
      model.removals() != 0U) {
    return 52;
  }
  if (model.data(model.index(3, 0), Qt::UserRole + 1)
          .toMap()
          .value(QStringLiteral("presentationFrequencyHz"))
          .toDouble() != 812.4) {
    return 53;
  }

  // Key-down moves at Morse cadence on the one stream that is sending, and it
  // is drawn, so it must reach exactly that stream's row and no other.
  changed_rows = 0;
  changed_row = -1;
  setIn(channels, 1, QStringLiteral("keyDown"), true);
  model.replace(channels);
  if (changed_rows != 1 || changed_row != 1) return 54;
  if (model.rowChanges() != 2U) return 55;
  return 0;
}

int testTrackArrivalAndDepartureKeepTheRest() {
  DecoderChannelMarkerModel model;
  QVariantList channels = makeChannels(4);
  model.replace(channels);
  // Filling an empty model is four arrivals; everything below is counted
  // against that, so only what this test provokes is being read.
  const qulonglong resets_after_load = model.resets();
  const qulonglong insertions_after_load = model.insertions();
  if (resets_after_load != 0U || insertions_after_load != 4U) return 59;

  // A new carrier is acquired. One row appears; nothing else is disturbed.
  channels.push_back(makeChannel(99, 1420.0));
  model.replace(channels);
  if (model.rowCount() != 5) return 60;
  if (model.insertions() != insertions_after_load + 1U) return 61;
  if (model.removals() != 0U) return 62;
  if (model.resets() != resets_after_load) return 63;
  if (model.rowChanges() != 0U) return 64;

  // One in the middle is dropped.
  channels.removeAt(2);
  model.replace(channels);
  if (model.rowCount() != 4) return 65;
  if (model.removals() != 1U) return 66;
  if (model.insertions() != insertions_after_load + 1U) return 67;
  if (model.resets() != resets_after_load) return 68;
  if (model.rowChanges() != 0U) return 69;
  if (model.data(model.index(2, 0), Qt::UserRole + 1)
          .toMap()
          .value(QStringLiteral("id"))
          .toULongLong() != 4U) {
    return 70;
  }

  // An arrival at the front and a departure at the back in one publish.
  channels.removeLast();
  channels.prepend(makeChannel(150, 400.0));
  model.replace(channels);
  if (model.rowCount() != 4) return 71;
  if (model.insertions() != insertions_after_load + 2U ||
      model.removals() != 2U) {
    return 72;
  }
  if (model.resets() != resets_after_load) return 73;
  if (model.data(model.index(0, 0), Qt::UserRole + 1)
          .toMap()
          .value(QStringLiteral("id"))
          .toULongLong() != 150U) {
    return 74;
  }

  // A genuine reorder is the one case that still resets, and it must leave
  // the model holding the new order rather than the old one.
  QVariantList reordered = channels;
  std::reverse(reordered.begin(), reordered.end());
  model.replace(reordered);
  if (model.resets() != resets_after_load + 1U) return 75;
  if (model.rowCount() != reordered.size()) return 76;
  for (int row = 0; row < reordered.size(); ++row) {
    if (model.data(model.index(row, 0), Qt::UserRole + 1).toMap() !=
        decoderChannelMarkerRow(reordered.at(row).toMap())) {
      return 77;
    }
  }

  // The source is changed and the decoder reset. The model holds its own copy
  // of the rows, so it has to be emptied too or it keeps drawing the previous
  // source's streams over the new one's spectrum.
  model.replace(QVariantList{});
  if (model.rowCount() != 0) return 78;
  if (model.data(model.index(0, 0), Qt::UserRole + 1).isValid()) return 79;
  return 0;
}

// ---------------------------------------------------------------------------
// What Qt Quick does with it.
// ---------------------------------------------------------------------------

// A Repeater with the same delegate in both cases, so the only difference
// between the two measurements is what drives it. The delegate reads the two
// keys a marker is positioned and coloured by, which is enough for Qt to have
// to build it.
constexpr const char* kProbeQml = R"(
import QtQuick
Item {
    id: probeRoot
    property int created: 0
    property int destroyed: 0
    property var markerList: []
    property var markerModel: null
    property bool useModel: false
    Repeater {
        model: probeRoot.useModel ? probeRoot.markerModel : probeRoot.markerList
        delegate: Item {
            required property var modelData
            property real hz: modelData.presentationFrequencyHz
            property string tint: modelData.color
            Component.onCompleted: probeRoot.created = probeRoot.created + 1
            Component.onDestruction: probeRoot.destroyed = probeRoot.destroyed + 1
        }
    }
}
)";

struct Probe {
  QQmlEngine engine;
  QQmlComponent component{&engine};
  QObject* root{nullptr};

  [[nodiscard]] bool build() {
    component.setData(QByteArray(kProbeQml), QUrl(QStringLiteral("probe.qml")));
    root = component.create();
    if (root == nullptr) {
      std::fprintf(stderr, "probe QML failed: %s\n",
                   qPrintable(component.errorString()));
    }
    return root != nullptr;
  }
  [[nodiscard]] int created() const { return root->property("created").toInt(); }
  [[nodiscard]] int destroyed() const {
    return root->property("destroyed").toInt();
  }
  void settle() { QCoreApplication::processEvents(); }
};

int testRepeaterChurn() {
  constexpr int kTracks = 8;
  QVariantList channels = makeChannels(kTracks);

  // The shape that shipped: the whole annotated list, reassigned.
  Probe before;
  if (!before.build()) return 80;
  before.root->setProperty("markerList", channels);
  before.settle();
  if (before.created() != kTracks) return 81;

  // One stream's signal-to-noise moves by a tenth of a decibel. No marker
  // shows it, but the list is a different list, so every marker is destroyed
  // and rebuilt. This is the cost, measured.
  QVariantList moved = channels;
  setIn(moved, 2, QStringLiteral("snrDb"), 18.5);
  before.root->setProperty("markerList", moved);
  before.settle();
  const int churn_created = before.created() - kTracks;
  const int churn_destroyed = before.destroyed();
  std::fprintf(stdout,
               "QVariantList Repeater, one stream's SNR moved: %d delegates "
               "created, %d destroyed\n",
               churn_created, churn_destroyed);
  // If this did not churn, the premise of the whole change is wrong and the
  // comparison below would prove nothing.
  if (churn_created != kTracks) return 82;

  // The model, given the same two publishes.
  DecoderChannelMarkerModel model;
  model.replace(channels);
  Probe after;
  if (!after.build()) return 83;
  after.root->setProperty("markerModel", QVariant::fromValue(
                                             static_cast<QObject*>(&model)));
  after.root->setProperty("useModel", true);
  after.settle();
  if (after.created() != kTracks) return 84;

  model.replace(moved);
  after.settle();
  std::fprintf(stdout,
               "marker model, one stream's SNR moved: %d delegates created, "
               "%d destroyed\n",
               after.created() - kTracks, after.destroyed());
  if (after.created() != kTracks) return 85;
  if (after.destroyed() != 0) return 86;

  // Five publishes that change nothing at all.
  for (int publish = 0; publish < 5; ++publish) model.replace(moved);
  after.settle();
  if (after.created() != kTracks || after.destroyed() != 0) return 87;

  // One stream retunes. The row it is drawn from changes, and no delegate is
  // destroyed for it.
  QVariantList retuned = moved;
  setIn(retuned, 5, QStringLiteral("presentationFrequencyHz"), 1234.5);
  model.replace(retuned);
  after.settle();
  if (after.created() != kTracks || after.destroyed() != 0) return 88;

  // A new carrier: one more delegate, and the eight already drawn are left
  // alone.
  QVariantList grown = retuned;
  grown.push_back(makeChannel(500, 2100.0));
  model.replace(grown);
  after.settle();
  std::fprintf(stdout,
               "marker model, a ninth stream acquired: %d delegates created "
               "in all, %d destroyed\n",
               after.created(), after.destroyed());
  if (after.created() != kTracks + 1) return 89;
  if (after.destroyed() != 0) return 90;

  return 0;
}

// Both of the controller's statements about the model, read out of the
// source. The model's own emptying is asserted above, but nothing there can
// see whether the controller remembers to ask for it, and a marker model left
// alone when the decoder is reset keeps drawing the previous source's streams
// over the new one's spectrum.
int testControllerPublishesAndEmptiesTheModel() {
  std::ifstream source(CWA_REPLAY_CONTROLLER_CPP_PATH, std::ios::binary);
  if (!source) return 100;
  std::string cpp{std::istreambuf_iterator<char>{source},
                  std::istreambuf_iterator<char>{}};
  cpp.erase(std::remove(cpp.begin(), cpp.end(), '\r'), cpp.end());

  const auto body = [&cpp](const std::string& signature) {
    const std::size_t start = cpp.find(signature);
    if (start == std::string::npos) return std::string{};
    const std::size_t end = cpp.find("\n}\n", start);
    if (end == std::string::npos) return std::string{};
    return cpp.substr(start, end - start);
  };
  const std::string rebuild = body("void ReplayController::rebuildDecoderModels() {");
  const std::string reset = body("void ReplayController::resetDecoder() {");
  if (rebuild.empty() || reset.empty()) return 101;
  if (rebuild.find("decoder_marker_model_.replace(") == std::string::npos) {
    return 102;
  }
  if (reset.find("decoder_marker_model_.replace(") == std::string::npos) {
    return 103;
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication application(argc, argv);
  if (const int failure = testDelegateReadsNothingTheRowDoesNotCarry();
      failure != 0) {
    return failure;
  }
  if (const int failure = testRowCarriesTheMarkerKeysAndNothingHeavy();
      failure != 0) {
    return failure;
  }
  if (const int failure = testUnverifiedRowNamesNoStation(); failure != 0) {
    return failure;
  }
  if (const int failure = testUnchangedPublishReportsNothing(); failure != 0) {
    return failure;
  }
  if (const int failure = testOneTrackChangeTouchesOneRow(); failure != 0) {
    return failure;
  }
  if (const int failure = testTrackArrivalAndDepartureKeepTheRest();
      failure != 0) {
    return failure;
  }
  if (const int failure = testControllerPublishesAndEmptiesTheModel();
      failure != 0) {
    return failure;
  }
  if (const int failure = testRepeaterChurn(); failure != 0) return failure;
  return 0;
}
