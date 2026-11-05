// SPDX-License-Identifier: MIT
#include <cy/networking/secure_transport.h>

#include "secure_wire.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace cy::net {

const char* disconnect_reason_name(DisconnectReason reason) noexcept {
    switch (reason) {
        case DisconnectReason::None:
            return "None";
        case DisconnectReason::LocalRequest:
            return "LocalRequest";
        case DisconnectReason::PeerRequest:
            return "PeerRequest";
        case DisconnectReason::Timeout:
            return "Timeout";
        case DisconnectReason::HandshakeTimeout:
            return "HandshakeTimeout";
        case DisconnectReason::ServerFull:
            return "ServerFull";
        case DisconnectReason::AuthenticationFailed:
            return "AuthenticationFailed";
        case DisconnectReason::ReliableAbandoned:
            return "ReliableAbandoned";
        case DisconnectReason::TransportClosed:
            return "TransportClosed";
    }
    return "Unknown";
}

namespace {

/// The smallest MTU the transport accepts: one full reliability datagram, framed and sealed.
constexpr u32 kMinimumMtu = wire::kDataOverhead + wire::kFrameHeaderBytes + kMaxWireDatagram;
/// A disconnect is sent this many times, each in its own packet, because it is never retransmitted.
constexpr u32 kDisconnectRepeats = 3;

enum class Phase : u8 {
    Hello = 0,
    Response,
    Established,
};

/// One queue of frames waiting for the wire, each already carrying its frame header.
struct FrameQueue {
    struct Entry {
        u32 offset = 0;
        u32 size = 0;
        u64 enqueued_us = 0;
        bool reliable = false;
    };

    Array<u8> bytes;
    Array<Entry> entries;
    u32 head = 0;

    explicit FrameQueue(Allocator& allocator) noexcept : bytes(allocator), entries(allocator) {}

    [[nodiscard]] bool empty() const noexcept { return head >= entries.size(); }
    [[nodiscard]] const Entry& front() const noexcept { return entries[head]; }
    [[nodiscard]] Span<const u8> view(const Entry& entry) const noexcept {
        return {bytes.data() + entry.offset, entry.size};
    }
    [[nodiscard]] usize queued_bytes() const noexcept {
        return empty() ? 0 : bytes.size() - entries[head].offset;
    }

    [[nodiscard]] Status push(Span<const u8> frame, u64 now_us, bool reliable) noexcept {
        Entry entry;
        entry.offset = static_cast<u32>(bytes.size());
        entry.size = static_cast<u32>(frame.size());
        entry.enqueued_us = now_us;
        entry.reliable = reliable;
        if (Status appended = bytes.append(frame); !appended) {
            return appended;
        }
        return entries.push_back(entry);
    }

    void pop() noexcept {
        ++head;
        if (head >= entries.size()) {
            clear();
            return;
        }
        if (head >= 256 && static_cast<usize>(head) * 2 >= entries.size()) {
            compact();
        }
    }

    /// Move what is still queued to the front. A queue with a standing backlog never empties, and
    /// without this its arrays would grow by everything it ever carried.
    void compact() noexcept {
        const u32 base = entries[head].offset;
        const usize remaining_bytes = bytes.size() - base;
        std::memmove(bytes.data(), bytes.data() + base, remaining_bytes);
        (void)bytes.resize(remaining_bytes);
        const usize remaining = entries.size() - head;
        for (usize index = 0; index < remaining; ++index) {
            Entry moved = entries[head + index];
            moved.offset -= base;
            entries[index] = moved;
        }
        (void)entries.resize(remaining);
        head = 0;
    }

    void clear() noexcept {
        entries.clear();
        bytes.clear();
        head = 0;
    }
};

template <class T, class... Args>
[[nodiscard]] T* make(Allocator& allocator, Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return nullptr;
    }
    return construct_at<T>(storage, std::forward<Args>(args)...);
}

template <class T>
void unmake(Allocator& allocator, T* object) noexcept {
    if (object != nullptr) {
        object->~T();
        allocator.deallocate(object, sizeof(T), alignof(T));
    }
}

[[nodiscard]] u64 random_u64(const crypto::EntropySource& entropy) noexcept {
    u8 bytes[8];
    entropy(bytes, sizeof(bytes));
    return wire::get_u64(bytes);
}

}  // namespace

struct SecureTransport::Link {
    PeerId peer;
    SocketAddress address;
    ConnectionState state = ConnectionState::Connecting;
    DisconnectReason reason = DisconnectReason::None;
    Phase phase = Phase::Hello;
    bool client = false;
    /// Set by `close_link()`; the link is released at the end of the `advance()` that set it, so
    /// nothing in the middle of handling a packet ever frees the object it is using.
    bool dead = false;

    // The handshake.
    crypto::KeyPair ephemeral{};
    crypto::Key remote_ephemeral{};
    Array<u8> handshake_packet;
    u64 handshake_started_us = 0;
    u64 handshake_sent_us = 0;
    /// `connect()` ran before the transport's first `advance()`, so there was no clock to start the
    /// handshake's timers from. They start at the first `advance()` instead.
    bool clock_pending = false;

