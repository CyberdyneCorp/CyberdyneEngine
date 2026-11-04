// SPDX-License-Identifier: MIT
#pragma once
// The secure transport: an authenticated, encrypted connection layer over any datagram substrate.
// Issue #82.
//
// ================================================================================================
// WHAT IT IS, IN ONE PARAGRAPH
// ================================================================================================
//
// A `Transport` whose links are opened by a three-message handshake with a stateless challenge,
// whose every packet after the handshake is sealed with XChaCha20-Poly1305 under per-direction
// session keys and checked against a 64-bit replay window, whose application payloads may be up to
// `kMaxMessageBytes` long (fragmented on send, reassembled on receipt), whose send rate is limited
// by a token bucket that responds to measured loss and delay, and whose connections end with a
// named `DisconnectReason`. The reliability layer underneath — channels, sequencing,
// acknowledgement, retransmission and ordered delivery — is `reliability.h`'s, composed exactly as
// the local and plain UDP transports compose it.
//
// ================================================================================================
// THE HANDSHAKE
// ================================================================================================
//
// The pattern is Noise's NK, spelled with Monocypher's primitives: the client knows the server's
// static X25519 public key in advance (shipped with the game, or handed out by a matchmaker), and
// the server learns nothing about the client from cryptography — the client's identity is the
// application's credential, checked by `CredentialCheck` before the peer is admitted.
//
//   1. HELLO     client -> server   protocol id, the client's ephemeral public key `ce`, padding.
//   2. CHALLENGE server -> client   the server's ephemeral public key `se` and a cookie, tagged
//                                   under a key derived from X25519(`ce`, server static) — which
//                                   only the holder of the server's secret can compute, so a client
//                                   that verifies the tag knows it reached the right server.
//   3. RESPONSE  client -> server   `ce` and the cookie again, and the credential sealed under the
//                                   client-to-server session key.
//
// Session keys are BLAKE2b over both Diffie-Hellman results — static and ephemeral — and every
// public key, so they are fresh per connection and forward secret against a later leak of the
// server's static key. The server's reply to RESPONSE is the first sealed packet: `Accept`, or
// `Deny` with the reason.
//
// THE SERVER KEEPS NO STATE UNTIL RESPONSE. The cookie is a keyed BLAKE2b tag over the client's
// address, its ephemeral key and the second it was issued, and the server's ephemeral secret is
// derived from the same inputs, so a RESPONSE carries everything needed to rebuild the handshake.
// A flood of HELLOs from spoofed addresses therefore allocates nothing, and because HELLO is padded
// larger than CHALLENGE it cannot be used to amplify traffic towards a victim either.
//
// ================================================================================================
// A PACKET, ON THE WIRE
// ================================================================================================
//
//   [type = Data : 1][connection id : 4][counter : 8][ciphertext ...][tag : 16]
//
// The first thirteen bytes are associated data, so a changed header fails the tag just as a changed
// payload does. The counter is the nonce (with a direction byte, so the two directions never share
// one), increments per packet, and is checked against `ReplayWindow` before decryption and marked
// after it. Inside, the plaintext is a run of frames — reliability datagrams, pings, pongs, and the
// session's own control messages — packed up to the MTU, so several small channel datagrams cost
// one tag rather than one each.
//
// ================================================================================================
// WHAT IT DOES NOT DO
// ================================================================================================
//
// * It is not DTLS, and does not interoperate with anything but itself. The specification's
//   "Security posture" names DTLS; `openspec/changes/add-secure-transport-replication` records why
//   an NK handshake over audited primitives answers the same obligations with one dependency the
//   tree already carries.
// * It does not authenticate the client cryptographically. A shipped game that needs that issues a
//   signed ticket and checks it in `CredentialCheck`.
// * It does not hide traffic analysis: sizes and timing are visible on the wire.
// * IPv6 is not implemented (`UdpSocket` is IPv4).

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/networking/congestion.h>
#include <cy/networking/crypto.h>
#include <cy/networking/datagram_socket.h>
#include <cy/networking/fragmentation.h>
#include <cy/networking/reliability.h>
#include <cy/networking/replay_window.h>
#include <cy/networking/transport.h>

