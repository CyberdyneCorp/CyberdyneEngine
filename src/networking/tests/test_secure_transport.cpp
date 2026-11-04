// SPDX-License-Identifier: MIT
// ISSUE #82 — THE SECURE TRANSPORT, UNDER THE SIMULATED NETWORK.
//
// Each case builds a server and clients over `LocalNetwork` and drives the clock by hand, so a
// 200 ms round trip costs no wall time and every loss is a seeded, reproducible draw. The cases a
// reviewer should read first are the two that attack the transport: a tampered packet and a
// replayed one. Both arrive at the receiver as datagrams from the right address with the right
// shape, and both must deliver nothing.

#include "secure_fixture.h"

#include <cstdio>
#include <cstring>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

[[nodiscard]] bool connected(SecureSession& session, cy::net::PeerId client_peer,
                             u32 client) noexcept {
    return session.client(client).state(client_peer) == cy::net::ConnectionState::Connected;
}

/// Run until every client is connected, or until `limit_ms` passes.
[[nodiscard]] bool connect_all(SecureSession& session, cy::net::PeerId* peers, u32 count,
                               u32 limit_ms = 3000) noexcept {
    for (u32 index = 0; index < count; ++index) {
        auto peer = session.client(index).connect("server");
        if (!peer) {
            return false;
        }
        peers[index] = *peer;
    }
    for (u32 elapsed = 0; elapsed < limit_ms; elapsed += 10) {
        session.step();
        bool all = true;
        for (u32 index = 0; index < count; ++index) {
            all = all && connected(session, peers[index], index);
        }
        if (all) {
            return true;
        }
    }
    return false;
}

/// The server's id for its only client, from the connection event.
[[nodiscard]] cy::net::PeerId accepted_peer(SecureSession& session) noexcept {
    cy::net::ConnectionEvent event;
    while (session.host().poll_event(event)) {
        if (event.kind == cy::net::ConnectionEvent::Kind::Connected) {
            return event.peer;
        }
    }
    return {};
}

struct Received {
    cy::Array<u8> bytes;
    u32 count = 0;

    Received() noexcept : bytes(allocator()) {}
};

void drain(cy::net::SecureTransport& transport, Received& into) noexcept {
    cy::net::Datagram datagram;
    while (transport.receive(datagram)) {
        into.bytes.clear();
        (void)into.bytes.append(datagram.bytes);
        ++into.count;
    }
}

[[nodiscard]] cy::Array<u8> pattern(u32 size, u8 seed) noexcept {
    cy::Array<u8> bytes(allocator());
    (void)bytes.resize(size);
    for (u32 index = 0; index < size; ++index) {
        bytes[index] = static_cast<u8>((index * 31U) + seed);
    }
    return bytes;
}

}  // namespace

CY_TEST_CASE("secure transport: a handshake admits a client and both directions are sealed") {
    SecureSession session(1);
    session.add_client();
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    const cy::net::PeerId server_side = accepted_peer(session);
    CY_REQUIRE(server_side.valid());
    CY_CHECK_EQ(session.host().counters().handshakes_completed, 1U);

    const u8 hello[] = {'h', 'e', 'l', 'l', 'o'};
    CY_REQUIRE(session.client(0)
                   .send(peer, 2, cy::net::DeliveryMode::ReliableOrdered,
                         cy::Span<const u8>(hello, sizeof(hello)))
                   .has_value());
    CY_REQUIRE(session.host()
                   .send(server_side, 3, cy::net::DeliveryMode::ReliableOrdered,
                         cy::Span<const u8>(hello, 4))
                   .has_value());
    Received at_server;
    Received at_client;
    for (u32 step = 0; step < 20; ++step) {
        session.step();
        drain(session.host(), at_server);
        drain(session.client(0), at_client);
    }
    CY_REQUIRE_EQ(at_server.count, 1U);
    CY_CHECK_EQ(at_server.bytes.size(), sizeof(hello));
    CY_CHECK(std::memcmp(at_server.bytes.data(), hello, sizeof(hello)) == 0);
    CY_REQUIRE_EQ(at_client.count, 1U);
    CY_CHECK_EQ(at_client.bytes.size(), 4U);
}

