// Guards the audio plane of the diagnostics service.
//
// This process holds transmit, and until now the class next door could promise
// that no byte from a peer selected anything. It no longer can: an observer
// asks for audio, and asking is selecting. What replaced that promise is
// narrower and every clause of it is pinned here -- that the set of requests is
// closed and literal, that a request reaches one switch and nothing else, that
// no request can create the operator's permission, and that a peer supplies
// neither half of the destination audio is sent to.
//
// The rest of these cases are about the other two faults this feature had to
// answer for. UDP has no backpressure, and a sender that emits at the rate
// audio is produced to a receiver that may be gone is how this application
// reached 668 MB and had to be killed; the bound is asserted as an exact
// number of datagrams, not as "roughly bounded". And audio is a larger
// disclosure than telemetry, so the cases that matter most are the ones where
// nothing is sent: consent off, consent withdrawn, control connection closed.
//
// Why nothing here opens a receiving socket. The audio port IS the control
// port -- UDP beside TCP on the same number -- so on one machine the station is
// already holding it and an observer cannot bind it. That is the documented
// consequence of the port design rather than a gap in the test: what actually
// goes on the wire is asserted byte for byte against `buildDatagram`, which is
// the one place the format is written, and everything else is asserted against
// the counters the station publishes in its own diagnostics record. A station
// with a subscriber on loopback sends to itself, which those counters still
// count, so "did anything go out" is answerable without a second machine.
//
// Distinct nonzero exit codes; each comment says what an operator loses.

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>

#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "diagnostics/audio_stream_server.hpp"
#include "diagnostics/diagnostics_server.hpp"

namespace {

using cwassistant::desktop::AudioStreamServer;
using cwassistant::desktop::DiagnosticsServer;

void pump(const int milliseconds) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < milliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  }
}

[[nodiscard]] QString testToken() {
  return QStringLiteral("0123456789abcdef");
}

[[nodiscard]] quint16 boundPort(const DiagnosticsServer& server) {
  const QStringList bound = server.boundAddresses();
  if (bound.isEmpty()) return 0;
  const int colon = bound.front().lastIndexOf(QLatin1Char(':'));
  return colon < 0 ? 0
                   : static_cast<quint16>(bound.front().mid(colon + 1).toUInt());
}

// Brings the service up on loopback, on a port the operating system chose.
[[nodiscard]] bool openService(DiagnosticsServer& server,
                               const QString& token) {
  server.configure({QStringLiteral("127.0.0.1")}, 0, token);
  server.setEnabled(true);
  pump(200);
  return boundPort(server) != 0;
}

// One complete line from the control connection, or nothing.
[[nodiscard]] std::optional<QJsonObject> readResponse(QTcpSocket& socket,
                                                      QByteArray& carry,
                                                      const int milliseconds) {
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < milliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    carry.append(socket.readAll());
    const qsizetype end = carry.indexOf('\n');
    if (end < 0) continue;
    const QByteArray line = carry.left(end);
    carry.remove(0, end + 1);
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) return std::nullopt;
    return document.object();
  }
  return std::nullopt;
}

// Connects, authenticates, and hands back the greeting.
[[nodiscard]] std::optional<QJsonObject> connectAndAuthenticate(
    QTcpSocket& socket, QByteArray& carry, const quint16 control_port,
    const QString& token) {
  socket.connectToHost(QHostAddress::LocalHost, control_port);
  pump(300);
  socket.write(token.toUtf8() + '\n');
  socket.flush();
  return readResponse(socket, carry, 800);
}

// What the station says it has done with audio, out of the diagnostics record
// it publishes -- which is where an operator reads it too.
[[nodiscard]] std::optional<QJsonObject> audioStatistics(
    DiagnosticsServer& server, QTcpSocket& observer, QByteArray& carry) {
  QJsonObject probe;
  probe.insert(QStringLiteral("event"), QStringLiteral("probe"));
  server.publish(probe);
  const std::optional<QJsonObject> record = readResponse(observer, carry, 800);
  if (!record) return std::nullopt;
  if (!record->contains(QStringLiteral("audioStream"))) return std::nullopt;
  return record->value(QStringLiteral("audioStream")).toObject();
}

