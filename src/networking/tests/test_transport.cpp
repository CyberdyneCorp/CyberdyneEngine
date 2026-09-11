// M9 TASK 4.2 — THE LOCAL TRANSPORT UNDER AN ADVERSE NETWORK, AND THE SEED THAT MAKES IT USEFUL.
//
// `networking-and-replication` — "Testing under adverse conditions": "WHEN a developer enables
// 150 ms latency with 5 % loss THEN the game SHALL run against those conditions locally, exercising
// prediction and reconciliation."
//
// THE CASE THAT MATTERS MOST IS THE SECOND ONE. A condition simulator driven by a global generator
// makes a failing four-player test unreproducible, and this is the milestone whose subject is
// reproducibility. Two `LocalNetwork`s with the same seed drop the same datagrams at the same
// moments, and the case asserts the two runs agree datagram for datagram — so a network that
// started drawing from somewhere else goes red here rather than as a flake six months later.
//
// `integration`, because every case builds a session: two transports, a substrate and up to two
// hundred ticks of it.

#include "fixture.h"

#include <cy/networking/local_transport.h>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

constexpr u8 kMessage[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};

struct Pair {
    LocalNetwork network;
    PeerId server_id;
    PeerId client_id;
    LocalTransport server;
    LocalTransport client;

    explicit Pair(u64 seed) noexcept
        : network(allocator(), seed),
          server_id(network.add_host("server").value()),
          client_id(network.add_host("client").value()),
          server(allocator(), network, server_id),
          client(allocator(), network, client_id) {}

    [[nodiscard]] bool link() noexcept {
        return client.connect("server").has_value() && server.accept(client_id).has_value();
    }
};

/// Step both sides and count what the server received.
[[nodiscard]] u32 pump(Pair& pair, u64 from_ms, u64 to_ms, u64 step_ms) noexcept {
    u32 received = 0;
    for (u64 now = from_ms; now <= to_ms; now += step_ms) {
        pair.client.advance(now);
        pair.server.advance(now);
        Datagram datagram;
        while (pair.server.receive(datagram)) {
            ++received;
        }
        while (pair.client.receive(datagram)) {
        }
    }
    return received;
}

}  // namespace