    // The session.
    crypto::Key send_key{};
    crypto::Key receive_key{};
    u32 connection_id = 0;
    u64 send_counter = 0;
    ReplayWindow window;
    ReliableEndpoint endpoint;
    Reassembler reassembler;
    u16 next_message[kMaxChannels] = {};

    // Pacing and measurement.
    SendRateController rate;
    RttEstimator rtt;
    LossMeter inbound;
    /// The peer's reports on what it received from us: the loss of the direction we send in.
    LossEstimate outbound_loss;
    /// Our own count of what arrived: the loss of the direction we receive in.
    LossEstimate inbound_loss;
    u32 ping_id = 0;
    u64 last_ping_us = 0;
    u64 last_received_us = 0;

    FrameQueue control;
    FrameQueue data;
    ConnectionStats stats{};

    Link(Allocator& allocator, const SecureConfig& config) noexcept
        : handshake_packet(allocator),
          endpoint(allocator),
          reassembler(allocator, config.max_message_bytes),
          rate(config.rate),
          control(allocator),
          data(allocator) {}

    ~Link() {
        crypto::wipe(&ephemeral, sizeof(ephemeral));
        crypto::wipe(&send_key, sizeof(send_key));
        crypto::wipe(&receive_key, sizeof(receive_key));
    }

    [[nodiscard]] wire::Direction outgoing() const noexcept {
        return client ? wire::Direction::ClientToServer : wire::Direction::ServerToClient;
    }
    [[nodiscard]] wire::Direction incoming() const noexcept {
        return client ? wire::Direction::ServerToClient : wire::Direction::ClientToServer;
    }
    [[nodiscard]] bool established() const noexcept { return phase == Phase::Established; }
};

// --- Construction and links ----------------------------------------------------------------------

SecureTransport::SecureTransport(Allocator& allocator, DatagramSocket& socket,
                                 const SecureConfig& config) noexcept
    : allocator_(&allocator),
      socket_(&socket),
      config_(config),
      slots_(allocator),
      generations_(allocator),
      by_address_(allocator),
      events_(allocator),
      wire_(allocator),
      plain_(allocator),
      scratch_(allocator),
      collected_(allocator) {
    config_.mtu = std::clamp(config_.mtu, kMinimumMtu, kMaxSocketDatagram);
    config_.max_message_bytes = std::min(config_.max_message_bytes, kMaxMessageBytes);
    config_.entropy(cookie_secret_.bytes, crypto::kKeyBytes);
    if (config_.accept_connections) {
        server_public_ = crypto::public_key_of(config_.server_secret);
    }
}

SecureTransport::~SecureTransport() {
    for (Link* link : slots_) {
        if (link != nullptr && !link->dead && link->established()) {
            close_link(*link, DisconnectReason::TransportClosed, true);
        }
        unmake(*allocator_, link);
    }
    crypto::wipe(&cookie_secret_, sizeof(cookie_secret_));
    crypto::wipe(&config_.server_secret, sizeof(config_.server_secret));
}

SecureTransport::Link* SecureTransport::find(PeerId peer) noexcept {
    if (!peer.valid() || peer.slot() >= slots_.size()) {
        return nullptr;
    }
    Link* link = slots_[peer.slot()];
    return (link != nullptr && link->peer == peer) ? link : nullptr;
}

const SecureTransport::Link* SecureTransport::find(PeerId peer) const noexcept {
    if (!peer.valid() || peer.slot() >= slots_.size()) {
        return nullptr;
    }
    const Link* link = slots_[peer.slot()];
    return (link != nullptr && link->peer == peer) ? link : nullptr;
}

SecureTransport::Link* SecureTransport::find(const SocketAddress& address) noexcept {
    const u32* slot = by_address_.find(address.key());
    if (slot == nullptr || *slot >= slots_.size()) {
        return nullptr;
    }
    Link* link = slots_[*slot];
    return (link != nullptr && !link->dead && link->address == address) ? link : nullptr;
}

Expected<SecureTransport::Link*, Error> SecureTransport::open_link(
    const SocketAddress& address) noexcept {
    u32 slot = 0;
    while (slot < slots_.size() && slots_[slot] != nullptr) {
        ++slot;
    }
    if (slot == slots_.size()) {
        if (!slots_.push_back(nullptr) || !generations_.push_back(0)) {
            return fail(ErrorCode::OutOfMemory, "networking: could not grow the link table");
        }
    }
    Link* link = make<Link>(*allocator_, *allocator_, config_);
    if (link == nullptr) {
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate a secure link");
    }
    if (Status prepared = link->endpoint.prepare(); !prepared) {
        unmake(*allocator_, link);
        return make_unexpected(prepared.error());
    }
    link->endpoint.set_policy(config_.retransmit);
    generations_[slot] += 1;
    link->peer = PeerId::make(slot, generations_[slot]);
    link->address = address;
    link->last_received_us = now_us_;
    link->last_ping_us = now_us_;
    if (!by_address_.insert(address.key(), slot)) {
        unmake(*allocator_, link);
        return fail(ErrorCode::OutOfMemory, "networking: could not index a secure link");
    }
    slots_[slot] = link;
    return link;
}