// Mono float samples, as the decoder hands its audio out.
[[nodiscard]] QByteArray floatBlock(const std::vector<float>& samples) {
  QByteArray bytes;
  bytes.resize(static_cast<qsizetype>(samples.size() * sizeof(float)));
  std::memcpy(bytes.data(), samples.data(), samples.size() * sizeof(float));
  return bytes;
}

// ---------------------------------------------------------------------------

// The wire format, read back field by field exactly as the class header
// describes it. Written out longhand rather than through a shared decoder so
// that changing the layout has to be done twice, once in the sender and once
// here, and so the header comment and the bytes cannot quietly disagree.
bool theWireFormatIsWhatTheHeaderSaysItIs() {
  // Full scale, negative full scale, silence, and a sample past full scale
  // that has to clip rather than wrap: a wrapped overshoot is a sign flip and
  // sounds like a fault in the receiver rather than like a loud signal.
  const QByteArray audio = floatBlock({0.0F, 1.0F, -1.0F, 2.0F});
  const QByteArray datagram = AudioStreamServer::buildDatagram(
      audio, 0, 4, 8'000, 7, 3, 1'700'000'000'123ULL);

  if (datagram.size() != AudioStreamServer::kHeaderBytes + 8) return false;
  const auto* raw = reinterpret_cast<const uchar*>(datagram.constData());
  if (datagram.left(4) != QByteArrayLiteral("CWA1")) return false;
  if (raw[4] != AudioStreamServer::kHeaderBytes) return false;
  if (raw[5] != AudioStreamServer::kPayloadFormatPcm16BigEndian) return false;
  if (qFromBigEndian<quint16>(raw + 6) != 4) return false;
  if (qFromBigEndian<quint32>(raw + 8) != 7) return false;
  if (qFromBigEndian<quint32>(raw + 12) != 8'000) return false;
  if (qFromBigEndian<quint32>(raw + 16) != 3) return false;
  if (qFromBigEndian<quint64>(raw + 20) != 1'700'000'000'123ULL) return false;
  const std::vector<qint16> expected{0, 32'767, -32'767, 32'767};
  for (int index = 0; index < 4; ++index) {
    if (qFromBigEndian<qint16>(
            raw + AudioStreamServer::kHeaderBytes + (index * 2)) !=
        expected.at(static_cast<std::size_t>(index))) {
      return false;
    }
  }
  // A datagram never exceeds what fits inside the smallest path anything
  // routes. Fragmentation is worth avoiding rather than tolerating: one lost
  // fragment destroys the whole datagram, so it turns one lost packet into
  // several lost milliseconds.
  const QByteArray full = floatBlock(std::vector<float>(
      static_cast<std::size_t>(AudioStreamServer::kMaximumSamplesPerDatagram),
      0.125F));
  const QByteArray largest = AudioStreamServer::buildDatagram(
      full, 0, AudioStreamServer::kMaximumSamplesPerDatagram, 48'000, 0, 0, 1);
  return largest.size() == AudioStreamServer::kMaximumDatagramBytes &&
         largest.size() <= 1'280;
}

// The greeting is the only description of this protocol a peer ever gets, so
// it has to be the true one. It used to promise that nothing sent would be
// read; that promise is gone, the version moved so a reader written against
// the old one refuses this stream instead of trusting it, the whole closed set
// is named, and the audio port it advertises is the control port itself.
bool theGreetingWithdrawsTheEmitOnlyPromise() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  const quint16 port = boundPort(server);

  QTcpSocket observer;
  QByteArray carry;
  const std::optional<QJsonObject> greeting =
      connectAndAuthenticate(observer, carry, port, testToken());
  server.setEnabled(false);
  if (!greeting) return false;
  if (greeting->value(QStringLiteral("event")).toString() !=
      QStringLiteral("diagnostics-stream-open")) {
    return false;
  }
  // The promise this service no longer keeps must not still be claimed.
  if (greeting->value(QStringLiteral("emitOnly")).toBool(true)) return false;
  if (greeting->value(QStringLiteral("protocol")).toInt() < 2) return false;
  const QJsonArray requests =
      greeting->value(QStringLiteral("requests")).toArray();
  if (requests.size() != 2) return false;
  if (requests.at(0).toString() != QStringLiteral("start-audio-stream")) {
    return false;
  }
  if (requests.at(1).toString() != QStringLiteral("stop-audio-stream")) {
    return false;
  }
  // Enough of the format for a receiver to be written from the stream alone,
  // and the port, which is this connection's own.
  const QJsonObject audio = greeting->value(QStringLiteral("audio")).toObject();
  if (audio.value(QStringLiteral("enabled")).toBool(true)) return false;
  if (audio.value(QStringLiteral("port")).toInt() != port) return false;
  if (audio.value(QStringLiteral("transport")).toString() !=
      QStringLiteral("udp")) {
    return false;
  }
  if (audio.value(QStringLiteral("magic")).toString() !=
      QStringLiteral("CWA1")) {
    return false;
  }
  return audio.value(QStringLiteral("headerBytes")).toInt() ==
         AudioStreamServer::kHeaderBytes;
}

// THE CASE THIS FEATURE EXISTS TO NOT FAIL. An observer that holds the token,
// is on the allowed list, and asks correctly still gets nothing, because the
// operator has not said audio may leave the station. Asking does not turn it
// on, and asking twice does not either.
bool aPeerCannotStartAudioTheOperatorHasNotAllowed() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  if (server.audioStreamingEnabled()) return false;

  QTcpSocket observer;
  QByteArray carry;
  if (!connectAndAuthenticate(observer, carry, boundPort(server),
                              testToken())) {
    return false;
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    observer.write(QByteArrayLiteral("start-audio-stream\n"));
    observer.flush();
    const std::optional<QJsonObject> answer = readResponse(observer, carry, 800);
    if (!answer) return false;
    if (answer->value(QStringLiteral("event")).toString() !=
        QStringLiteral("audio-stream-refused")) {
      return false;
    }
    // WHICH refusal, not just that it was one. Two independent things stop
    // audio here -- the consent gate, and the fact that nothing is bound
    // until there is consent -- and a test that accepted either would pass
    // with the consent gate deleted. It also matters to whoever is holding
    // the remote client: "the operator has not allowed this" and "this
    // station could not bind its port" are different problems.
    if (!answer->value(QStringLiteral("reason"))
             .toString()
             .contains(QStringLiteral("has not allowed"))) {
      return false;
    }
    if (server.audioSubscriberCount() != 0) return false;
  }
  // A refusal is not a disconnection: the peer did nothing wrong, and an
  // observer that loses its records for asking would be a worse answer than
  // saying no.
  if (server.clientCount() != 1) return false;

  // And nothing went out. This is the assertion that matters: the subscriber
  // count could be wrong in a way that still sent audio.
  server.publishAudio(floatBlock({0.5F, -0.5F, 0.25F, -0.25F}), 8'000.0);
  pump(200);
  const std::optional<QJsonObject> audio =
      audioStatistics(server, observer, carry);
  server.setEnabled(false);
  if (!audio) return false;
  if (audio->value(QStringLiteral("enabled")).toBool(true)) return false;
  return audio->value(QStringLiteral("datagramsSent")).toInt() == 0;
}

// Once the operator has allowed it, a request is honoured, the station says
// where it is sending, and that destination is the peer's own address on this
// same port. Neither half of it came from the peer.
bool theDestinationIsThePeersOwnAddressOnThisPort() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  const quint16 port = boundPort(server);
  server.setAudioStreamingEnabled(true);
  if (!server.audioListening()) return false;

  QTcpSocket observer;
  QByteArray carry;
  if (!connectAndAuthenticate(observer, carry, port, testToken())) return false;
  observer.write(QByteArrayLiteral("start-audio-stream\n"));
  observer.flush();
  const std::optional<QJsonObject> answer = readResponse(observer, carry, 800);
  if (!answer) return false;
  if (answer->value(QStringLiteral("event")).toString() !=
      QStringLiteral("audio-stream-started")) {
    return false;
  }
  // THE PORT DESIGN, ASSERTED. The same number as the control connection, not
  // one beside it and not one a peer could have named.
  if (answer->value(QStringLiteral("destination")).toString() !=
      QStringLiteral("127.0.0.1:%1").arg(port)) {
    return false;
  }
  if (server.audioSubscriberCount() != 1) return false;

  server.publishAudio(floatBlock({0.0F, 1.0F, -1.0F, 0.5F}), 8'000.0);
  pump(300);
  const std::optional<QJsonObject> audio =
      audioStatistics(server, observer, carry);
  server.setEnabled(false);
  if (!audio) return false;
  if (!audio->value(QStringLiteral("enabled")).toBool(false)) return false;
  return audio->value(QStringLiteral("datagramsSent")).toInt() == 1 &&
         audio->value(QStringLiteral("sendFailures")).toInt() == 0;
}

// Three ways a stream must end, and the one that matters most is the last: a
// stream that outlived its control connection would be a stream nobody could
// stop, because the connection is the only handle on it.
bool everyWayOfStoppingActuallyStops() {
  // Asked for and then stopped.
  {
    DiagnosticsServer server;
    if (!openService(server, testToken())) return false;
    server.setAudioStreamingEnabled(true);
    QTcpSocket observer;
    QByteArray carry;
    if (!connectAndAuthenticate(observer, carry, boundPort(server),
                                testToken())) {
      return false;
    }
    observer.write(QByteArrayLiteral("start-audio-stream\n"));
    observer.flush();
    if (!readResponse(observer, carry, 800)) return false;
    if (server.audioSubscriberCount() != 1) return false;
    observer.write(QByteArrayLiteral("stop-audio-stream\n"));
    observer.flush();
    const std::optional<QJsonObject> stopped =
        readResponse(observer, carry, 800);
    if (!stopped) return false;
    if (stopped->value(QStringLiteral("event")).toString() !=
        QStringLiteral("audio-stream-stopped")) {
      return false;
    }
    if (server.audioSubscriberCount() != 0) return false;
    server.publishAudio(floatBlock({0.5F, 0.5F}), 8'000.0);
    pump(200);
    const std::optional<QJsonObject> audio =
        audioStatistics(server, observer, carry);
    server.setEnabled(false);
    if (!audio) return false;
    if (audio->value(QStringLiteral("datagramsSent")).toInt() != 0) return false;
  }

  // The operator takes the permission back while it is running.
  {
    DiagnosticsServer server;
    if (!openService(server, testToken())) return false;
    server.setAudioStreamingEnabled(true);
    QTcpSocket observer;
    QByteArray carry;
    if (!connectAndAuthenticate(observer, carry, boundPort(server),
                                testToken())) {
      return false;
    }
    observer.write(QByteArrayLiteral("start-audio-stream\n"));
    observer.flush();
    if (!readResponse(observer, carry, 800)) return false;
    if (server.audioSubscriberCount() != 1) return false;
    server.setAudioStreamingEnabled(false);
    if (server.audioSubscriberCount() != 0) return false;
    // The port goes with the permission: one held open for a disclosure the
    // operator has withdrawn is a port nobody agreed to.
    if (server.audioListening()) return false;
    server.publishAudio(floatBlock({0.5F, 0.5F}), 8'000.0);
    pump(200);
    const std::optional<QJsonObject> audio =
        audioStatistics(server, observer, carry);
    server.setEnabled(false);
    if (!audio) return false;
    if (audio->value(QStringLiteral("datagramsSent")).toInt() != 0) return false;
  }

  // The control connection goes away without saying anything.
  {
    DiagnosticsServer server;
    if (!openService(server, testToken())) return false;
    server.setAudioStreamingEnabled(true);
    QTcpSocket observer;
    QByteArray carry;
    if (!connectAndAuthenticate(observer, carry, boundPort(server),
                                testToken())) {
      return false;
    }
    observer.write(QByteArrayLiteral("start-audio-stream\n"));
    observer.flush();
    if (!readResponse(observer, carry, 800)) return false;
    if (server.audioSubscriberCount() != 1) return false;
    observer.abort();
    pump(400);
    const bool released = server.audioSubscriberCount() == 0;
    server.setEnabled(false);
    if (!released) return false;
  }
  return true;
}

// The set is closed, and closed means these are not near misses that get a
// helpful answer -- they end the connection. Every one of them is a line a
// grammar would have accepted: a verb with an argument, a different case, a
// truncation, a blank line, and the request wrapped in JSON.
bool anythingOutsideTheClosedSetEndsTheConnection() {
  const QList<QByteArray> rejected = {
      QByteArrayLiteral("start-audio-stream now\n"),
      QByteArrayLiteral("START-AUDIO-STREAM\n"),
      QByteArrayLiteral("start-audio-strea\n"),
      QByteArrayLiteral("start-audio-streams\n"),
      QByteArrayLiteral("\n"),
      QByteArrayLiteral("{\"request\":\"start-audio-stream\"}\n"),
  };
  for (const QByteArray& line : rejected) {
    DiagnosticsServer server;
    if (!openService(server, testToken())) return false;
    // Allowed, deliberately: the point is that permission is not what stops
    // these, the closed set is.
    server.setAudioStreamingEnabled(true);
    QTcpSocket observer;
    QByteArray carry;
    if (!connectAndAuthenticate(observer, carry, boundPort(server),
                                testToken())) {
      return false;
    }
    observer.write(line);
    observer.flush();
    pump(500);
    const bool dropped = server.clientCount() == 0;
    const bool no_audio = server.audioSubscriberCount() == 0;
    server.setEnabled(false);
    pump(50);
    if (!dropped || !no_audio) return false;
  }
  return true;
}

// A peer that has not presented the token is not read for requests at all: its
// bytes go to the token comparison, fail it, and end the connection. There is
// no ordering in which a request is considered before authentication.
bool anUnauthenticatedPeerCannotStartAudio() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  server.setAudioStreamingEnabled(true);

  QTcpSocket observer;
  observer.connectToHost(QHostAddress::LocalHost, boundPort(server));
  pump(300);
  if (server.clientCount() != 1) return false;
  observer.write(QByteArrayLiteral("start-audio-stream\n"));
  observer.flush();
  pump(500);
  const bool dropped = server.clientCount() == 0;
  const bool no_audio = server.audioSubscriberCount() == 0;
  server.setEnabled(false);
  return dropped && no_audio;
}

// THE 668 MB CASE. Audio is offered far faster than the sender can drain it,
// which is what happens when a receiver stops reading or the thread that sends
// is blocked. Nothing may be buffered for it: at most the in-flight bound
// exists at once, and everything past it is dropped and counted, so the
// operator can see that it happened rather than watching memory climb.
bool theSenderIsBoundedAndCountsWhatItDropped() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  server.setAudioStreamingEnabled(true);

  QTcpSocket observer;
  QByteArray carry;
  if (!connectAndAuthenticate(observer, carry, boundPort(server),
                              testToken())) {
    return false;
  }
  observer.write(QByteArrayLiteral("start-audio-stream\n"));
  observer.flush();
  if (!readResponse(observer, carry, 800)) return false;
  if (server.audioSubscriberCount() != 1) return false;

  // Offered without ever returning to the event loop, which is exactly the
  // shape of the fault: the producer runs on and the sender never gets a turn.
  // Ten times the bound, so the assertion below is not near the edge.
  const int offered = AudioStreamServer::kMaximumDatagramsInFlight * 10;
  for (int index = 0; index < offered; ++index) {
    server.publishAudio(floatBlock({0.25F, 0.25F, 0.25F, 0.25F}), 8'000.0);
  }
  pump(600);

  const std::optional<QJsonObject> audio =
      audioStatistics(server, observer, carry);
  server.setEnabled(false);
  if (!audio) return false;
  // The bound, as an exact number rather than as "not too many". Every one of
  // these was offered before the event loop ran once, so no datagram could
  // have been sent and its slot reused: what went out is the ceiling.
  const int sent = audio->value(QStringLiteral("datagramsSent")).toInt();
  if (sent <= 0) return false;
  if (sent > AudioStreamServer::kMaximumDatagramsInFlight) return false;
  // And the rest was thrown away rather than queued, which is the whole point.
  return audio->value(QStringLiteral("datagramsDropped")).toInt() ==
         offered - sent;
}

// An IQ passband is not audio. Wired to one by mistake this would be megabytes
// a second aimed at whatever asked for it, so it is refused at the rate check
// and counted, which is the difference between a wrong tap that shows up as a
// number and one that shows up as silence nobody can explain.
bool anIqRateIsRefusedRatherThanStreamed() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  server.setAudioStreamingEnabled(true);

  QTcpSocket observer;
  QByteArray carry;
  if (!connectAndAuthenticate(observer, carry, boundPort(server),
                              testToken())) {
    return false;
  }
  observer.write(QByteArrayLiteral("start-audio-stream\n"));
  observer.flush();
  if (!readResponse(observer, carry, 800)) return false;

  server.publishAudio(floatBlock({0.5F, 0.5F, 0.5F, 0.5F}), 2'400'000.0);
  pump(200);
  const std::optional<QJsonObject> audio =
      audioStatistics(server, observer, carry);
  server.setEnabled(false);
  if (!audio) return false;
  if (audio->value(QStringLiteral("datagramsSent")).toInt() != 0) return false;
  return audio->value(QStringLiteral("sampleRateRefusals")).toInt() > 0;
}

// Nothing is sent for a subscription that was never made. The consent switch
// alone changes nothing: it permits, it does not start.
bool allowingAudioDoesNotByItselfSendAny() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  server.setAudioStreamingEnabled(true);
  if (server.audioSubscriberCount() != 0) return false;

  QTcpSocket observer;
  QByteArray carry;
  if (!connectAndAuthenticate(observer, carry, boundPort(server),
                              testToken())) {
    return false;
  }
  server.publishAudio(floatBlock({0.5F, -0.5F}), 8'000.0);
  pump(200);
  const std::optional<QJsonObject> audio =
      audioStatistics(server, observer, carry);
  server.setEnabled(false);
  if (!audio) return false;
  return audio->value(QStringLiteral("datagramsSent")).toInt() == 0;
}

// A client that writes its token and its first request in one call keeps the
// request. Ordinary for anything scripted, and losing it silently would be a
// trap: the connection would come up, the peer would wait, and no audio would
// ever arrive with nothing anywhere saying why.
bool aRequestPipelinedWithTheTokenIsKept() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  server.setAudioStreamingEnabled(true);

  QTcpSocket observer;
  observer.connectToHost(QHostAddress::LocalHost, boundPort(server));
  pump(300);
  observer.write(testToken().toUtf8() + "\nstart-audio-stream\n");
  observer.flush();
  QByteArray carry;
  // The greeting first, then the answer to the request that came in with it.
  if (!readResponse(observer, carry, 800)) return false;
  const std::optional<QJsonObject> answer = readResponse(observer, carry, 800);
  if (!answer) return false;
  if (answer->value(QStringLiteral("event")).toString() !=
      QStringLiteral("audio-stream-started")) {
    return false;
  }
  const bool subscribed = server.audioSubscriberCount() == 1;
  server.setEnabled(false);
  return subscribed;
}

// But only one line of it. A peer that pipelines more than a request's worth
// ahead of a greeting it has not read is not following this protocol, and is
// disconnected rather than having part of what it sent acted on.
bool anOverlongPipelineEndsTheConnection() {
  DiagnosticsServer server;
  if (!openService(server, testToken())) return false;
  server.setAudioStreamingEnabled(true);

  QTcpSocket observer;
  observer.connectToHost(QHostAddress::LocalHost, boundPort(server));
  pump(300);
  // A perfectly good request buried in a pipeline that is too long. None of it
  // may be acted on: acting on the part that fits would mean a peer could get
  // a request honoured by burying it, and the rule would be about where the
  // bytes fell rather than about what the peer sent.
  observer.write(testToken().toUtf8() + "\nstart-audio-stream\n" +
                 QByteArray(static_cast<int>(
                                DiagnosticsServer::kMaximumRequestBytes),
                            'x'));
  observer.flush();
  pump(500);
  const bool dropped = server.clientCount() == 0;
  const bool no_audio = server.audioSubscriberCount() == 0;
  server.setEnabled(false);
  return dropped && no_audio;
}

// An audio half that could not be bound must never read as a service that is
// up, and must say why.
//
// Forced by holding the UDP port before the station starts, which is the only
// way to get the one state worth testing: the control half listening and the
// audio half not. TCP and UDP are different namespaces, so the control
// listener comes up on that number perfectly happily and the audio listener
// cannot -- exactly what a second copy of this application, or anything else
// on that port, would produce on an operator's machine.
bool anAudioPortThatCannotBindIsNotReportedAsReady() {
  for (int attempt = 0; attempt < 24; ++attempt) {
    QUdpSocket blocker;
    if (!blocker.bind(QHostAddress::LocalHost, 0)) continue;
    const quint16 port = blocker.localPort();
    if (port < 1025) continue;

    DiagnosticsServer server;
    server.setAudioStreamingEnabled(true);
    server.configure({QStringLiteral("127.0.0.1")},
                     static_cast<std::uint16_t>(port), testToken());
    server.setEnabled(true);
    pump(300);
    // The control half has to be up, or this is testing nothing.
    if (boundPort(server) != port) continue;

    const bool audio_refused = !server.audioListening();
    // And it is reported the way a failed control bind is, in the same status
    // line, because an operator asking "is this service up" gets one answer
    // and half of it being up is not a yes.
    const bool explained = server.statusMessage().contains(
        QStringLiteral("Receive audio cannot be sent from 127.0.0.1:%1")
            .arg(port));

    // A peer that asks is told the truth rather than being started into a
    // stream that cannot carry anything.
    QTcpSocket observer;
    QByteArray carry;
    bool told = false;
    if (connectAndAuthenticate(observer, carry, port, testToken())) {
      observer.write(QByteArrayLiteral("start-audio-stream\n"));
      observer.flush();
      const std::optional<QJsonObject> answer =
          readResponse(observer, carry, 800);
      told = answer &&
             answer->value(QStringLiteral("event")).toString() ==
                 QStringLiteral("audio-stream-refused") &&
             answer->value(QStringLiteral("reason"))
                 .toString()
                 .contains(QStringLiteral("could not bind"));
    }
    const bool no_audio = server.audioSubscriberCount() == 0;
    server.setEnabled(false);
    pump(50);
    return audio_refused && explained && told && no_audio;
  }
  std::printf(
      "diagnostics audio stream: no port could be held for the failed-bind "
      "case, that check skipped\n");
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  if (!theWireFormatIsWhatTheHeaderSaysItIs()) return 40;
  if (!theGreetingWithdrawsTheEmitOnlyPromise()) return 41;
  if (!aPeerCannotStartAudioTheOperatorHasNotAllowed()) return 42;
  if (!theDestinationIsThePeersOwnAddressOnThisPort()) return 43;
  if (!everyWayOfStoppingActuallyStops()) return 44;
  if (!anythingOutsideTheClosedSetEndsTheConnection()) return 45;
  if (!anUnauthenticatedPeerCannotStartAudio()) return 46;
  if (!theSenderIsBoundedAndCountsWhatItDropped()) return 47;
  if (!anIqRateIsRefusedRatherThanStreamed()) return 48;
  if (!allowingAudioDoesNotByItselfSendAny()) return 49;
  if (!aRequestPipelinedWithTheTokenIsKept()) return 50;
  if (!anOverlongPipelineEndsTheConnection()) return 51;
  if (!anAudioPortThatCannotBindIsNotReportedAsReady()) return 52;
  std::printf("diagnostics audio stream: all checks passed\n");
  return 0;
}