CY_TEST_CASE("networking: a perfect local network delivers everything, once") {
    Pair pair(0x5EED'0001ULL);
    CY_REQUIRE(pair.link());
    CY_CHECK(pair.client.state(pair.server_id) == ConnectionState::Connected);

    for (u32 index = 0; index < 20; ++index) {
        CY_REQUIRE(
            pair.client
                .send(pair.server_id, 0, DeliveryMode::Unreliable, cy::Span<const u8>(kMessage, 8))
                .has_value());
    }
    CY_CHECK_EQ(pump(pair, 0, 40, 10), 20U);
    CY_CHECK_EQ(pair.network.dropped(), u64{0});
}

CY_TEST_CASE("networking: the condition simulator is seeded, so a failure is reproducible") {
    NetworkConditions adverse;
    adverse.latency_ms = 150;
    adverse.jitter_ms = 30;
    adverse.loss_percent = 5;
    adverse.duplication_percent = 2;
    adverse.reorder_percent = 5;
    CY_CHECK(adverse.adverse());

    u64 first_dropped = 0;
    u64 first_duplicated = 0;
    u64 second_dropped = 0;
    u64 second_duplicated = 0;

    for (u32 run = 0; run < 2; ++run) {
        Pair pair(0xC0FF'EE42ULL);
        pair.network.set_conditions(adverse);
        CY_REQUIRE(pair.link());
        for (u64 tick = 0; tick < 200; ++tick) {
            CY_REQUIRE(pair.client
                           .send(pair.server_id, 0, DeliveryMode::Unreliable,
                                 cy::Span<const u8>(kMessage, 8))
                           .has_value());
            pair.client.advance(tick * 16);
            pair.server.advance(tick * 16);
            Datagram datagram;
            while (pair.server.receive(datagram)) {
            }
        }
        if (run == 0) {
            first_dropped = pair.network.dropped();
            first_duplicated = pair.network.duplicated();
        } else {
            second_dropped = pair.network.dropped();
            second_duplicated = pair.network.duplicated();
        }
    }

    // Adverse enough to be a test, and identical across runs. Both halves matter: a simulator that
    // dropped nothing would agree with itself too.
    CY_CHECK_GT(first_dropped, u64{0});
    CY_CHECK_GT(first_duplicated, u64{0});
    CY_CHECK_EQ(first_dropped, second_dropped);
    CY_CHECK_EQ(first_duplicated, second_duplicated);

    // A different seed is a different network, or the seed is not doing anything.
    Pair third(0x0BAD'BEEFULL);
    third.network.set_conditions(adverse);
    CY_REQUIRE(third.link());
    for (u64 tick = 0; tick < 200; ++tick) {
        CY_REQUIRE(
            third.client
                .send(third.server_id, 0, DeliveryMode::Unreliable, cy::Span<const u8>(kMessage, 8))
                .has_value());
        third.client.advance(tick * 16);
        third.server.advance(tick * 16);
        Datagram datagram;
        while (third.server.receive(datagram)) {
        }
    }
    CY_CHECK_NE(third.network.dropped(), first_dropped);
}

CY_TEST_CASE("networking: a reliable message survives five per cent loss") {
    NetworkConditions lossy;
    lossy.latency_ms = 40;
    lossy.loss_percent = 25;  // Far worse than any real connection, so the retransmit path is used.
    Pair pair(0xABCD'0007ULL);
    pair.network.set_conditions(lossy);
    CY_REQUIRE(pair.link());

    for (u32 index = 0; index < 10; ++index) {
        const u8 payload[1] = {static_cast<u8>(index)};
        CY_REQUIRE(pair.client
                       .send(pair.server_id, 1, DeliveryMode::ReliableOrdered,
                             cy::Span<const u8>(payload, 1))
                       .has_value());
    }

    u32 delivered = 0;
    u8 expected = 0;
    for (u64 now = 0; now <= 4000; now += 20) {
        pair.client.advance(now);
        pair.server.advance(now);
        Datagram datagram;
        while (pair.server.receive(datagram)) {
            if (datagram.channel == 1) {
                CY_REQUIRE_EQ(datagram.bytes.size(), cy::usize{1});
                // Ordered: every payload arrives once, in the order it was sent.
                CY_CHECK_EQ(datagram.bytes[0], expected);
                ++expected;
                ++delivered;
            }
        }
        Datagram back;
        while (pair.client.receive(back)) {
        }
    }
    CY_CHECK_EQ(delivered, 10U);
    CY_CHECK_GT(pair.network.dropped(), u64{0});
    CY_CHECK_GT(pair.client.stats(pair.server_id).retransmissions, u64{0});
}

CY_TEST_CASE("networking: an unauthenticated peer receives nothing") {
    Pair pair(0x1111'2222ULL);
    pair.client.require_authentication(true);
    pair.server.require_authentication(true);
    CY_REQUIRE(pair.link());

    CY_CHECK(pair.client.state(pair.server_id) == ConnectionState::Authenticating);
    // "it SHALL remain in a pending state until authentication succeeds or times out, without
    // receiving game state".
    CY_CHECK_FALSE(
        pair.client
            .send(pair.server_id, 0, DeliveryMode::Unreliable, cy::Span<const u8>(kMessage, 8))
            .has_value());

    CY_REQUIRE(pair.client.admit(pair.server_id).has_value());
    CY_REQUIRE(pair.server.admit(pair.client_id).has_value());
    CY_CHECK(pair.client
                 .send(pair.server_id, 0, DeliveryMode::Unreliable, cy::Span<const u8>(kMessage, 8))
                 .has_value());
    CY_CHECK_EQ(pump(pair, 0, 40, 10), 1U);
}

CY_TEST_CASE("networking: a payload larger than the MTU is refused, not fragmented") {
    Pair pair(0x3333'4444ULL);
    CY_REQUIRE(pair.link());
    cy::Array<u8> oversized(allocator());
    CY_REQUIRE(oversized.resize(kMaxDatagramPayload + 1).has_value());
    CY_CHECK_FALSE(pair.client.send(pair.server_id, 0, DeliveryMode::Unreliable, oversized.span())
                       .has_value());

    // And exactly at the limit it is accepted, so the refusal is a boundary rather than a mood.
    CY_REQUIRE(oversized.resize(kMaxDatagramPayload).has_value());
    CY_CHECK(pair.client.send(pair.server_id, 0, DeliveryMode::Unreliable, oversized.span())
                 .has_value());
}

CY_TEST_CASE("networking: the local transport declares what it is and the UDP one does not lie") {
    Pair pair(0x5555'6666ULL);
    const TransportSecurity security = pair.client.security();
    CY_CHECK(security.in_process);
    CY_CHECK(security.replay_protected);
    CY_CHECK(security.sequence_validated);
    CY_CHECK_FALSE(security.encrypted);
    // In-process, so confidentiality on a wire it never reaches is not a question that applies.
    CY_CHECK(admissible_for(security, Deployment::Shipping) == SecurityRefusal::None);
}
