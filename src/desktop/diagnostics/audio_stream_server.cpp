#include "audio_stream_server.hpp"

#include <QAbstractSocket>
#include <QByteArray>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonObject>
#include <QLatin1Char>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QUdpSocket>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cwassistant::desktop {
namespace {

// The address to actually send to, given the address the control connection
// reported.
//
// A socket bound to `::` accepts IPv4 peers and reports one as
// `::ffff:192.168.1.50`. That spelling is the same machine, but it is an IPv6
// address, and sending to it depends on a dual-stack send path that not every
// operating system offers on an unbound socket. Undone here for the same
// reason the allowed-peer rules undo it: the operator's network is IPv4, the
// peer is on it, and the mapped spelling is an artefact of how this station
// chose to listen rather than anything about the observer.
[[nodiscard]] QHostAddress sendableAddress(const QHostAddress& peer) {
  if (peer.protocol() != QAbstractSocket::IPv6Protocol) return peer;
  bool mapped = false;
  const quint32 inner = peer.toIPv4Address(&mapped);
  return mapped ? QHostAddress(inner) : peer;
}

// One float sample as the 16-bit value that goes on the wire.
//
// Clamped rather than wrapped. A sample that overshoots full scale is loud,
// and wrapping turns loud into a full-amplitude sign flip -- a click that is
// far more audible than the clipping it replaced, and one that would be read
// as a fault in the receiver rather than in the signal.
// How one bound endpoint is written for the status line, bracketing an IPv6
// address so the colon before the port is unambiguously the last one -- the
// same spelling the control listener's own endpoints use.
[[nodiscard]] QString endpointText(const QHostAddress& address,
                                   const quint16 port) {
  const QString text = address.toString();
  const QString host = text.contains(QLatin1Char(':'))
                           ? QStringLiteral("[%1]").arg(text)
                           : text;
  return QStringLiteral("%1:%2").arg(host).arg(port);
}

[[nodiscard]] qint16 pcmSample(const float sample) {
  if (!std::isfinite(sample)) return 0;
  const float clamped = std::clamp(sample, -1.0F, 1.0F);
  return static_cast<qint16>(std::lround(clamped * 32'767.0F));
}

}  // namespace

AudioStreamServer::AudioStreamServer(QObject* parent) : QObject(parent) {}

AudioStreamServer::~AudioStreamServer() {
  // The sockets are children of this object and Qt closes them. The subscriber list
  // is plain values and goes with the object. Nothing is announced: a
  // destructor is not the place to emit state nobody can act on.
  subscribers_.clear();
  subscriber_count_.store(0, std::memory_order_release);
  sending_.store(false, std::memory_order_release);
}

void AudioStreamServer::setEnabled(const bool enabled) {
  if (enabled_.load(std::memory_order_acquire) == enabled) return;
  enabled_.store(enabled, std::memory_order_release);
  // Revoked immediately, not at the end of whatever is in flight. An operator
  // switching this off is switching it off because of what is being sent now,
  // and a stream that finished politely would keep sending for exactly as long
  // as the peer it was being taken away from chose to listen. `rebind` drops
  // every subscription and, when the answer is no, leaves nothing bound: the
  // port is released too, because a port held for a disclosure that has been
  // withdrawn is a port nobody agreed to.
  rebind();
  emit stateChanged();
}

bool AudioStreamServer::enabled() const noexcept {
  return enabled_.load(std::memory_order_acquire);
}

QStringList AudioStreamServer::configure(const QList<Endpoint>& endpoints) {
  requested_endpoints_ = endpoints;
  rebind();
  emit stateChanged();
  return bind_notes_;
}

bool AudioStreamServer::listening() const noexcept {
  return !sockets_.isEmpty();
}

const QStringList& AudioStreamServer::bindNotes() const noexcept {
  return bind_notes_;
}

// Takes down every socket and puts up one per requested endpoint, or none at
// all while the operator has not allowed audio.
//
// Subscriptions go first and unconditionally. Each one names the socket its
// audio leaves by, and a rebind replaces those sockets; keeping a subscription
// across it would either aim audio at a freed socket or quietly move which
// address it left from. An observer whose stream ends because the service was
// rebound can ask again, which is the same answer the records already give it.
void AudioStreamServer::rebind() {
  unsubscribeAll();
  closeSockets();
  bind_notes_.clear();
  if (!enabled_.load(std::memory_order_acquire)) return;
  for (const Endpoint& endpoint : std::as_const(requested_endpoints_)) {
    auto* const socket = new QUdpSocket(this);
    if (!socket->bind(endpoint.address, endpoint.port)) {
      // Named, and the other endpoints still come up, exactly as a failed
      // control bind is handled. A service that offers audio and silently
      // cannot send it is the one failure this has to avoid.
      bind_notes_.append(
          QStringLiteral("Receive audio cannot be sent from %1: %2.")
              .arg(endpointText(endpoint.address, endpoint.port),
                   socket->errorString()));
      socket->deleteLater();
      continue;
    }
    // Bound, so something can be sent to this port. Everything that arrives is
    // taken off the device and dropped inside it -- a zero-length read
    // discards the payload without it ever becoming a value here. Drained
    // rather than ignored so that nothing can pile up, and never examined, so
    // there is nothing on this path for a parser to be attached to later.
    connect(socket, &QUdpSocket::readyRead, socket, [socket] {
      while (socket->hasPendingDatagrams()) {
        if (socket->readDatagram(nullptr, 0) < 0) break;
      }
    });
    sockets_.append(BoundSocket{.address = endpoint.address,
                                .port = endpoint.port,
                                .socket = socket});
  }
}

void AudioStreamServer::closeSockets() {
  for (const BoundSocket& bound : std::as_const(sockets_)) {
    if (bound.socket == nullptr) continue;
    bound.socket->disconnect();
    bound.socket->close();
    bound.socket->deleteLater();
  }
  sockets_.clear();
}

QUdpSocket* AudioStreamServer::socketFor(const QHostAddress& local_address,
                                         const std::uint16_t local_port) const {
  // The exact address this control connection arrived on, first. Audio should
  // leave by the address the observer connected to; anything else would reach
  // it from somewhere it has no reason to expect, and would be refused by any
  // receiver that checks.
  for (const BoundSocket& bound : sockets_) {
    if (bound.port != local_port) continue;
    if (bound.address.isEqual(local_address,
                              QHostAddress::ConvertV4MappedToIPv4)) {
      return bound.socket;
    }
  }
  // A wildcard bind names no single interface, so its socket is the right one
  // for every connection of its family that came in on that port. Matched on
  // the destination's family, because a socket bound to `0.0.0.0` cannot reach
  // an IPv6 peer.
  for (const BoundSocket& bound : sockets_) {
    if (bound.port != local_port) continue;
    const bool wildcard = bound.address == QHostAddress::Any ||
                          bound.address == QHostAddress::AnyIPv6;
    if (!wildcard) continue;
    if (bound.address.protocol() == local_address.protocol()) {
      return bound.socket;
    }
    // A socket bound to `::` accepts IPv4 too, which is exactly how the
    // control listener accepted this connection in the first place.
    if (bound.address == QHostAddress::AnyIPv6) return bound.socket;
  }
  return nullptr;
}

AudioStreamServer::SubscribeOutcome AudioStreamServer::subscribe(
    const std::uint64_t subscriber_id, const QHostAddress& peer,
    const QHostAddress& local_address, const std::uint16_t local_port) {
  // Asked first, and it is the only question whose answer a peer cannot
  // change. Everything below decides how to serve a request; this decides
  // whether there is anything to serve at all.
  if (!enabled_.load(std::memory_order_acquire)) {
    return SubscribeOutcome::NotEnabled;
  }
  for (const Subscriber& existing : std::as_const(subscribers_)) {
    if (existing.id == subscriber_id) return SubscribeOutcome::AlreadyStreaming;
  }
  if (subscribers_.size() >= kMaximumSubscribers) {
    return SubscribeOutcome::TooManySubscribers;
  }
  const QHostAddress destination = sendableAddress(peer);
  if (destination.isNull()) return SubscribeOutcome::NoDestination;
  // The port audio is sent to is the port this control connection is on. Not
  // configured separately and not nameable by a peer: one number for both
  // halves, which is the whole of the port design.
  if (local_port == 0) return SubscribeOutcome::NoDestination;
  QUdpSocket* const socket = socketFor(local_address, local_port);
  if (socket == nullptr) return SubscribeOutcome::NoDestination;
  subscribers_.append(Subscriber{.id = subscriber_id,
                                 .address = destination,
                                 .port = local_port,
                                 .socket = socket});
  subscriber_count_.store(static_cast<int>(subscribers_.size()),
                          std::memory_order_release);
  sending_.store(true, std::memory_order_release);
  emit stateChanged();
  return SubscribeOutcome::Started;
}

bool AudioStreamServer::unsubscribe(const std::uint64_t subscriber_id) {
  bool removed = false;
  for (qsizetype index = subscribers_.size() - 1; index >= 0; --index) {
    if (subscribers_.at(index).id != subscriber_id) continue;
    subscribers_.removeAt(index);
    removed = true;
  }
  if (!removed) return false;
  subscriber_count_.store(static_cast<int>(subscribers_.size()),
                          std::memory_order_release);
  if (subscribers_.isEmpty()) sending_.store(false, std::memory_order_release);
  emit stateChanged();
  return true;
}

void AudioStreamServer::unsubscribeAll() {
  if (subscribers_.isEmpty()) return;
  subscribers_.clear();
  subscriber_count_.store(0, std::memory_order_release);
  sending_.store(false, std::memory_order_release);
  emit stateChanged();
}

bool AudioStreamServer::hasSubscriber(const std::uint64_t subscriber_id) const {
  for (const Subscriber& existing : subscribers_) {
    if (existing.id == subscriber_id) return true;
  }
  return false;
}

int AudioStreamServer::subscriberCount() const noexcept {
  return subscriber_count_.load(std::memory_order_acquire);
}

QString AudioStreamServer::destinationFor(
    const std::uint64_t subscriber_id) const {
  for (const Subscriber& existing : subscribers_) {
    if (existing.id != subscriber_id) continue;
    return endpointText(existing.address, existing.port);
  }
  return {};
}

void AudioStreamServer::publishAudio(const QByteArray& float_mono_audio,
                                     const double sample_rate_hz) {
  // One atomic load is what a station with no observer pays per block of
  // audio. Everything below it costs something, so nothing below it runs until
  // there is somebody to send to and an operator who said this may happen.
  if (!sending_.load(std::memory_order_acquire)) return;
  if (float_mono_audio.isEmpty()) return;
  if (!(sample_rate_hz > 0.0)) return;
  if (sample_rate_hz > kMaximumSampleRateHz) {
    // Counted rather than truncated or resampled. A tap wired to an IQ stream
    // by mistake then reads as a number in the diagnostics record instead of
    // as silence nobody can explain.
    rate_refusals_.fetch_add(1, std::memory_order_acq_rel);
    return;
  }

  const qsizetype total_samples =
      float_mono_audio.size() / static_cast<qsizetype>(sizeof(float));
  if (total_samples <= 0) return;
  const auto rate = static_cast<quint32>(std::lround(sample_rate_hz));
  const auto captured_ms =
      static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

  for (qsizetype offset = 0; offset < total_samples;
       offset += kMaximumSamplesPerDatagram) {
    const qsizetype chunk =
        std::min<qsizetype>(kMaximumSamplesPerDatagram, total_samples - offset);

    // The bound, tested before a datagram is built rather than after. Refusing
    // here is what makes the memory ceiling real: nothing is allocated for
    // audio that is not going to be sent, so a receiver that stopped reading
    // -- or a thread that stopped draining -- costs at most the datagrams
    // already in flight and never one more.
    if (datagrams_in_flight_.load(std::memory_order_acquire) >=
        kMaximumDatagramsInFlight) {
      dropped_since_sent_.fetch_add(1, std::memory_order_acq_rel);
      dropped_total_.fetch_add(1, std::memory_order_acq_rel);
      continue;
    }

    // Taken and cleared together, so the count lands in exactly one header.
    // The receiver reads it as "this many datagrams were dropped by the
    // station immediately before this one", which is what lets a gap in a
    // remote recording be told apart from a gap the network caused.
    const QByteArray datagram = buildDatagram(
        float_mono_audio, offset, chunk, rate,
        sequence_.fetch_add(1, std::memory_order_acq_rel),
        dropped_since_sent_.exchange(0, std::memory_order_acq_rel),
        captured_ms);

    datagrams_in_flight_.fetch_add(1, std::memory_order_acq_rel);
    // Queued even when the caller is already on this object's thread, unlike
    // the record path beside it. The bound above only means anything if every
    // datagram passes through the same count, and a direct call from the
    // owning thread would leave it permanently at zero -- so the ceiling would
    // hold for the audio thread and not for a caller that happened to be the
    // one draining the queue. One posted event per datagram costs nothing next
    // to the send itself, and only ever runs while somebody is subscribed.
    QMetaObject::invokeMethod(
        this,
        [this, datagram] {
          sendDatagram(datagram);
          datagrams_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        },
        Qt::QueuedConnection);
  }
}

QByteArray AudioStreamServer::buildDatagram(
    const QByteArray& float_mono_audio, const qsizetype first_sample,
    const qsizetype sample_count, const quint32 sample_rate_hz,
    const quint32 sequence, const quint32 dropped_before,
    const quint64 captured_unix_ms) {
  QByteArray datagram(kHeaderBytes + (sample_count * 2), Qt::Uninitialized);
  char* const out = datagram.data();
  std::memcpy(out, kMagic, sizeof(kMagic));
  out[4] = static_cast<char>(kHeaderBytes);
  out[5] = static_cast<char>(kPayloadFormatPcm16BigEndian);
  qToBigEndian<quint16>(static_cast<quint16>(sample_count), out + 6);
  qToBigEndian<quint32>(sequence, out + 8);
  qToBigEndian<quint32>(sample_rate_hz, out + 12);
  qToBigEndian<quint32>(dropped_before, out + 16);
  qToBigEndian<quint64>(captured_unix_ms, out + 20);
  for (qsizetype index = 0; index < sample_count; ++index) {
    float sample = 0.0F;
    // Copied rather than reinterpreted. A QByteArray carries bytes, and
    // reading a float straight out of them is only safe where the allocator
    // happens to have aligned it; the copy compiles to the same load on every
    // platform this ships to and is correct on the ones where it would not.
    std::memcpy(&sample,
                float_mono_audio.constData() +
                    ((first_sample + index) *
                     static_cast<qsizetype>(sizeof(float))),
                sizeof(float));
    qToBigEndian<qint16>(pcmSample(sample), out + kHeaderBytes + (index * 2));
  }
  return datagram;
}

void AudioStreamServer::sendDatagram(const QByteArray& datagram) {
  for (const Subscriber& subscriber : std::as_const(subscribers_)) {
    if (subscriber.socket == nullptr) continue;
    const qint64 written = subscriber.socket->writeDatagram(
        datagram, subscriber.address, subscriber.port);
    if (written == datagram.size()) {
      sent_total_.fetch_add(1, std::memory_order_acq_rel);
      continue;
    }
    // Never retried, never queued for later. A datagram the operating system
    // would not take is a datagram whose moment has passed, and holding it
    // would be the queue this class exists not to have. The gap shows up at
    // the receiver as a missing sequence number, which is what a receiver
    // already has to cope with on UDP.
    send_failures_.fetch_add(1, std::memory_order_acq_rel);
  }
}

QJsonObject AudioStreamServer::statisticsRecord() const {
  QJsonObject record;
  record.insert(QStringLiteral("enabled"),
                enabled_.load(std::memory_order_acquire));
  record.insert(QStringLiteral("subscribers"), subscriberCount());
  // Counts datagrams put on the wire, so two subscribers receiving the same
  // audio count twice. That is the number that answers "what did this station
  // send", which is the question an operator reading the record is asking.
  record.insert(
      QStringLiteral("datagramsSent"),
      static_cast<qint64>(sent_total_.load(std::memory_order_acquire)));
  // The number the bound above exists to make visible. Non-zero means audio
  // was produced faster than it could be sent and was thrown away rather than
  // queued -- which is the designed behaviour, not a fault, and is why it is
  // reported rather than logged.
  record.insert(
      QStringLiteral("datagramsDropped"),
      static_cast<qint64>(dropped_total_.load(std::memory_order_acquire)));
  record.insert(
      QStringLiteral("sendFailures"),
      static_cast<qint64>(send_failures_.load(std::memory_order_acquire)));
  record.insert(
      QStringLiteral("sampleRateRefusals"),
      static_cast<qint64>(rate_refusals_.load(std::memory_order_acquire)));
  return record;
}

}  // namespace cwassistant::desktop