namespace cy::net {

namespace wire {
struct ResponseHeader;
struct SessionKeys;
}  // namespace wire

/// The application's admission decision for a client that has completed the handshake. Returns
/// `None` to admit; `AuthenticationFailed` (or any other reason) to refuse, which the client is
/// told. Runs before the peer exists, so a refused client never receives application data.
using CredentialCheck = DisconnectReason (*)(void* user, Span<const u8> credential) noexcept;

/// The largest credential a client may present in RESPONSE.
inline constexpr u32 kMaxCredentialBytes = 256;

/// Everything a secure transport is configured with. One struct for both roles: a server sets
/// `accept_connections` and its secret, a client the server's public key and its credential.
struct SecureConfig {
    /// Distinct games, and distinct incompatible builds of one game, refuse each other at HELLO.
    u64 protocol_id = 0x4359'4E45'5453'0082ULL;

    /// Listening side: the server's static secret. A transport with `accept_connections` false
    /// ignores every HELLO.
    bool accept_connections = false;
    crypto::Key server_secret{};
    u32 max_connections = 64;
    CredentialCheck authenticate = nullptr;
    void* authenticate_user = nullptr;

    /// Connecting side: the server's static public key, and the credential sent in RESPONSE.
    crypto::Key server_public{};
    Span<const u8> credential;

    /// Wire datagrams never exceed this, headers and tag included.
    u32 mtu = kDefaultMtu;
    u32 handshake_timeout_ms = 5000;
    u32 handshake_resend_ms = 250;
    /// No authentic packet for this long ends the connection with `Timeout`.
    u32 connection_timeout_ms = 5000;
    /// Pings measure the round trip and loss and keep an idle connection alive.
    u32 ping_interval_ms = 100;
    /// How long an unreliable datagram may wait for the rate limit before it is discarded.
    u32 unreliable_hold_ms = 100;
    /// How long a partial unreliable message waits for its missing fragments.
    u32 reassembly_timeout_ms = 1000;
    u32 max_message_bytes = kMaxMessageBytes;
    /// Bytes a connection may hold queued for the rate limit before `send()` refuses.
    u32 max_queued_bytes = 1024U * 1024U;
    /// A reliable datagram abandoned after its retransmissions ends the connection. On by default:
    /// an ordered channel that has abandoned a datagram can never deliver again.
    bool disconnect_on_abandoned = true;
    /// Cookies older than this are refused, in seconds.
    u32 cookie_lifetime_s = 10;

    RateConfig rate{};
    RetransmitPolicy retransmit{};
    /// Keys, nonces and the cookie secret. The operating system's generator unless a test says
    /// otherwise — see `crypto.h`.
    crypto::EntropySource entropy = crypto::system_entropy();
};

/// Counters about what the handshake and the packet checks refused. Transport-wide, because most of
/// what they count arrives before there is a connection to attribute it to.
struct SecureCounters {
    u64 hellos_answered = 0;
    u64 hellos_ignored = 0;
    u64 handshakes_completed = 0;
    u64 handshakes_refused = 0;
    u64 cookies_rejected = 0;
    u64 responses_rejected = 0;
    u64 packets_rejected_authentication = 0;
    u64 packets_rejected_replay = 0;
    u64 packets_malformed = 0;
    u64 packets_from_strangers = 0;
    u64 packets_oversized = 0;
};

/// The transport a shipping game uses — see the header comment for the handshake and the wire.
class SecureTransport final : public Transport {
public:
    SecureTransport(Allocator& allocator, DatagramSocket& socket,
                    const SecureConfig& config) noexcept;
    ~SecureTransport() override;

    /// Start a handshake with the server at `address` (as `DatagramSocket::resolve()` reads it),
    /// using the configured server key and credential. The peer is `Connecting` until the server
    /// accepts, then `Connected`, and a `ConnectionEvent` says which way it went.
    [[nodiscard]] Expected<PeerId, Error> connect(const char* address) noexcept override;
    void disconnect(PeerId peer) noexcept override;
    /// Up to `max_message_bytes`. Larger than one datagram is fragmented; see `fragmentation.h`.
    [[nodiscard]] Status send(PeerId peer, ChannelId channel, DeliveryMode delivery,
                              Span<const u8> bytes) noexcept override;
    [[nodiscard]] bool receive(Datagram& out) noexcept override;
    void advance(u64 now_ms) noexcept override;
    [[nodiscard]] ConnectionState state(PeerId peer) const noexcept override;
    [[nodiscard]] ConnectionStats stats(PeerId peer) const noexcept override;
    [[nodiscard]] TransportSecurity security() const noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "secure"; }
    [[nodiscard]] bool poll_event(ConnectionEvent& out) noexcept override;