void SecureTransport::release(Link* link) noexcept {
    const u32 slot = link->peer.slot();
    if (const u32* indexed = by_address_.find(link->address.key());
        indexed != nullptr && *indexed == slot) {
        (void)by_address_.remove(link->address.key());
    }
    slots_[slot] = nullptr;
    unmake(*allocator_, link);
}

void SecureTransport::push_event(ConnectionEvent::Kind kind, const Link& link) noexcept {
    ConnectionEvent event;
    event.kind = kind;
    event.peer = link.peer;
    event.reason = link.reason;
    (void)events_.push_back(event);
}

void SecureTransport::close_link(Link& link, DisconnectReason reason, bool tell_peer) noexcept {
    if (link.dead) {
        return;
    }
    if (tell_peer && link.established()) {
        const u8 body[1] = {static_cast<u8>(reason)};
        for (u32 repeat = 0; repeat < kDisconnectRepeats; ++repeat) {
            plain_.clear();
            if (wire::append_frame(wire::FrameKind::Disconnect, Span<const u8>(body, 1), plain_)) {
                (void)seal_and_send(link, plain_.span());
            }
        }
    }
    link.dead = true;
    link.reason = reason;
    link.state = ConnectionState::Disconnected;
    // Ended before it began: the application never saw a `Connected`, but a client still needs to
    // hear why its `connect()` came to nothing.
    push_event(ConnectionEvent::Kind::Disconnected, link);
}

u32 SecureTransport::connection_count() const noexcept {
    u32 count = 0;
    for (const Link* link : slots_) {
        count += (link != nullptr && !link->dead) ? 1U : 0U;
    }
    return count;
}

// --- The Transport interface ---------------------------------------------------------------------

Expected<PeerId, Error> SecureTransport::connect(const char* address) noexcept {
    Expected<SocketAddress, Error> resolved = socket_->resolve(address);
    if (!resolved) {
        return make_unexpected(resolved.error());
    }
    if (config_.credential.size() > kMaxCredentialBytes) {
        return fail(ErrorCode::OutOfRange, "networking: a credential larger than 256 bytes");
    }
    if (find(*resolved) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "networking: already connected to that address");
    }
    Expected<Link*, Error> opened = open_link(*resolved);
    if (!opened) {
        return make_unexpected(opened.error());
    }
    Link& link = **opened;
    link.client = true;
    link.ephemeral = crypto::generate_key_pair(config_.entropy);
    u8 hello[wire::kHelloBytes];
    wire::write_hello(config_.protocol_id, link.ephemeral.public_key, hello);
    if (Status stored = link.handshake_packet.append(Span<const u8>(hello, sizeof(hello)));
        !stored) {
        release(&link);
        return make_unexpected(stored.error());
    }
    link.handshake_started_us = now_us_;
    link.handshake_sent_us = now_us_;
    link.clock_pending = !clock_started_;
    transmit(link.address, link.handshake_packet.span());
    return link.peer;
}

void SecureTransport::disconnect(PeerId peer) noexcept {
    if (Link* link = find(peer); link != nullptr) {
        close_link(*link, DisconnectReason::LocalRequest, true);
    }
}

Status SecureTransport::send(PeerId peer, ChannelId channel, DeliveryMode delivery,
                             Span<const u8> bytes) noexcept {
    Link* link = find(peer);
    if (link == nullptr || link->dead || link->state != ConnectionState::Connected) {
        return fail(ErrorCode::Unavailable, "networking: that peer is not connected");
    }
    if (channel >= kMaxChannels) {
        return fail(ErrorCode::OutOfRange, "networking: channel out of range");
    }
    if (bytes.size() > config_.max_message_bytes) {
        return fail(ErrorCode::OutOfRange,
                    "networking: a message larger than the transport's configured maximum");
    }
    if (link->data.queued_bytes() > config_.max_queued_bytes) {
        return fail(ErrorCode::Unavailable,
                    "networking: the send queue is full; the link is slower than the sender");
    }
    const u32 fragments = fragments_for(bytes.size());
    if (fragments == 0) {
        if (Status framed = write_whole(bytes, scratch_); !framed) {
            return framed;
        }
        return send_message(*link, channel, delivery, scratch_.span());
    }
    // A sequenced channel drops whatever arrives behind a newer datagram, which would discard the
    // earlier fragments of the very message they belong to. Fragments therefore travel as plain
    // unreliable datagrams, and the message as a whole is still delivered only complete.
    const DeliveryMode per_fragment =
        delivery == DeliveryMode::UnreliableSequenced ? DeliveryMode::Unreliable : delivery;
    const u16 message = link->next_message[channel]++;
    for (u32 index = 0; index < fragments; ++index) {
        if (Status framed = write_fragment(message, index, bytes, scratch_); !framed) {
            return framed;
        }
        if (Status sent = send_message(*link, channel, per_fragment, scratch_.span()); !sent) {
            return sent;
        }
    }
    return ok();
}