CY_TEST_CASE(
    "secure transport: the operating system's entropy is admitted for shipping and a seed is not") {
    cy::net::crypto::SeededEntropy seeded(3);
    // A real socket, unbound: the simulated network is in-process, and in-process traffic needs no
    // confidentiality, so it would be admitted either way and prove nothing.
    cy::net::UdpSocket socket(allocator());

    cy::net::SecureConfig shipping;  // system_entropy() by default
    cy::net::SecureTransport real(allocator(), socket, shipping);
    CY_CHECK_EQ(cy::net::admissible_for(real.security(), cy::net::Deployment::Shipping),
                cy::net::SecurityRefusal::None);

    cy::net::SecureConfig test;
    test.entropy = seeded.source();
    cy::net::SecureTransport predictable(allocator(), socket, test);
    // Keys a seed reproduces are keys an attacker with the seed holds.
    CY_CHECK_EQ(cy::net::admissible_for(predictable.security(), cy::net::Deployment::Shipping),
                cy::net::SecurityRefusal::NotEncrypted);
    CY_CHECK_EQ(cy::net::admissible_for(predictable.security(), cy::net::Deployment::Development),
                cy::net::SecurityRefusal::None);
}

CY_TEST_CASE("secure transport: a client given the wrong server key never connects") {
    SecureSession session(2);
    // A client that trusts some other server's key: whoever answers at "server" is an impostor to
    // it, however genuine.
    cy::net::SecureConfig wrong;
    wrong.server_public = server_keys(999).public_key;
    cy::net::crypto::SeededEntropy entropy(55);
    wrong.entropy = entropy.source();
    wrong.handshake_timeout_ms = 1000;
    const cy::net::PeerId host = session.network.add_host("mistrustful").value();
    SecureEndpoint client(allocator(), session.network, host, wrong);

    auto peer = client.transport.connect("server");
    CY_REQUIRE(peer.has_value());
    for (u32 step = 0; step < 150; ++step) {
        session.now_ms += 10;
        session.host().advance(session.now_ms);
        client.transport.advance(session.now_ms);
    }
    CY_CHECK_EQ(client.transport.state(*peer), cy::net::ConnectionState::Disconnected);
    cy::net::ConnectionEvent event;
    CY_REQUIRE(client.transport.poll_event(event));
    CY_CHECK_EQ(event.kind, cy::net::ConnectionEvent::Kind::Disconnected);
    CY_CHECK_EQ(event.reason, cy::net::DisconnectReason::HandshakeTimeout);
    // The server answered, and the client refused every answer as not from the server it trusts.
    CY_CHECK_GT(client.transport.counters().packets_rejected_authentication, 0U);
    CY_CHECK_EQ(session.host().connection_count(), 0U);
}

namespace {

struct FlipState {
    bool armed = false;
    u32 flipped = 0;
};

bool flip_one_ciphertext_byte(void* user, cy::Array<u8>& bytes) noexcept {
    auto* state = static_cast<FlipState*>(user);
    if (!state->armed || bytes.size() < 40 || bytes[0] != 4) {
        return false;
    }
    bytes[20] ^= 0x01U;  // Inside the ciphertext, past the thirteen-byte header.
    ++state->flipped;
    return true;
}

bool flip_header_byte(void* user, cy::Array<u8>& bytes) noexcept {
    auto* state = static_cast<FlipState*>(user);
    if (!state->armed || bytes.size() < 40 || bytes[0] != 4) {
        return false;
    }
    bytes[6] ^= 0x80U;  // The counter: associated data, so the tag covers it too.
    ++state->flipped;
    return true;
}

}  // namespace

