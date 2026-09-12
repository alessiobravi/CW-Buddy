#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>

class QUdpSocket;

namespace cwassistant::desktop {

// The media plane of the diagnostics service: receive audio, carried to an
// already-authenticated observer over UDP.
//
// Why UDP, and why a second socket at all. A retransmitted audio sample is
// worthless by the time it arrives, so the reliability the control connection
// gives is not merely unnecessary here, it is harmful: it converts loss into
// delay, and delay in a stream somebody is listening to is the one failure
// they cannot ignore. This is the shape RTSP and SIP are built in -- control
// on the authenticated connection, media beside it -- and it is the right one
// here for the same reason.
//
// WHAT AN OPERATOR IS CHOOSING. Audio is a larger disclosure than the
// diagnostics records beside it. A record says what this application decoded;
// the audio carries every signal in the passband, decoded or not, including
// whatever else the receiver happens to be hearing. It is therefore off until
// the operator says otherwise, the main window shows an indicator for as long
// as anything is being sent, and NO REQUEST FROM A PEER CAN TURN IT ON. A peer
// may ask for a stream the operator has already allowed; it cannot create the
// permission. Those are different things and this class never confuses them.
//
// ONE PORT, TWO PROTOCOLS. The audio uses the same port number as the
// diagnostics listener it belongs to: TCP/17300 carries the control connection
// and the records, UDP/17300 carries the audio. They are different protocols,
// so there is no conflict, and `DiagnosticsServer::kDefaultPort` stays the one
// number either half is configured by. An operator moving the service in
// Network settings moves both together, reasons about one number, and opens
// one hole in a firewall; and the pairing is self-evident, because the audio
// for a diagnostics stream on 17300 arrives on 17300.
//
// The socket is bound to that port on the same addresses the control listener
// bound, so the audio leaves from the port it arrives on. A bind that fails is
// reported in the status line exactly as a failed control bind is: a service
// that came up without the audio half it says it offers must not look like a
// service that is fully up.
//
// The consequence worth stating: an observer on the SAME machine as the
// station cannot receive, because the station is already holding that port
// there. Audio streaming is for a remote observer, which is what it is for
// anyway, and the alternative -- a second port number, or one a peer nominates
// -- costs more than that case is worth.
//
// THIS IS NOT A REFLECTOR. A subscriber's destination is never taken from
// anything a peer said. The address is the one the authenticated control
// connection arrived from, read off that socket, and the port is the port that
// connection is on. A peer can ask that audio be sent to itself and has no way
// to spell anything else, so this cannot be pointed at a third party and
// cannot amplify traffic toward one.
//
// BOUNDED BY CONSTRUCTION, BECAUSE UDP HAS NO BACKPRESSURE. A sender that
// emits at the rate audio is produced, to a receiver that may have vanished,
// is the fault that grew this application to 668 MB and forced a kill, with
// the queue moved into a socket. There is no queue here that a stalled
// receiver can grow: at most `kMaximumDatagramsInFlight` datagrams of at most
// `kMaximumDatagramBytes` each exist at once -- about 32 kB, whatever happens
// downstream -- and audio offered past that bound is dropped, never buffered.
// What was dropped is counted, and the count travels two ways: to the
// operator, in the diagnostics record this service publishes, and to the
// receiver, in the header of the next datagram that does go out, so a gap in a
// remote recording can be told apart from a gap the network caused.
//
// The socket is bound, so something can be sent to it, and what arrives is
// DISCARDED WITHOUT BEING LOOKED AT. Each datagram is taken off the device with
// a zero-length read, which drops the payload inside the socket and never hands
// it to this class as a value -- the same discipline the control plane uses for
// a refused peer. It is drained rather than left to pile up so that no buffer
// anywhere can grow, and it is never examined, so there is no buffer of peer
// input here for a parser to be attached to later. Nothing that arrives on this
// port can start, stop or change anything; the only way to affect this class
// from outside is the closed request set on the control connection.
//
// THE WIRE FORMAT. Every datagram is a 28-byte header in network byte order
// followed by mono signed 16-bit PCM, also big-endian:
//
//   offset  size  field
//   0       4     magic, the ASCII bytes "CWA1"
//   4       1     header length in bytes (28)
//   5       1     payload format: 1 = mono signed 16-bit PCM, big-endian
//   6       2     sample count in this datagram
//   8       4     sequence number, +1 per datagram sent, wraps at 2^32
//   12      4     sample rate in Hz
//   16      4     datagrams this station dropped before this one
//   20      8     capture time, milliseconds since the Unix epoch
//   28      ...   sample count x 2 bytes of PCM
//
// Chosen so an operator can decode it without a tool written for it. The
// header is fixed-length and self-describing -- the sample rate travels with
// the audio rather than having to be agreed out of band -- so stripping 28
// bytes from each datagram leaves a stream `sox -t s16 -b 16 -e signed -B -c 1
// -r <rate>` or ffmpeg's `s16be` reads directly. The header length is in the
// header so that a later version may grow it and a reader written against this
// one still finds the payload.
//
// RTP was considered and declined. It would have been playable in VLC, but
// only with an SDP file agreed separately, which is exactly the out-of-band
// step this header avoids; it has no field for the station-side drop count,
// which is the number this design exists to make visible; and it brings a
// session protocol -- SSRC collision handling, RTCP -- which is protocol
// surface this service does not want near a process that holds transmit.
//
// 16-bit rather than float: every common tool reads it without argument, it is
// half the wire cost of the float32 the decoder hands its audio out in, and
// nothing audible is lost -- which matters, because the stream this carries is
// the whole decode region rather than one track. Mono because receive audio is
// one channel.
//
// WHAT IS EXPECTED ON THIS PATH: mono, 48 kHz, the whole decode region
// demodulated to audio by one producer that also feeds the local monitor. The
// decode region is capped at 24 kHz for exactly that reason -- 48 kHz audio
// carries 24 kHz of bandwidth and no more, so the region an operator decodes
// is the region they can hear, with no second concept between them. At 48 kHz
// 16-bit that is about 768 kbit/s, which is real for an uplink and is accepted
// as the cost of sending PCM.
//
// Neither the rate nor the format is assumed anywhere: both travel in every
// datagram's header. That is what leaves room for a compressed payload later
// (REM-006, Opus) as another value of the one-byte format field, with no
// protocol break and no negotiation -- a receiver reads the byte and knows.
// NOTHING HERE IMPLEMENTS THAT, and this class takes no dependency for it: the
// room is the field, and the field costs one byte.
class AudioStreamServer final : public QObject {
  Q_OBJECT