Status SecureTransport::send_message(Link& link, ChannelId channel, DeliveryMode delivery,
                                     Span<const u8> payload) noexcept {
    collected_.clear();
    if (Status framed = link.endpoint.frame(channel, delivery, payload, now_us_ / 1000, collected_);
        !framed) {
        return framed;
    }
    return queue_frame(link, static_cast<u8>(wire::FrameKind::Datagram), collected_.span(), false,
                       is_reliable(delivery));
}

bool SecureTransport::receive(Datagram& out) noexcept {
    return drain(out);
}

ConnectionState SecureTransport::state(PeerId peer) const noexcept {
    const Link* link = find(peer);
    return link == nullptr ? ConnectionState::Disconnected : link->state;
}

DisconnectReason SecureTransport::disconnect_reason(PeerId peer) const noexcept {
    const Link* link = find(peer);
    return link == nullptr ? DisconnectReason::None : link->reason;
}

ConnectionStats SecureTransport::stats(PeerId peer) const noexcept {
    const Link* link = find(peer);
    if (link == nullptr) {
        return {};
    }
    ConnectionStats out = link->stats;
    out.round_trip_us = link->rtt.smoothed();
    out.jitter_us = link->rtt.jitter();
    out.loss_per_mille = link->outbound_loss.per_mille();
    out.inbound_loss_per_mille = link->inbound_loss.per_mille();
    out.retransmissions = link->endpoint.retransmissions();
    out.reliable_abandoned = link->endpoint.abandoned();
    out.send_rate_bytes_per_second = link->rate.rate();
    return out;
}

TransportSecurity SecureTransport::security() const noexcept {
    TransportSecurity security;
    // A seeded entropy source makes every key predictable to whoever has the seed. The packets are
    // still sealed, and the replay window still works, but nothing is confidential or authentic
    // against an attacker who knows the seed, so the transport stops claiming that it is.
    security.encrypted = config_.entropy.cryptographic;
    security.authenticated = config_.entropy.cryptographic;
    security.replay_protected = true;
    security.sequence_validated = true;
    security.in_process = socket_->in_process();
    security.mechanism =
        config_.entropy.cryptographic
            ? "NK handshake: X25519, XChaCha20-Poly1305, BLAKE2b (Monocypher 4.0.2)"
            : "NK handshake over SEEDED entropy: test only, never shipping";
    return security;
}

bool SecureTransport::poll_event(ConnectionEvent& out) noexcept {
    if (event_head_ >= events_.size()) {
        events_.clear();
        event_head_ = 0;
        return false;
    }
    out = events_[event_head_++];
    return true;
}

void SecureTransport::advance(u64 now_ms) noexcept {
    advance_us(now_ms * 1000);
}

void SecureTransport::advance_us(u64 now_us) noexcept {
    now_us_ = std::max(now_us_, now_us);
    clock_started_ = true;
    socket_->advance(now_us_ / 1000);
    pump();
    for (Link* link : slots_) {
        if (link != nullptr && !link->dead) {
            tick_link(*link);
        }
    }
    for (Link* link : slots_) {
        if (link != nullptr && link->dead) {
            release(link);
        }
    }
}

// --- Inbound -------------------------------------------------------------------------------------

void SecureTransport::pump() noexcept {
    SocketAddress from;
    Span<const u8> bytes;
    while (socket_->receive_from(from, bytes)) {
        handle_datagram(from, bytes);
    }
}

void SecureTransport::handle_datagram(const SocketAddress& from, Span<const u8> bytes) noexcept {
    if (bytes.empty()) {
        ++counters_.packets_malformed;
        return;
    }
    if (bytes.size() > config_.mtu) {
        ++counters_.packets_oversized;
        return;
    }
    switch (static_cast<wire::PacketType>(bytes[0])) {
        case wire::PacketType::Hello:
            handle_hello(from, bytes);
            return;
        case wire::PacketType::Response:
            handle_response(from, bytes);
            return;
        case wire::PacketType::Challenge:
        case wire::PacketType::Data: {
            Link* link = find(from);
            if (link == nullptr) {
                ++counters_.packets_from_strangers;
                return;
            }
            if (bytes[0] == static_cast<u8>(wire::PacketType::Challenge)) {
                handle_challenge(*link, bytes);
            } else {
                handle_data(*link, bytes);
            }
            return;
        }
    }
    ++counters_.packets_malformed;
}

void SecureTransport::handle_hello(const SocketAddress& from, Span<const u8> bytes) noexcept {
    crypto::Key client_ephemeral;
    if (!config_.accept_connections || find(from) != nullptr ||
        !wire::read_hello(bytes, config_.protocol_id, client_ephemeral)) {
        ++counters_.hellos_ignored;
        return;
    }
    crypto::Key static_shared;
    if (!crypto::key_exchange(config_.server_secret, client_ephemeral, static_shared)) {
        ++counters_.hellos_ignored;
        return;
    }
    const u64 issued_s = now_us_ / 1'000'000;
    crypto::Key ephemeral_secret =
        wire::server_ephemeral(cookie_secret_, issued_s, client_ephemeral, from);
    wire::Challenge challenge;
    challenge.server_ephemeral_public = crypto::public_key_of(ephemeral_secret);
    challenge.cookie =
        wire::make_cookie(cookie_secret_, config_.protocol_id, issued_s, client_ephemeral, from);
    config_.entropy(challenge.nonce, sizeof(challenge.nonce));
    crypto::Key key = wire::challenge_key(static_shared, config_.protocol_id, client_ephemeral,
                                          challenge.server_ephemeral_public, server_public_);
    u8 packet[wire::kChallengeBytes];
    wire::write_challenge(config_.protocol_id, challenge, key, packet);
    crypto::wipe(&static_shared, sizeof(static_shared));
    crypto::wipe(&ephemeral_secret, sizeof(ephemeral_secret));
    crypto::wipe(&key, sizeof(key));
    transmit(from, Span<const u8>(packet, sizeof(packet)));
    ++counters_.hellos_answered;
}