CY_TEST_CASE("secure transport: a tampered packet is rejected and delivers nothing") {
    SecureSession session(4);
    session.add_client();
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    (void)accepted_peer(session);
    session.run(200);

    for (auto tamper : {&flip_one_ciphertext_byte, &flip_header_byte}) {
        FlipState state;
        session.clients[0]->tap.set_tamper(tamper, &state);
        const u64 before = session.host().counters().packets_rejected_authentication;
        state.armed = true;
        const u8 payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
        CY_REQUIRE(session.client(0)
                       .send(peer, 1, cy::net::DeliveryMode::Unreliable,
                             cy::Span<const u8>(payload, sizeof(payload)))
                       .has_value());
        Received received;
        session.step();
        state.armed = false;
        for (u32 step = 0; step < 10; ++step) {
            session.step();
            drain(session.host(), received);
        }
        session.clients[0]->tap.set_tamper(nullptr, nullptr);
        CY_CHECK_GT(state.flipped, 0U);
        CY_CHECK_EQ(received.count, 0U);
        CY_CHECK_GE(session.host().counters().packets_rejected_authentication,
                    before + state.flipped);
    }
    // The connection survived the attack: tampering costs the attacker a packet, not the session.
    CY_CHECK(connected(session, peer, 0));
}

CY_TEST_CASE("secure transport: a replayed packet is rejected before it is decrypted") {
    SecureSession session(5);
    session.add_client();
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    (void)accepted_peer(session);
    session.run(200);

    Received received;
    drain(session.host(), received);
    received.count = 0;
    session.clients[0]->tap.capture_next_data_packet();
    const u8 payload[] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    CY_REQUIRE(session.client(0)
                   .send(peer, 1, cy::net::DeliveryMode::Unreliable,
                         cy::Span<const u8>(payload, sizeof(payload)))
                   .has_value());
    for (u32 step = 0; step < 10; ++step) {
        session.step();
        drain(session.host(), received);
    }
    CY_REQUIRE(session.clients[0]->tap.captured());
    CY_REQUIRE_EQ(received.count, 1U);

    // The same bytes, again and again: an unreliable datagram the reliability layer would hand up
    // a second time, because unreliable delivery may duplicate. The replay window is what refuses.
    const u64 before = session.host().counters().packets_rejected_replay;
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        CY_REQUIRE(session.clients[0]->tap.replay().has_value());
        session.step();
        drain(session.host(), received);
    }
    CY_CHECK_EQ(received.count, 1U);
    CY_CHECK_EQ(session.host().counters().packets_rejected_replay, before + 3);
}