 public:
  explicit AudioStreamServer(QObject* parent = nullptr);
  ~AudioStreamServer() override;

  // One address and port the control listener actually came up on, so the
  // audio half can be bound to exactly the same place.
  struct Endpoint {
    QHostAddress address;
    std::uint16_t port{0};
  };

  // Binds the audio half on the same endpoints the control listener bound.
  //
  // Takes the endpoints that actually came up rather than the ones an operator
  // asked for, so the two halves can never be bound to different places, and
  // takes the port with each address rather than one port for all of them,
  // because a service configured onto port 0 gets a different ephemeral port
  // per address and "the same port" has to mean the same port as THAT address's
  // listener.
  //
  // Nothing is bound while the operator has not allowed audio: a port is not
  // held before there is consent for the thing that port is for. Both this and
  // `setEnabled` therefore rebind, and both drop every subscription first,
  // because a subscription names a socket that is about to be replaced.
  //
  // Returns one sentence per endpoint that could not be bound, for the status
  // line. Empty when they all came up.
  QStringList configure(const QList<Endpoint>& endpoints);

  // The operator's consent, and the only thing that can grant it. Switching it
  // off stops every subscription immediately rather than letting the ones
  // already running finish: an operator revoking this is revoking it because
  // of what is being sent now, and it closes the port, because a port held
  // open for a disclosure the operator has withdrawn is a port nobody agreed
  // to.
  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept;
  // Whether the audio half is actually able to send. False while the operator
  // has not allowed it, and false when it was allowed but nothing could be
  // bound -- which is the state that must never read as a working service.
  [[nodiscard]] bool listening() const noexcept;
  // Why the audio half is not fully up, as sentences for the status line.
  [[nodiscard]] const QStringList& bindNotes() const noexcept;