void SecureTransport::handle_challenge(Link& link, Span<const u8> bytes) noexcept {
    wire::Challenge challenge;
    if (!link.client || link.phase != Phase::Hello ||
        !wire::peek_challenge(bytes, config_.protocol_id, challenge)) {
        ++counters_.packets_malformed;
        return;
    }
    crypto::Key static_shared;
    crypto::Key ephemeral_shared;
    if (!crypto::key_exchange(link.ephemeral.secret, config_.server_public, static_shared) ||
        !crypto::key_exchange(link.ephemeral.secret, challenge.server_ephemeral_public,
                              ephemeral_shared)) {
        ++counters_.packets_rejected_authentication;
        return;
    }
    crypto::Key key =
        wire::challenge_key(static_shared, config_.protocol_id, link.ephemeral.public_key,
                            challenge.server_ephemeral_public, config_.server_public);
    const bool authentic = wire::read_challenge(bytes, config_.protocol_id, key, challenge);
    crypto::wipe(&key, sizeof(key));
    if (!authentic) {
        // Not the server whose key this client was given. Keep sending HELLO: the real server may
        // still answer, and if it never does the handshake times out with the reason named.
        crypto::wipe(&static_shared, sizeof(static_shared));
        crypto::wipe(&ephemeral_shared, sizeof(ephemeral_shared));
        ++counters_.packets_rejected_authentication;
        return;
    }
    const wire::SessionKeys keys = wire::session_keys(
        static_shared, ephemeral_shared, config_.protocol_id, link.ephemeral.public_key,
        challenge.server_ephemeral_public, config_.server_public);
    crypto::wipe(&static_shared, sizeof(static_shared));
    crypto::wipe(&ephemeral_shared, sizeof(ephemeral_shared));
    link.send_key = keys.client_to_server;
    link.receive_key = keys.server_to_client;

    wire::ResponseHeader header;
    header.client_ephemeral = link.ephemeral.public_key;
    header.cookie = challenge.cookie;
    if (!wire::write_response(config_.protocol_id, header, link.send_key, config_.credential,
                              link.handshake_packet)) {
        return;
    }
    // Counter zero of this direction was RESPONSE's nonce.
    link.send_counter = 1;
    link.phase = Phase::Response;
    link.handshake_sent_us = now_us_;
    transmit(link.address, link.handshake_packet.span());
}

void SecureTransport::refuse(const SocketAddress& to, const crypto::Key& key,
                             DisconnectReason reason) noexcept {
    plain_.clear();
    const u8 body[1] = {static_cast<u8>(reason)};
    if (!wire::append_frame(wire::FrameKind::Deny, Span<const u8>(body, 1), plain_)) {
        return;
    }
    // A random counter, never zero-based: a client that retries RESPONSE may be refused twice, for
    // two different reasons, and two plaintexts must never share a key and a nonce.
    wire::DataHeader header;
    header.counter = random_u64(config_.entropy) | (1ULL << 63);
    if (wire::seal_data(header, wire::Direction::ServerToClient, key, plain_.span(), wire_)) {
        transmit(to, wire_.span());
    }
    ++counters_.handshakes_refused;
}

bool SecureTransport::answer_retried_response(const SocketAddress& from,
                                              const crypto::Key& client_ephemeral) noexcept {
    Link* existing = find(from);
    if (existing == nullptr) {
        return false;
    }
    // A retried RESPONSE whose ACCEPT was lost. Answer it again; anything else from a known address
    // is somebody else's handshake and is refused.
    if (!existing->client && existing->remote_ephemeral == client_ephemeral) {
        (void)queue_frame(*existing, static_cast<u8>(wire::FrameKind::Accept), {}, true, false);
        flush(*existing);
    } else {
        ++counters_.responses_rejected;
    }
    return true;
}

bool SecureTransport::cookie_valid(const wire::ResponseHeader& header,
                                   const SocketAddress& from) const noexcept {
    const u64 now_s = now_us_ / 1'000'000;
    const u64 issued = header.cookie.issued_s;
    if (issued > now_s || now_s - issued > config_.cookie_lifetime_s) {
        return false;
    }
    const wire::Cookie expected = wire::make_cookie(cookie_secret_, config_.protocol_id, issued,
                                                    header.client_ephemeral, from);
    return crypto::equal_tags(expected.tag, header.cookie.tag);
}