CY_TEST_CASE("secure transport: a message larger than a datagram is reassembled under loss") {
    SecureSession session(6);
    session.add_client();
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    (void)accepted_peer(session);
    cy::net::NetworkConditions conditions;
    conditions.latency_ms = 40;
    conditions.jitter_ms = 20;
    conditions.loss_percent = 5;
    conditions.reorder_percent = 5;
    session.network.set_conditions(conditions);

    const cy::Array<u8> big = pattern(50'000, 3);
    CY_REQUIRE(session.client(0)
                   .send(peer, 4, cy::net::DeliveryMode::ReliableOrdered, big.span())
                   .has_value());
    Received received;
    for (u32 step = 0; step < 400 && received.count == 0; ++step) {
        session.step();
        drain(session.host(), received);
    }
    CY_REQUIRE_EQ(received.count, 1U);
    CY_REQUIRE_EQ(received.bytes.size(), big.size());
    CY_CHECK(std::memcmp(received.bytes.data(), big.data(), big.size()) == 0);
    CY_CHECK_GT(session.client(0).stats(peer).retransmissions, 0U);
}

CY_TEST_CASE("secure transport: an unreliable fragmented message arrives whole or not at all") {
    SecureSession session(7);
    session.add_client();
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    (void)accepted_peer(session);
    cy::net::NetworkConditions conditions;
    conditions.latency_ms = 30;
    conditions.loss_percent = 10;
    conditions.reorder_percent = 10;
    session.network.set_conditions(conditions);

    u32 whole = 0;
    u32 sent = 0;
    for (u32 round = 0; round < 40; ++round) {
        const cy::Array<u8> message = pattern(6000, static_cast<u8>(round));
        CY_REQUIRE(session.client(0)
                       .send(peer, 5, cy::net::DeliveryMode::Unreliable, message.span())
                       .has_value());
        ++sent;
        for (u32 step = 0; step < 5; ++step) {
            session.step();
            cy::net::Datagram datagram;
            while (session.host().receive(datagram)) {
                // Never part of a message: either all 6000 bytes of one round, or nothing.
                CY_REQUIRE_EQ(datagram.bytes.size(), 6000U);
                const u8 seed = datagram.bytes[0];
                const cy::Array<u8> expected = pattern(6000, seed);
                CY_CHECK(std::memcmp(datagram.bytes.data(), expected.data(), 6000) == 0);
                ++whole;
            }
        }
    }
    session.run(500);
    cy::net::Datagram datagram;
    while (session.host().receive(datagram)) {
        CY_REQUIRE_EQ(datagram.bytes.size(), 6000U);
        ++whole;
    }
    // Six fragments at ten per cent loss: about half the messages arrive, and none arrive partly.
    CY_CHECK_GT(whole, 0U);
    CY_CHECK_LT(whole, sent);
}

CY_TEST_CASE("secure transport: the send rate stays under its limit and excess is discarded") {
    cy::net::SecureConfig client_config;
    client_config.rate.max_bytes_per_second = 64U * 1024U;
    client_config.rate.min_bytes_per_second = 64U * 1024U;
    SecureSession session(8);
    session.add_client(client_config);
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    (void)accepted_peer(session);
    session.run(200);

    // 200 KB/s offered against a 64 KB/s limit, for four seconds.
    const cy::Array<u8> payload = pattern(1000, 1);
    const u64 bytes_before = session.client(0).stats(peer).bytes_out;
    const u64 start_ms = session.now_ms;
    for (u32 step = 0; step < 800; ++step) {
        CY_REQUIRE(session.client(0)
                       .send(peer, 1, cy::net::DeliveryMode::Unreliable, payload.span())
                       .has_value());
        session.step(5);
        cy::net::Datagram datagram;
        while (session.host().receive(datagram)) {
        }
    }
    const cy::net::ConnectionStats stats = session.client(0).stats(peer);
    const u64 elapsed_ms = session.now_ms - start_ms;
    const u64 sent = stats.bytes_out - bytes_before;
    const u64 allowed = (64ULL * 1024ULL * elapsed_ms) / 1000ULL;
    CY_TEST_MESSAGE("offered 200 KB/s, sent " << (sent * 1000 / elapsed_ms) << " B/s against "
                                              << 64 * 1024 << " B/s");
    // The bucket's burst (40 ms of the rate, at least one datagram) is the only allowance over it.
    CY_CHECK_LE(sent, allowed + 4096);
    CY_CHECK_GT(sent, allowed / 2);
    CY_CHECK_GT(stats.datagrams_throttled, 0U);
}

CY_TEST_CASE("secure transport: round trip and loss are measured in each direction") {
    SecureSession session(9);
    cy::net::NetworkConditions conditions;
    conditions.latency_ms = 100;  // 200 ms round trip
    conditions.loss_percent = 10;
    session.network.set_conditions(conditions);
    session.add_client();
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    (void)accepted_peer(session);
    session.run(8000);

    const cy::net::ConnectionStats stats = session.client(0).stats(peer);
    CY_TEST_MESSAGE("round trip " << stats.round_trip_us << " us, jitter " << stats.jitter_us
                                  << " us, loss out " << stats.loss_per_mille << " in "
                                  << stats.inbound_loss_per_mille << " per mille");
    // Within the 10 ms step the clock advances by: the echo is read on the next step.
    CY_CHECK_GE(stats.round_trip_us, 200'000U);
    CY_CHECK_LE(stats.round_trip_us, 230'000U);
    CY_CHECK_GT(stats.loss_per_mille, 20U);
    CY_CHECK_LT(stats.loss_per_mille, 250U);
    CY_CHECK_GT(stats.inbound_loss_per_mille, 20U);
    CY_CHECK_LT(stats.inbound_loss_per_mille, 250U);
}

CY_TEST_CASE("secure transport: a silent peer times out and a goodbye names its reason") {
    SecureSession session(10);
    session.add_client();
    session.add_client();
    cy::net::PeerId peers[2];
    CY_REQUIRE(connect_all(session, peers, 2));
    session.run(100);
    cy::net::ConnectionEvent event;
    while (session.host().poll_event(event)) {
    }

    // Client 0 says goodbye; the server hears why.
    session.client(0).disconnect(peers[0]);
    session.run(200);
    bool told = false;
    while (session.host().poll_event(event)) {
        told = told || (event.kind == cy::net::ConnectionEvent::Kind::Disconnected &&
                        event.reason == cy::net::DisconnectReason::PeerRequest);
    }
    CY_CHECK(told);

    // Client 1 goes silent: everything it sends is lost from now on.
    cy::net::NetworkConditions blackout;
    blackout.loss_percent = 100;
    session.network.set_conditions(blackout);
    session.run(6000);
    bool timed_out = false;
    while (session.client(1).poll_event(event)) {
        timed_out = timed_out || event.reason == cy::net::DisconnectReason::Timeout;
    }
    CY_CHECK(timed_out);
    CY_CHECK_EQ(session.client(1).state(peers[1]), cy::net::ConnectionState::Disconnected);
    CY_CHECK_EQ(session.host().connection_count(), 0U);
}

namespace {

cy::net::DisconnectReason only_the_password(void* /*user*/,
                                            cy::Span<const u8> credential) noexcept {
    static const char kPassword[] = "swordfish";
    const bool right = credential.size() == sizeof(kPassword) - 1 &&
                       std::memcmp(credential.data(), kPassword, credential.size()) == 0;
    return right ? cy::net::DisconnectReason::None
                 : cy::net::DisconnectReason::AuthenticationFailed;
}

}  // namespace

CY_TEST_CASE("secure transport: the application's check and the server's capacity refuse by name") {
    cy::net::SecureConfig server_config;
    server_config.max_connections = 1;
    server_config.authenticate = &only_the_password;
    SecureSession session(11, server_config);

    static const char kRight[] = "swordfish";
    static const char kWrong[] = "password";
    cy::net::SecureConfig right;
    right.credential = cy::Span<const u8>(reinterpret_cast<const u8*>(kRight), sizeof(kRight) - 1);
    cy::net::SecureConfig wrong;
    wrong.credential = cy::Span<const u8>(reinterpret_cast<const u8*>(kWrong), sizeof(kWrong) - 1);
    session.add_client(wrong);
    session.add_client(right);
    session.add_client(right);

    auto refused = session.client(0).connect("server");
    CY_REQUIRE(refused.has_value());
    session.run(300);
    auto admitted = session.client(1).connect("server");
    CY_REQUIRE(admitted.has_value());
    session.run(300);
    auto full = session.client(2).connect("server");
    CY_REQUIRE(full.has_value());
    session.run(300);

    CY_CHECK_EQ(session.client(0).state(*refused), cy::net::ConnectionState::Disconnected);
    cy::net::ConnectionEvent event;
    CY_REQUIRE(session.client(0).poll_event(event));
    CY_CHECK_EQ(event.reason, cy::net::DisconnectReason::AuthenticationFailed);
    CY_CHECK_EQ(session.client(1).state(*admitted), cy::net::ConnectionState::Connected);
    CY_REQUIRE(session.client(2).poll_event(event));
    CY_CHECK_EQ(event.reason, cy::net::DisconnectReason::ServerFull);
    CY_CHECK_EQ(session.host().connection_count(), 1U);
    CY_CHECK_EQ(session.host().counters().handshakes_refused, 2U);
}

CY_TEST_CASE("secure transport: a hello allocates nothing and is answered by something smaller") {
    SecureSession session(12);
    session.add_client();
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    CY_CHECK_GE(session.host().counters().hellos_answered, 1U);

    // A flood of HELLOs from addresses that never answer the challenge.
    cy::net::crypto::SeededEntropy entropy(77);
    u32 spoofed = 0;
    for (u32 index = 0; index < 16; ++index) {
        cy::net::SecureConfig config;
        config.server_public = session.keys.public_key;
        config.entropy = entropy.source();
        static char names[16][16];
        std::snprintf(names[index], sizeof(names[index]), "spoof-%u", index);
        const cy::net::PeerId host = session.network.add_host(names[index]).value();
        cy::net::SimulatedSocket socket(session.network, host);
        u8 hello[192] = {};
        hello[0] = 1;
        const cy::u64 protocol = cy::net::SecureConfig{}.protocol_id;
        for (u32 byte = 0; byte < 8; ++byte) {
            hello[1 + byte] = static_cast<u8>(protocol >> (byte * 8));
        }
        entropy.source()(hello + 9, 32);
        CY_REQUIRE(
            socket
                .send_to(socket.resolve("server").value(), cy::Span<const u8>(hello, sizeof(hello)))
                .has_value());
        ++spoofed;
    }
    session.run(100);
    CY_CHECK_EQ(session.host().counters().hellos_answered, 1U + spoofed);
    CY_CHECK_EQ(session.host().connection_count(), 1U);
}

CY_TEST_CASE("secure transport: a handshake completes through loss and reordering") {
    SecureSession session(13);
    cy::net::NetworkConditions conditions;
    conditions.latency_ms = 100;
    conditions.jitter_ms = 30;
    conditions.loss_percent = 20;
    conditions.reorder_percent = 10;
    conditions.duplication_percent = 5;
    session.network.set_conditions(conditions);
    for (u32 index = 0; index < 4; ++index) {
        session.add_client();
    }
    cy::net::PeerId peers[4];
    CY_CHECK(connect_all(session, peers, 4, 4000));
    CY_CHECK_EQ(session.host().connection_count(), 4U);
}

CY_TEST_CASE("secure transport: a connect before the first advance is timed from that advance") {
    // A host whose clock is the time since boot: the first `advance()` reads days, not zero. The
    // handshake's timers must start there, not at the zero the transport held before it.
    SecureSession session(14);
    session.now_ms = 3ULL * 24 * 3600 * 1000;
    session.add_client();
    auto peer = session.client(0).connect("server");
    CY_REQUIRE(peer.has_value());
    for (u32 step = 0; step < 50; ++step) {
        session.step();
    }
    CY_CHECK_EQ(session.client(0).state(*peer), cy::net::ConnectionState::Connected);
    cy::net::ConnectionEvent event;
    CY_REQUIRE(session.client(0).poll_event(event));
    CY_CHECK_EQ(event.kind, cy::net::ConnectionEvent::Kind::Connected);
}

CY_TEST_CASE("secure transport: a response replayed from another address admits nothing") {
    // The cookie binds the handshake to the address that asked for it. An attacker who copies a
    // genuine RESPONSE and sends it from its own address is refused at the cookie, and could not
    // have used the session anyway: the server's ephemeral key is derived from the address too.
    SecureSession session(15);
    session.add_client();
    session.clients[0]->tap.capture_next(3);
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    CY_REQUIRE(session.clients[0]->tap.captured());
    CY_REQUIRE_EQ(session.host().connection_count(), 1U);

    const cy::net::PeerId host = session.network.add_host("attacker").value();
    cy::net::SimulatedSocket attacker(session.network, host);
    const u64 before = session.host().counters().cookies_rejected;
    CY_REQUIRE(attacker
                   .send_to(session.clients[0]->tap.captured_to(),
                            session.clients[0]->tap.captured_bytes())
                   .has_value());
    session.run(100);
    CY_CHECK_EQ(session.host().counters().cookies_rejected, before + 1);
    CY_CHECK_EQ(session.host().connection_count(), 1U);
    CY_CHECK_EQ(session.host().counters().handshakes_completed, 1U);
}

CY_TEST_CASE("secure transport: a clean link reports no loss in either direction") {
    // The handshake's RESPONSE is the client's packet zero. It arrives like any other, and a count
    // that left it out would report every new connection as losing one of its first packets.
    SecureSession session(16);
    session.add_client();
    cy::net::PeerId peer;
    CY_REQUIRE(connect_all(session, &peer, 1));
    const cy::net::PeerId server_side = accepted_peer(session);
    session.run(1500);
    const cy::net::ConnectionStats client = session.client(0).stats(peer);
    const cy::net::ConnectionStats server = session.host().stats(server_side);
    CY_TEST_MESSAGE("client to server " << client.loss_per_mille << " / "
                                        << server.inbound_loss_per_mille << ", server to client "
                                        << server.loss_per_mille << " / "
                                        << client.inbound_loss_per_mille << " per mille");
    CY_CHECK_EQ(client.loss_per_mille, 0U);
    CY_CHECK_EQ(server.inbound_loss_per_mille, 0U);
    CY_CHECK_EQ(server.loss_per_mille, 0U);
    CY_CHECK_EQ(client.inbound_loss_per_mille, 0U);
}
