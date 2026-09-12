#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>

#include "audio_stream_server.hpp"

class QTcpServer;
class QTcpSocket;

namespace cwassistant::desktop {

// A line-delimited JSON diagnostics stream, offered on operator-chosen network
// addresses so a station can be watched while it runs.
//
// Why this exists: a fault that did not reproduce on a developer's machine left
// nothing to examine but a description of it. The debug capture answers what
// the decoder found, and the throughput counters answer whether the application
// is keeping up, but both had to be collected, stopped and sent. This publishes
// the same records as they are produced.
//
// A PEER CAN SELECT ONE THING, OUT OF TWO, AND IT REACHES ONE SWITCH. THAT IS
// THE SAFETY ARGUMENT.
//
// This header used to say that no byte from a peer could ever select an
// action, and that exactly one thing was ever read from a client. Neither is
// true any more. Carrying receive audio to a remote observer needs the
// observer to be able to ask for it, and asking IS selecting an action -- so
// the claim has been replaced rather than left standing, because the sentence
// above is a promise about what this code does and a promise the code no
// longer keeps is worse than no promise at all.
//
// What is true now, exactly:
//
//   * A client presents the access token first, as a single bounded line,
//     before anything is written to it. That has not changed.
//   * After it is authenticated, a client may send request lines. Each is a
//     single bounded line, and it is COMPARED WHOLE against a CLOSED SET OF
//     TWO LITERAL STRINGS: `start-audio-stream` and `stop-audio-stream`.
//     There is no verb and an argument, no key and a value, no length field,
//     no JSON, no grammar and nothing to nest. A line either is one of those
//     two strings or the connection ends. The comparison is the same kind of
//     thing the token comparison is -- an equality test against a constant --
//     and it is the only thing done to a request.
//   * Both of those two outcomes reach one switch: whether this connection is
//     on the list of addresses receive audio is being sent to. NOTHING A PEER
//     SENDS CAN REACH THE RADIO, THE SETTINGS OR THE DECODER. There is no
//     path from this class to any of the three, and adding one would be a
//     change to this paragraph first.
//   * A request cannot create permission, only spend it. Audio streaming is
//     off until the operator turns it on, and `start-audio-stream` from a peer
//     while it is off is answered with a refusal and changes nothing. A peer
//     that has not authenticated is not read at all.
//   * A peer cannot say where audio goes. It supplies neither the address nor
//     the port: the address is the one its own control connection arrived
//     from, read off the socket, and the port IS the port that connection is
//     on -- the audio uses the same number as the control listener, over UDP
//     instead of TCP. A request carries no data whatsoever -- that is why the
//     set is two fixed strings and not a verb with a destination, and it is
//     what stops this being a reflector that could be aimed at somebody else.
//   * Every request is bounded and counted. A line longer than
//     `kMaximumRequestBytes`, a line that is not one of the two, or more than
//     `kMaximumRequestsPerClient` requests on one connection, all end the
//     connection. A peer cannot make this service work in a loop and cannot
//     grow it by talking.
//
// An earlier draft of this class refused to read at all, and therefore had no
// per-client authentication, on the reasoning that any input path near a
// transmit-capable process was unacceptable. That conflated reading a secret
// with interpreting a command. A fixed-length comparison against a shared
// token is not an interpreter, and refusing it bought no safety while costing
// the only thing standing between a bound routable address and anyone who
// could reach it. The same distinction is what the request set above rests on,
// and it is why the set is closed and literal: the moment a request carries a
// field, this stops being a comparison and becomes a parser, and the argument
// in this comment stops holding.
//
// What an operator is choosing when they bind this to a routable address: the
// station's internal state -- frequencies, callsigns decoded, device
// identifiers, transcripts -- becomes readable by anything that can reach that
// address and holds the token. That is a deliberate act and the settings page
// says so plainly. It is off unless asked for, a token is required for any
// address that is not loopback, and the main window shows an indicator for as
// long as the service is listening, because a service an operator has forgotten
// is running is the one that will surprise them.
//
// ONE PORT FOR BOTH HALVES. TCP on the configured port carries the control
// connection and the records; UDP on the SAME port carries the audio. Two
// protocols cannot collide on one number, so `kDefaultPort` stays the single
// thing either half is configured by: one number for an operator to reason
// about, one hole to open in a firewall, and a pairing that needs no
// explaining, because the audio for a diagnostics stream on 17300 arrives on
// 17300. The audio half binds exactly the addresses and ports the control half
// actually came up on, and a bind it could not manage is reported in the
// status line beside a failed control bind, because half a service is not a
// service that is up. The one case this costs is an observer running on the
// station's own machine, which cannot bind a port the station is already
// holding; audio streaming is for a remote observer, which is what it is for.
//
// Audio is a second, larger choice, and it is asked separately. A diagnostics
// record says what this application decoded; the audio carries every signal in
// the passband, decoded or not, and whatever else the receiver happens to be
// hearing. So it has its own switch, its own indicator on the main window for
// as long as anything is being sent, and it does not follow from having turned
// the diagnostics stream on. An operator who exposed telemetry did not thereby
// agree to put the station's audio on the network.
//
// That switch is deliberately NOT remembered between runs. Every other network
// setting here is persisted, because an operator who set up a diagnostics
// stream wants it back; audio is the one disclosure where coming back on by
// itself, into a session the operator has not thought about yet, is the wrong
// default. It costs one click at the start of a session that wants it and
// removes a whole class of surprise from every session that does not.
class DiagnosticsServer final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool listening READ listening NOTIFY stateChanged)
  Q_PROPERTY(int clientCount READ clientCount NOTIFY stateChanged)
  Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
  Q_PROPERTY(bool audioStreamingEnabled READ audioStreamingEnabled WRITE
                 setAudioStreamingEnabled NOTIFY stateChanged)
  Q_PROPERTY(int audioSubscriberCount READ audioSubscriberCount NOTIFY
                 stateChanged)
  Q_PROPERTY(bool audioListening READ audioListening NOTIFY stateChanged)

 public:
  explicit DiagnosticsServer(QObject* parent = nullptr);
  ~DiagnosticsServer() override;

  [[nodiscard]] bool listening() const noexcept;
  [[nodiscard]] int clientCount() const noexcept;
  [[nodiscard]] const QString& statusMessage() const noexcept;
  // Every address actually bound, for the indicator and the status line.
  [[nodiscard]] QStringList boundAddresses() const;

  // Addresses this machine could bind, as QML-ready maps carrying `address`,
  // `interfaceName`, `loopback` and `description`. Offered to the operator
  // rather than guessed at, because only they know which of their networks is
  // the one they meant.
  [[nodiscard]] static QVariantList discoverLocalAddresses();

  // A token long enough to be worth having. Rejects the empty string and
  // anything shorter than the minimum, because a token that can be guessed is
  // a token that was never asked for.
  [[nodiscard]] static bool isAcceptableToken(const QString& token);

  // Rebinds to exactly these addresses. An address that cannot be bound is
  // reported in the status line and the others still come up: a typo in one
  // interface should not silently take the service down.
  void configure(const QStringList& bind_addresses, std::uint16_t port,
                 const QString& access_token);
  // Which peers may connect at all, as addresses or CIDR subnets --
  // `192.168.1.50`, `192.168.1.0/24`, `2001:db8::/32` -- or the single entry
  // `any` for no restriction.
  //
  // This is the stronger of the two controls and the cheaper one. A peer's
  // address is known from the socket before a byte is exchanged, so a
  // disallowed peer is closed without a greeting, without a handshake and
  // without ever being told what is behind the port. The token can only be
  // checked after inviting the peer to present it; this needs no invitation.
  //
  // An empty list means loopback only. That is the safe reading of "the
  // operator has not said", and it matches the default binding: a service
  // nobody has configured should be reachable from nowhere but the machine
  // running it.
  void setAllowedPeers(const QStringList& patterns);
  [[nodiscard]] const QStringList& allowedPeers() const noexcept;
  // Whether one address would be admitted. Public so a settings page can tell
  // an operator what their list actually permits before the service is
  // running, and so the rule can be tested without a socket.
  [[nodiscard]] static bool peerIsAllowed(const QHostAddress& peer,
                                          const QStringList& patterns);

  // The network one address of this machine sits on, as a CIDR rule --
  // `192.168.1.42` on a /24 answers `192.168.1.0/24` -- or an empty string
  // when this machine cannot say.
  //
  // Offered so the settings page can put an "add this network" button beside
  // the allowed-peer list: the operator gets the convenience of their own
  // segment without the rule becoming invisible. The result is a line in the
  // list like any other, which they can read, edit or delete. Nothing here
  // changes what `peerIsAllowed` permits, and no rule is ever added behind the
  // operator's back -- a rule that decides who may read the station must be
  // one they can see.
  //
  // Empty rather than a guess, in three cases. The text is not an address;
  // this machine has no interface holding that address, which is also the
  // answer for `0.0.0.0` and `::` -- binding every interface names no single
  // network, so there is nothing honest to offer and the operator must say
  // which networks they meant; or the interface reports a prefix length that
  // cannot be used. A guessed prefix would widen access to a network the
  // operator never had, and offering nothing is a button that does not appear
  // rather than a rule that admits strangers.
  //
  // Loopback answers with its real segment, `127.0.0.0/8` or `::1/128`, rather
  // than with nothing. It is the truthful answer, and it is a useful line to
  // be able to add: an explicit list is exact, so an operator who writes only
  // their loopback segment has said "this computer alone may watch" in a form
  // they can see in the list.
  [[nodiscard]] static QString localSegmentForAddress(const QString& address);
  // The pure half of the above: the network `address` belongs to at this
  // prefix length, with the host bits cleared, or empty when the prefix cannot
  // be used -- zero or negative, or longer than the family allows. Separated
  // from the interface lookup so the masking can be tested without a machine
  // that happens to have the right network on it.
  [[nodiscard]] static QString segmentForPrefix(const QHostAddress& address,
                                                int prefix_length);

  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept;

  // Publishes one record to every authenticated client. Called from whichever
  // thread produced the record; delivery is marshalled internally.
  //
  // The record is augmented on its way out with what the audio plane has done
  // -- how much was sent, and how much was dropped rather than queued. It goes
  // here rather than into the producer of the record because the audio plane
  // belongs to this service and not to the decoder, and because the drop count
  // is only meaningful next to the throughput figures it sits beside.
  void publish(const QJsonObject& record);

  // The operator's consent for audio to leave the station, and the only thing
  // that can grant it. No request from a peer reaches this.
  void setAudioStreamingEnabled(bool enabled);
  [[nodiscard]] bool audioStreamingEnabled() const noexcept;
  // How many observers are being sent audio right now. The main window's
  // indicator is built from this, so it says "audio is leaving this machine"
  // rather than "audio was allowed to".
  [[nodiscard]] int audioSubscriberCount() const noexcept;
  // Whether the audio half is actually bound and able to send. False while the
  // operator has not allowed it, and false when it was allowed and the audio
  // port could not be bound -- the state that must never read as ready.
  [[nodiscard]] bool audioListening() const noexcept;

  // Receive audio, as mono 32-bit float samples, to be carried to whichever
  // observers asked for it. Deliberately the same shape as the decoder's
  // existing monitor-audio signal so that the tap is a connection rather than
  // a conversion. Safe to call from the thread that produced the audio.
  void publishAudio(const QByteArray& float_mono_audio, double sample_rate_hz);

  // 17300 is unassigned in the IANA registry and, more to the point, clear of
  // the ports amateur software has claimed by convention: Hamlib's rigctld
  // answers on 4532 and rotctld on 4533, DX cluster nodes commonly use 7300,
  // 7373, 8000 and 23, and reverse-beacon telnet uses 7000 and 7001. An
  // earlier default of 4531 sat one port below rigctld -- no collision, but a
  // needless invitation to confusion in the one domain where that neighbour is
  // familiar, and this application can itself be a rigctld client. Deliberately
  // in the registered range rather than above 49152, because a listener placed
  // in the ephemeral range can collide with the ports the operating system
  // hands out for this process's own outgoing connections.
  static constexpr std::uint16_t kDefaultPort = 17300;
  // A client has this long to present the token, and this much room to do it
  // in. Both are bounded so an opened-and-forgotten connection cannot hold a
  // slot, and so a peer cannot grow this process by never sending a newline.
  static constexpr int kHandshakeTimeoutMs = 5'000;
  static constexpr qint64 kMaximumHandshakeBytes = 256;
  static constexpr int kMinimumTokenLength = 16;
  // Bounded on purpose. Each client costs a buffer, and a diagnostics stream
  // that let an unbounded number of peers connect would be a way to exhaust
  // the station it is meant to help diagnose.
  static constexpr int kMaximumClients = 4;
  // A client that cannot keep up is disconnected rather than buffered without
  // limit. Losing a slow observer is a smaller failure than growing this
  // process until the station stops.
  static constexpr qint64 kMaximumClientBacklogBytes = 4LL * 1024LL * 1024LL;
  // One request line's allowance. Both of the two strings this service accepts
  // are under twenty bytes; this leaves room for a line ending and for a peer
  // that pads, and it is small enough that a peer which never sends a newline
  // cannot grow this process by talking. A line that fills it without arriving
  // ends the connection.
  static constexpr qint64 kMaximumRequestBytes = 64;
  // How many requests one connection may make. Each costs a string comparison
  // and, at most, one socket being made or unmade, so this is not a load
  // limit; it is there so a peer cannot use the request channel as a way to
  // make this service do work in a loop. No operator tool needs to start and
  // stop one audio stream thirty-two times on a single connection, and one
  // that tries has gone wrong in a way worth ending.
  static constexpr int kMaximumRequestsPerClient = 32;

 signals:
  void stateChanged();

 private:
  void acceptPending(QTcpServer* server);
  void dropClient(QTcpSocket* socket, const QString& reason);
  void restart();
  void setStatus(QString message);
  // The status line, rebuilt from everything this service currently knows.
  // One place rather than eight copies of the same four arguments, so that a
  // clause added to the line -- the audio one was -- cannot be added to seven
  // of the eight.
  void rebuildStatus(const QString& detail);
  struct Client;
  // Acts on one complete, authenticated request line. The whole of the
  // request-handling surface: it compares the line with two constants and, for
  // each, calls one method on the audio plane. Returns false when the line was
  // neither, which ends the connection.
  [[nodiscard]] bool handleRequestLine(Client* client,
                                       const QByteArray& line);

  QList<QTcpServer*> servers_;
  QList<Client*> clients_;
  // The audio plane. A child of this object, because an audio subscription
  // exists only for an authenticated control connection and must not outlive
  // the service that authenticated it.
  AudioStreamServer* audio_{nullptr};
  QStringList bind_addresses_;
  QStringList bound_addresses_;
  QString access_token_;
  QStringList allowed_peers_;
  QString status_message_;
  // Names one control connection for the audio plane, so that a subscription
  // can be ended when that connection goes without the audio plane ever
  // holding a pointer to a socket it does not own.
  std::uint64_t next_client_id_{1};
  std::uint16_t port_{kDefaultPort};
  bool enabled_{false};
};

}  // namespace cwassistant::desktop