bool SecureTransport::server_session_keys(const wire::ResponseHeader& header,
                                          const SocketAddress& from,
                                          wire::SessionKeys& keys) const noexcept {
    crypto::Key ephemeral_secret = wire::server_ephemeral(cookie_secret_, header.cookie.issued_s,
                                                          header.client_ephemeral, from);
    crypto::Key static_shared;
    crypto::Key ephemeral_shared;
    const bool agreed =
        crypto::key_exchange(config_.server_secret, header.client_ephemeral, static_shared) &&
        crypto::key_exchange(ephemeral_secret, header.client_ephemeral, ephemeral_shared);
    const crypto::Key ephemeral_public = crypto::public_key_of(ephemeral_secret);
    crypto::wipe(&ephemeral_secret, sizeof(ephemeral_secret));
    if (agreed) {
        keys = wire::session_keys(static_shared, ephemeral_shared, config_.protocol_id,
                                  header.client_ephemeral, ephemeral_public, server_public_);
    }
    crypto::wipe(&static_shared, sizeof(static_shared));
    crypto::wipe(&ephemeral_shared, sizeof(ephemeral_shared));
    return agreed;
}

DisconnectReason SecureTransport::admission_verdict(Span<const u8> credential) const noexcept {
    if (connection_count() >= config_.max_connections) {
        return DisconnectReason::ServerFull;
    }
    if (config_.authenticate != nullptr) {
        return config_.authenticate(config_.authenticate_user, credential);
    }
    return DisconnectReason::None;
}

void SecureTransport::handle_response(const SocketAddress& from, Span<const u8> bytes) noexcept {
    wire::ResponseHeader header;
    if (!config_.accept_connections ||
        !wire::read_response_header(bytes, config_.protocol_id, header)) {
        ++counters_.responses_rejected;
        return;
    }
    if (answer_retried_response(from, header.client_ephemeral)) {
        return;
    }
    if (!cookie_valid(header, from)) {
        ++counters_.cookies_rejected;
        return;
    }
    wire::SessionKeys keys;
    if (!server_session_keys(header, from, keys)) {
        ++counters_.responses_rejected;
        return;
    }
    if (!wire::open_response(bytes, keys.client_to_server, scratch_)) {
        ++counters_.packets_rejected_authentication;
        return;
    }
    if (const DisconnectReason verdict = admission_verdict(scratch_.span());
        verdict != DisconnectReason::None) {
        refuse(from, keys.server_to_client, verdict);
        return;
    }
    Expected<Link*, Error> opened = open_link(from);
    if (!opened) {
        return;
    }
    Link& link = **opened;
    link.client = false;
    link.phase = Phase::Established;
    link.state = ConnectionState::Connected;
    link.remote_ephemeral = header.client_ephemeral;
    link.send_key = keys.server_to_client;
    link.receive_key = keys.client_to_server;
    // RESPONSE spent the client's counter zero, and it arrived: both the replay window and the loss
    // count must say so, or every connection would report its first packet lost.
    link.window.mark(0);
    link.inbound.received(0);
    link.connection_id = static_cast<u32>(random_u64(config_.entropy)) | 1U;
    ++counters_.handshakes_completed;
    push_event(ConnectionEvent::Kind::Connected, link);
    (void)queue_frame(link, static_cast<u8>(wire::FrameKind::Accept), {}, true, false);
    flush(link);
}

void SecureTransport::handle_data(Link& link, Span<const u8> bytes) noexcept {
    wire::DataHeader header;
    if (link.phase == Phase::Hello || !wire::read_data_header(bytes, header)) {
        ++counters_.packets_malformed;
        return;
    }
    const bool first = link.phase == Phase::Response;
    if (!first && header.connection != link.connection_id) {
        ++counters_.packets_malformed;
        return;
    }
    if (link.window.rejects(header.counter)) {
        ++counters_.packets_rejected_replay;
        ++link.stats.packets_rejected_replay;
        return;
    }
    if (!wire::open_data(bytes, link.incoming(), link.receive_key, plain_)) {
        ++counters_.packets_rejected_authentication;
        ++link.stats.packets_rejected_authentication;
        return;
    }
    link.window.mark(header.counter);
    link.inbound.received(header.counter);
    link.last_received_us = now_us_;
    link.stats.datagrams_in += 1;
    link.stats.bytes_in += bytes.size();
    handle_frames(link, plain_.span());
    if (first && !link.dead) {
        // Any authentic packet from the server is an acceptance — the explicit ACCEPT may have
        // been the one that was lost.
        link.connection_id = header.connection;
        link.phase = Phase::Established;
        link.state = ConnectionState::Connected;
        link.handshake_packet.clear();
        push_event(ConnectionEvent::Kind::Connected, link);
    }
}

void SecureTransport::handle_frames(Link& link, Span<const u8> plain) noexcept {
    usize offset = 0;
    while (offset + wire::kFrameHeaderBytes <= plain.size() && !link.dead) {
        const u8 kind = plain[offset];
        const u16 size = wire::get_u16(plain.data() + offset + 1);
        offset += wire::kFrameHeaderBytes;
        if (offset + size > plain.size()) {
            ++counters_.packets_malformed;
            return;
        }
        handle_frame(link, kind, plain.subspan(offset, size));
        offset += size;
    }
}

