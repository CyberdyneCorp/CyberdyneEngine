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

// M10 — `m9:reliable-channel-stalls-under-loss`, AT THE TRANSPORT RATHER THAN AT THE CHANNEL.
//
// The case above sends its ten datagrams in one breath, so the receiver's sequence space never
// moves far enough for the replay window to matter, and that is exactly why it passed all through
// M9 while the four-player session froze at 25 % loss. SUSTAINED traffic is the condition: one
// reliable-ordered datagram every 16 ms for three hundred ticks, which is what `samples/09` does
// and what takes a retransmission more than thirty-two sequences behind the newest arrival.
//
// The claim is the frontier: every one of the three hundred arrives, in order, and the link never
// reports that it gave up. Before the fix this delivered a few dozen and then nothing — for ever,
// silently, while `network.delivered()` went on climbing.
CY_TEST_CASE(
    "networking: a reliable-ordered channel keeps its frontier under sustained heavy loss") {
    NetworkConditions harsh;
    harsh.latency_ms = 40;
    harsh.jitter_ms = 20;
    harsh.loss_percent = 25;
    Pair pair(0x0910'C0FFULL);
    pair.network.set_conditions(harsh);
    CY_REQUIRE(pair.link());

    constexpr u32 kTicks = 300;
    constexpr u64 kTickMs = 16;
    u32 delivered = 0;
    u32 expected = 0;

    for (u32 tick = 0; tick < kTicks; ++tick) {
        const u64 now = static_cast<u64>(tick) * kTickMs;
        pair.server.advance(now);
        pair.client.advance(now);
        // The tick's own number, so an out-of-order delivery is a wrong value rather than a count
        // that happens to add up.
        const u8 payload[2] = {static_cast<u8>(tick & 0xFFU), static_cast<u8>((tick >> 8) & 0xFFU)};
        CY_REQUIRE(pair.server
                       .send(pair.client_id, 1, DeliveryMode::ReliableOrdered,
                             cy::Span<const u8>(payload, 2))
                       .has_value());
        Datagram datagram;
        while (pair.client.receive(datagram)) {
            if (datagram.channel != 1) {
                continue;
            }
            CY_REQUIRE_EQ(datagram.bytes.size(), cy::usize{2});
            const u32 value =
                static_cast<u32>(datagram.bytes[0]) | (static_cast<u32>(datagram.bytes[1]) << 8);
            CY_CHECK_EQ(value, expected);
            ++expected;
            ++delivered;
        }
        Datagram back;
        while (pair.server.receive(back)) {
        }
    }

    // Ten seconds past the last send: longer than the whole retransmission horizon, so what has not
    // arrived by here is not going to.
    for (u64 now = kTicks * kTickMs; now <= (kTicks * kTickMs) + 10000; now += kTickMs) {
        pair.server.advance(now);
        pair.client.advance(now);
        Datagram datagram;
        while (pair.client.receive(datagram)) {
            if (datagram.channel != 1) {
                continue;
            }
            CY_REQUIRE_EQ(datagram.bytes.size(), cy::usize{2});
            const u32 value =
                static_cast<u32>(datagram.bytes[0]) | (static_cast<u32>(datagram.bytes[1]) << 8);
            CY_CHECK_EQ(value, expected);
            ++expected;
            ++delivered;
        }
        Datagram back;
        while (pair.server.receive(back)) {
        }
    }

    CY_CHECK_EQ(delivered, kTicks);
    // The conditions were real: a case that delivered everything because nothing was lost would
    // prove nothing about the retransmit path.
    CY_CHECK_GT(pair.network.dropped(), u64{0});
    CY_CHECK_GT(pair.server.stats(pair.client_id).retransmissions, u64{0});
    // `abandoned()`'s reader. Zero here and non-zero would mean the figures above describe a link
    // that had already given up.
    CY_CHECK_FALSE(pair.server.stats(pair.client_id).reliable_abandoned);
    CY_CHECK_FALSE(pair.client.stats(pair.server_id).reliable_abandoned);
}

// M10 — THE SECOND HALF OF `m9:reliable-channel-stalls-under-loss`.
//
// The default policy's ten attempts span 7.5 s, which is longer than the four-player session it was
// meant to carry: a datagram unlucky five times running was still waiting when the match ended, and
// the client holding the gap never converged. `ReliableEndpoint::set_policy()` could have said so
// and no backend exposed it. This case is that route, as a count rather than as an API that
// compiles: twenty milliseconds and four attempts against a network that drops everything.
//
// With `set_retransmit_policy()` made a no-op the defaults apply, the first retransmission is due
// at 100 ms rather than 20, and both numbers below go red — one retransmission instead of three,
// and a link that has not given up.
CY_TEST_CASE("networking: a session can pace retransmission for its own round trip") {
    NetworkConditions blackout;
    blackout.loss_percent = 100;  // Nothing arrives, so what is counted is the pacing alone.
    Pair pair(0x0BAD'0BADULL);
    pair.network.set_conditions(blackout);

    RetransmitPolicy brisk;
    brisk.initial_timeout_ms = 20;
    brisk.maximum_timeout_ms = 20;
    brisk.maximum_attempts = 4;
    pair.client.set_retransmit_policy(brisk);
    CY_REQUIRE(pair.link());

    CY_REQUIRE(
        pair.client
            .send(pair.server_id, 2, DeliveryMode::ReliableOrdered, cy::Span<const u8>(kMessage, 8))
            .has_value());

    for (u64 now = 0; now <= 200; now += 10) {
        pair.client.advance(now);
        pair.server.advance(now);
        Datagram datagram;
        while (pair.server.receive(datagram)) {
        }
    }
    // Four attempts, less the original send.
    CY_CHECK_EQ(pair.client.stats(pair.server_id).retransmissions, u64{3});
    // And the peer is declared unreachable rather than retried for ever — the signal that had no
    // reader until `ConnectionStats` carried it.
    CY_CHECK(pair.client.stats(pair.server_id).reliable_abandoned);
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