  // Why a subscription request was answered the way it was. A closed set,
  // because the control plane turns each of these into one fixed sentence and
  // there is nothing else it can say.
  enum class SubscribeOutcome {
    Started,
    AlreadyStreaming,
    NotEnabled,
    TooManySubscribers,
    // There is nowhere to send: the control connection reported no usable peer
    // address, or it is on a port with no port above it. Never a peer's fault
    // and never something a peer can influence, because a peer supplies
    // neither half of the destination.
    NoDestination,
  };

  // Starts sending to `peer` for the control connection identified by
  // `subscriber_id`.
  //
  // `local_address` and `local_port` are what that control connection reports
  // about THIS end of itself, not anything a peer said. They pick the bound
  // socket the audio leaves by -- so it leaves from the address the observer
  // connected to, which is the one it will be expecting -- and `local_port` is
  // also the port the audio is sent to, which is what makes the two halves one
  // number.
  SubscribeOutcome subscribe(std::uint64_t subscriber_id,
                             const QHostAddress& peer,
                             const QHostAddress& local_address,
                             std::uint16_t local_port);
  // Stops one subscription. Silent when there was none, because the control
  // plane calls this on every disconnection and most connections never asked
  // for audio.
  bool unsubscribe(std::uint64_t subscriber_id);
  void unsubscribeAll();
  [[nodiscard]] bool hasSubscriber(std::uint64_t subscriber_id) const;
  [[nodiscard]] int subscriberCount() const noexcept;
  // Where one subscription is being sent, as `address:port`, for the status
  // line and for the line the control plane writes back to the peer.
  [[nodiscard]] QString destinationFor(std::uint64_t subscriber_id) const;

  // Receive audio, as mono 32-bit float samples in native byte order -- the
  // shape the decoder already hands its monitor audio out in, so that the tap
  // is a connection and not a conversion. Safe to call from the thread that
  // produced the audio; the send is marshalled onto the thread that owns this
  // object.
  //
  // Returns nothing on purpose. A caller cannot usefully act on "that block
  // was dropped": the answer is always to carry on, and a return value would
  // invite a retry, which is the one response this bound exists to prevent.
  void publishAudio(const QByteArray& float_mono_audio, double sample_rate_hz);

  // The bytes of one datagram, exactly as they go on the wire.
  //
  // THE ONE PLACE THE FORMAT IS WRITTEN. Pulled out so that the layout has a
  // single definition rather than being spread through the send loop, and so
  // it can be asserted byte for byte without a socket -- which matters here
  // more than usual, because the port design means a receiver on the same
  // machine as the station cannot exist, and a format nobody can check is a
  // format that drifts from the header comment describing it.
  //
  // `first_sample` and `sample_count` index into `float_mono_audio` as 32-bit
  // floats; the caller is the one that bounds `sample_count` to
  // `kMaximumSamplesPerDatagram`.
  [[nodiscard]] static QByteArray buildDatagram(
      const QByteArray& float_mono_audio, qsizetype first_sample,
      qsizetype sample_count, quint32 sample_rate_hz, quint32 sequence,
      quint32 dropped_before, quint64 captured_unix_ms);

  // What this plane has done, for the diagnostics record. Counters only: no
  // peer address appears here, because the record goes to every observer and
  // one observer has no business learning about another.
  [[nodiscard]] QJsonObject statisticsRecord() const;

  // Mirrors DiagnosticsServer::kMaximumClients. A subscriber must already hold
  // an authenticated control connection, so this can never be exceeded through
  // that path; it is stated here as well so the bound is a property of this
  // class rather than a consequence of another one, and so it can be tested
  // without a socket. A static assertion in the control plane keeps the two in
  // step.
  static constexpr int kMaximumSubscribers = 4;