void SecureTransport::handle_frame(Link& link, u8 kind, Span<const u8> body) noexcept {
    switch (static_cast<wire::FrameKind>(kind)) {
        case wire::FrameKind::Datagram: {
            ChannelId channel = 0;
            ReceiveVerdict verdict = ReceiveVerdict::Delivered;
            if (!link.endpoint.ingest(body, channel, verdict)) {
                ++counters_.packets_malformed;
            } else if (verdict == ReceiveVerdict::Duplicate) {
                ++link.stats.datagrams_duplicate;
            }
            return;
        }
        case wire::FrameKind::Ping:
            handle_ping(link, body);
            return;
        case wire::FrameKind::Pong:
            handle_pong(link, body);
            return;
        case wire::FrameKind::Disconnect:
            close_link(link, DisconnectReason::PeerRequest, false);
            return;
        case wire::FrameKind::Deny:
            if (link.phase == Phase::Response && body.size() == 1) {
                close_link(link, static_cast<DisconnectReason>(body[0]), false);
            }
            return;
        case wire::FrameKind::Accept:
            return;
    }
    ++counters_.packets_malformed;
}

void SecureTransport::handle_ping(Link& link, Span<const u8> body) noexcept {
    if (body.size() != wire::kPingBytes) {
        ++counters_.packets_malformed;
        return;
    }
    const u64 received = wire::get_u64(body.data() + 12);
    const u64 expected = wire::get_u64(body.data() + 20);
    // The peer's count of what it received from us: the loss of the direction we send in.
    // The rate answers each measured interval once, not every ping that repeats the same estimate.
    if (link.outbound_loss.report(received, expected)) {
        link.rate.on_report(link.outbound_loss.per_mille(), link.rtt.smoothed(),
                            link.rtt.minimum());
    }

    u8 pong[wire::kPongBytes];
    std::memcpy(pong, body.data(), 12);  // The id and the sender's clock, echoed.
    (void)queue_frame(link, static_cast<u8>(wire::FrameKind::Pong),
                      Span<const u8>(pong, sizeof(pong)), true, false);
}

void SecureTransport::handle_pong(Link& link, Span<const u8> body) noexcept {
    if (body.size() != wire::kPongBytes) {
        ++counters_.packets_malformed;
        return;
    }
    const u64 sent_us = wire::get_u64(body.data() + 4);
    if (sent_us <= now_us_) {
        const u64 elapsed = now_us_ - sent_us;
        link.rtt.sample(elapsed > 0xFFFF'FFFFULL ? 0xFFFF'FFFFU : static_cast<u32>(elapsed));
    }
}

// --- Outbound ------------------------------------------------------------------------------------

Status SecureTransport::queue_frame(Link& link, u8 kind, Span<const u8> body, bool control,
                                    bool reliable) noexcept {
    scratch_.clear();
    if (Status framed = wire::append_frame(static_cast<wire::FrameKind>(kind), body, scratch_);
        !framed) {
        return framed;
    }
    FrameQueue& queue = control ? link.control : link.data;
    return queue.push(scratch_.span(), now_us_, reliable);
}

Status SecureTransport::queue_datagrams(Link& link, Span<const u8> wire_bytes,
                                        bool control) noexcept {
    usize offset = 0;
    while (offset + kDatagramHeaderSize <= wire_bytes.size()) {
        DatagramHeader header;
        if (!decode_header(wire_bytes.subspan(offset), header)) {
            return fail(ErrorCode::Internal, "networking: the reliability layer framed garbage");
        }
        const usize size = kDatagramHeaderSize + header.payload_size;
        // A bare acknowledgement carries no payload and is control traffic: it must reach the peer
        // however congested the link is, or the peer retransmits into the congestion.
        const bool bare = header.payload_size == 0;
        if (Status queued = queue_frame(link, static_cast<u8>(wire::FrameKind::Datagram),
                                        wire_bytes.subspan(offset, size), control || bare,
                                        is_reliable(header.delivery));
            !queued) {
            return queued;
        }
        offset += size;
    }
    return ok();
}

void SecureTransport::tick_link(Link& link) noexcept {
    if (!link.established()) {
        tick_handshake(link);
        return;
    }
    tick_connection(link);
}

void SecureTransport::tick_handshake(Link& link) noexcept {
    if (!link.client) {
        return;
    }
    if (link.clock_pending) {
        // Measured from zero, a host whose clock reads the time since boot would time the
        // handshake out before its first packet could arrive.
        link.clock_pending = false;
        link.handshake_started_us = now_us_;
        link.handshake_sent_us = now_us_;
    }
    if (now_us_ - link.handshake_started_us >
        static_cast<u64>(config_.handshake_timeout_ms) * 1000) {
        close_link(link, DisconnectReason::HandshakeTimeout, false);
        return;
    }
    if (now_us_ - link.handshake_sent_us >= static_cast<u64>(config_.handshake_resend_ms) * 1000) {
        link.handshake_sent_us = now_us_;
        transmit(link.address, link.handshake_packet.span());
    }
}