    /// Microsecond time. `advance(now_ms)` is this at millisecond resolution; a host that has a
    /// finer clock passes it here so round trips on a fast link are not all zero.
    void advance_us(u64 now_us) noexcept override;

    [[nodiscard]] DisconnectReason disconnect_reason(PeerId peer) const noexcept;
    [[nodiscard]] const SecureCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] u32 connection_count() const noexcept;
    [[nodiscard]] const SecureConfig& config() const noexcept { return config_; }

    /// The server's public key for `secret`, for a test or a tool that generates both halves.
    [[nodiscard]] static crypto::Key public_key_for(const crypto::Key& secret) noexcept {
        return crypto::public_key_of(secret);
    }

private:
    struct Link;
    struct Frame;

    [[nodiscard]] Link* find(PeerId peer) noexcept;
    [[nodiscard]] const Link* find(PeerId peer) const noexcept;
    [[nodiscard]] Link* find(const SocketAddress& address) noexcept;
    [[nodiscard]] Expected<Link*, Error> open_link(const SocketAddress& address) noexcept;
    void release(Link* link) noexcept;
    void close_link(Link& link, DisconnectReason reason, bool tell_peer) noexcept;
    void push_event(ConnectionEvent::Kind kind, const Link& link) noexcept;

    // Inbound.
    void pump() noexcept;
    void handle_datagram(const SocketAddress& from, Span<const u8> bytes) noexcept;
    void handle_hello(const SocketAddress& from, Span<const u8> bytes) noexcept;
    void handle_challenge(Link& link, Span<const u8> bytes) noexcept;
    void handle_response(const SocketAddress& from, Span<const u8> bytes) noexcept;
    [[nodiscard]] bool answer_retried_response(const SocketAddress& from,
                                               const crypto::Key& client_ephemeral) noexcept;
    [[nodiscard]] bool cookie_valid(const wire::ResponseHeader& header,
                                    const SocketAddress& from) const noexcept;
    [[nodiscard]] bool server_session_keys(const wire::ResponseHeader& header,
                                           const SocketAddress& from,
                                           wire::SessionKeys& keys) const noexcept;
    [[nodiscard]] DisconnectReason admission_verdict(Span<const u8> credential) const noexcept;
    void handle_data(Link& link, Span<const u8> bytes) noexcept;
    void handle_frames(Link& link, Span<const u8> plain) noexcept;
    void handle_frame(Link& link, u8 kind, Span<const u8> body) noexcept;
    void handle_ping(Link& link, Span<const u8> body) noexcept;
    void handle_pong(Link& link, Span<const u8> body) noexcept;
    void refuse(const SocketAddress& to, const crypto::Key& key, DisconnectReason reason) noexcept;

    // Outbound.
    [[nodiscard]] Status queue_frame(Link& link, u8 kind, Span<const u8> body, bool control,
                                     bool reliable) noexcept;
    [[nodiscard]] Status queue_datagrams(Link& link, Span<const u8> wire, bool control) noexcept;
    [[nodiscard]] Status send_message(Link& link, ChannelId channel, DeliveryMode delivery,
                                      Span<const u8> payload) noexcept;
    void tick_link(Link& link) noexcept;
    void tick_handshake(Link& link) noexcept;
    void tick_connection(Link& link) noexcept;
    void send_ping(Link& link) noexcept;
    void flush(Link& link) noexcept;
    void expire_unreliable(Link& link) const noexcept;
    [[nodiscard]] bool emit_packet(Link& link, bool control_only) noexcept;
    [[nodiscard]] Status seal_and_send(Link& link, Span<const u8> plain) noexcept;
    void transmit(const SocketAddress& to, Span<const u8> wire) noexcept;

    // Delivery.
    [[nodiscard]] bool drain(Datagram& out) noexcept;

    Allocator* allocator_;
    DatagramSocket* socket_;
    SecureConfig config_;
    crypto::Key cookie_secret_{};
    crypto::Key server_public_{};
    Array<Link*> slots_;
    Array<u32> generations_;
    HashMap<u64, u32> by_address_;
    Array<ConnectionEvent> events_;
    u32 event_head_ = 0;
    Array<u8> wire_;
    Array<u8> plain_;
    Array<u8> scratch_;
    Array<u8> collected_;
    SecureCounters counters_{};
    u64 now_us_ = 0;
    bool clock_started_ = false;
    u32 drain_slot_ = 0;
    u32 drain_channel_ = 0;
};

}  // namespace cy::net