  // At most 480 samples per datagram -- 10 ms at 48 kHz, 988 bytes with the
  // header -- so a datagram fits inside the smallest path anything routes
  // (1280 bytes on IPv6) without being fragmented. Fragmentation is worth
  // avoiding rather than tolerating: a lost fragment destroys the whole
  // datagram, so fragmenting turns one lost packet into several lost
  // milliseconds of audio.
  static constexpr int kMaximumSamplesPerDatagram = 480;
  static constexpr int kHeaderBytes = 28;
  static constexpr int kMaximumDatagramBytes =
      kHeaderBytes + (kMaximumSamplesPerDatagram * 2);
  // The magic and the format code a receiver checks before trusting the rest.
  static constexpr char kMagic[4] = {'C', 'W', 'A', '1'};
  static constexpr std::uint8_t kPayloadFormatPcm16BigEndian = 1;

  // How many datagrams may exist between this class and the socket at once.
  //
  // This is the whole backpressure story and the number the 668 MB fault
  // argues for. The audio arrives in blocks of about 2048 samples, which is
  // five datagrams, and a single drain of the audio pipe can carry several
  // blocks -- so a bound of two or four would refuse audio during ordinary
  // burstiness rather than only under load, which is the mistake the spectrum
  // frame bound already had to correct once. 32 is about 320 ms at 48 kHz and
  // at most 32 kB of memory no matter how long a receiver stays away. Past it
  // audio is dropped and counted; it is never queued, because a queue is the
  // thing that grew.
  static constexpr int kMaximumDatagramsInFlight = 32;

  // Above this, what is being offered is not audio. An IQ passband at 2.4 MS/s
  // would be 4.8 MB/s of "audio" and would saturate whatever it was pointed
  // at; a remote IQ subscription is a separate, capacity-controlled feature
  // (REM-007) and not something this path should become by accident. Refused
  // and counted rather than silently truncated, so a tap wired to the wrong
  // stream shows up as a number instead of as silence.
  static constexpr double kMaximumSampleRateHz = 96'000.0;

 signals:
  void stateChanged();

 private:
  struct Subscriber {
    std::uint64_t id{0};
    QHostAddress address;
    std::uint16_t port{0};
    // The bound socket this subscriber's audio leaves by. Safe to hold,
    // because rebinding drops every subscription before it replaces a socket.
    QUdpSocket* socket{nullptr};
  };

  struct BoundSocket {
    QHostAddress address;
    std::uint16_t port{0};
    QUdpSocket* socket{nullptr};
  };

  // Sends one prepared datagram to every subscriber. Runs on the thread that
  // owns this object, which is the only thread that ever touches a socket or
  // the subscriber list.
  void sendDatagram(const QByteArray& datagram);
  // Which bound socket a control connection on this address and port should
  // send audio from, or nothing when none of them fits.
  [[nodiscard]] QUdpSocket* socketFor(const QHostAddress& local_address,
                                      std::uint16_t local_port) const;
  void rebind();
  void closeSockets();

  QList<BoundSocket> sockets_;
  QList<Endpoint> requested_endpoints_;
  QStringList bind_notes_;
  QList<Subscriber> subscribers_;

  // Read on the audio thread before any work is done, so a station with no
  // observer pays one atomic load per block. Written only by the thread that
  // owns this object.
  std::atomic<bool> sending_{false};

  // The size of `subscribers_`, kept alongside it as an atomic.
  //
  // Not a convenience. The diagnostics record is assembled on whichever thread
  // produced it, which is not the thread that owns the subscriber list, and
  // reading a QList while another thread appends to it is a data race however
  // harmless the value looks. The list itself is touched only by the owning
  // thread; everything any other thread needs to know about it is here.
  std::atomic<int> subscriber_count_{0};

  std::atomic<bool> enabled_{false};
  std::atomic<std::uint32_t> sequence_{0};
  std::atomic<std::uint32_t> datagrams_in_flight_{0};
  // Dropped since the last datagram that was accepted, which is what the next
  // header reports; and the session total, which is what the diagnostics
  // record reports. Two counters because they answer different questions: the
  // receiver needs to know about the gap it is looking at, the operator needs
  // to know how much has been lost overall.
  std::atomic<std::uint32_t> dropped_since_sent_{0};
  std::atomic<std::uint64_t> dropped_total_{0};
  std::atomic<std::uint64_t> sent_total_{0};
  std::atomic<std::uint64_t> send_failures_{0};
  std::atomic<std::uint64_t> rate_refusals_{0};
};

}  // namespace cwassistant::desktop