void SecureTransport::tick_connection(Link& link) noexcept {
    if (now_us_ - link.last_received_us > static_cast<u64>(config_.connection_timeout_ms) * 1000) {
        close_link(link, DisconnectReason::Timeout, false);
        return;
    }
    if (config_.disconnect_on_abandoned && link.endpoint.abandoned()) {
        close_link(link, DisconnectReason::ReliableAbandoned, true);
        return;
    }
    link.reassembler.expire(now_us_, static_cast<u64>(config_.reassembly_timeout_ms) * 1000);
    if (now_us_ - link.last_ping_us >= static_cast<u64>(config_.ping_interval_ms) * 1000) {
        send_ping(link);
    }
    collected_.clear();
    if (link.endpoint.collect_outgoing(now_us_ / 1000, collected_)) {
        (void)queue_datagrams(link, collected_.span(), false);
    }
    flush(link);
}

void SecureTransport::send_ping(Link& link) noexcept {
    link.last_ping_us = now_us_;
    (void)link.inbound_loss.report(link.inbound.received(), link.inbound.expected());
    u8 body[wire::kPingBytes];
    wire::put_u32(body, ++link.ping_id);
    wire::put_u64(body + 4, now_us_);
    wire::put_u64(body + 12, link.inbound.received());
    wire::put_u64(body + 20, link.inbound.expected());
    (void)queue_frame(link, static_cast<u8>(wire::FrameKind::Ping),
                      Span<const u8>(body, sizeof(body)), true, false);
}

void SecureTransport::expire_unreliable(Link& link) const noexcept {
    const u64 hold_us = static_cast<u64>(config_.unreliable_hold_ms) * 1000;
    while (!link.data.empty()) {
        const FrameQueue::Entry& front = link.data.front();
        if (front.reliable || now_us_ - front.enqueued_us <= hold_us) {
            return;
        }
        link.data.pop();
        ++link.stats.datagrams_throttled;
    }
}

void SecureTransport::flush(Link& link) noexcept {
    if (link.dead || !link.established()) {
        return;
    }
    link.rate.refill(now_us_);
    expire_unreliable(link);
    while (emit_packet(link, false)) {
    }
}

bool SecureTransport::emit_packet(Link& link, bool control_only) noexcept {
    const usize room = config_.mtu - wire::kDataOverhead;
    plain_.clear();
    while (!link.control.empty() && plain_.size() + link.control.front().size <= room) {
        if (!plain_.append(link.control.view(link.control.front()))) {
            return false;
        }
        link.control.pop();
    }
    bool carries_data = false;
    while (!control_only && !link.data.empty()) {
        const FrameQueue::Entry& front = link.data.front();
        const usize grown = plain_.size() + front.size;
        if (grown > room || link.rate.tokens() < static_cast<i64>(grown + wire::kDataOverhead)) {
            break;
        }
        if (!plain_.append(link.data.view(front))) {
            return false;
        }
        link.data.pop();
        carries_data = true;
    }
    if (plain_.empty()) {
        return false;
    }
    const u32 size = static_cast<u32>(plain_.size() + wire::kDataOverhead);
    if (carries_data) {
        (void)link.rate.try_spend(size);
    } else {
        link.rate.spend_anyway(size);
    }
    return static_cast<bool>(seal_and_send(link, plain_.span()));
}

Status SecureTransport::seal_and_send(Link& link, Span<const u8> plain) noexcept {
    wire::DataHeader header;
    header.connection = link.connection_id;
    header.counter = link.send_counter++;
    if (Status sealed = wire::seal_data(header, link.outgoing(), link.send_key, plain, wire_);
        !sealed) {
        return sealed;
    }
    link.stats.datagrams_out += 1;
    link.stats.bytes_out += wire_.size();
    transmit(link.address, wire_.span());
    return ok();
}

void SecureTransport::transmit(const SocketAddress& to, Span<const u8> wire_bytes) noexcept {
    (void)socket_->send_to(to, wire_bytes);
}

// --- Delivery ------------------------------------------------------------------------------------

bool SecureTransport::drain(Datagram& out) noexcept {
    const u32 count = static_cast<u32>(slots_.size());
    for (u32 visited = 0; visited <= count * kMaxChannels && count != 0; ++visited) {
        if (drain_slot_ >= count) {
            drain_slot_ = 0;
        }
        Link* link = slots_[drain_slot_];
        if (link == nullptr || link->dead || !link->established()) {
            ++drain_slot_;
            drain_channel_ = 0;
            continue;
        }
        Span<const u8> payload;
        DeliveryMode mode = DeliveryMode::Unreliable;
        while (
            link->endpoint.next_delivery(static_cast<ChannelId>(drain_channel_), payload, mode)) {
            Span<const u8> message;
            const ReassemblyVerdict verdict = link->reassembler.accept(
                static_cast<ChannelId>(drain_channel_), payload, now_us_, message);
            if (verdict == ReassemblyVerdict::Complete) {
                out.peer = link->peer;
                out.channel = static_cast<ChannelId>(drain_channel_);
                out.delivery = mode;
                out.bytes = message;
                return true;
            }
        }
        if (++drain_channel_ >= kMaxChannels) {
            drain_channel_ = 0;
            ++drain_slot_;
        }
    }
    return false;
}

}  // namespace cy::net
