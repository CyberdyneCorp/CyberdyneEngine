// M9 TASK 4.2 — A REAL SOCKET, AND WHAT THIS ONE DOES NOT PROVIDE.
//
// The local transport composes `ReliableEndpoint` over an in-process substrate; this one composes
// the same class over a POSIX UDP socket. The case below runs both ends on the loopback, so the
// bytes genuinely leave the process and come back through the kernel — which is the only way to
// find the things a simulated network cannot have wrong: an address that does not parse, a socket
// that blocks, a datagram from a stranger.
//
// **THE SECOND CASE IS THE ONE TO READ.** `UdpTransport::security()` reports no encryption and no
// peer authentication, and `admissible_for()` therefore REFUSES this transport for a shipping
// deployment. That is `networking-and-replication`'s "no transport SHALL be offered as a default
// without them" as a refusal rather than as an intention, and it is how this milestone records that
// DTLS is a dependency it did not take.
//
// ONE OPERATING SYSTEM. This host is Linux. The socket half compiles on Linux and macOS; on Windows
// `open()` returns `Unsupported` rather than failing to link, and **Windows is reported
// unverified** because nothing here has been run on it.
//
// A separate suite from the rest, so that a machine which refuses to bind a socket loses one suite
// rather than fourteen cases about arithmetic.

#include "fixture.h"

#include <cy/networking/udp_transport.h>

#include <cstdio>
#include <cstring>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

constexpr u8 kMessage[12] = {'h', 'e', 'l', 'l', 'o', ' ', 'p', 'e', 'e', 'r', '!', 0};

}  // namespace

CY_TEST_CASE("networking: a UDP address parses, or it does not parse at all") {
    const UdpAddress good = parse_udp_address("127.0.0.1:9000");
    CY_CHECK(good.valid());
    CY_CHECK_EQ(good.ipv4, 0x7F00'0001U);
    CY_CHECK_EQ(good.port, cy::u16{9000});

    // Every one of these is a partial parse in a hand-rolled reader that stops at the first
    // surprise. None of them may produce a half-built address.
    CY_CHECK_FALSE(parse_udp_address("127.0.0.1").valid());
    CY_CHECK_FALSE(parse_udp_address("127.0.0.1:").valid());
    CY_CHECK_FALSE(parse_udp_address("127.0.0.1:0").valid());
    CY_CHECK_FALSE(parse_udp_address("127.0.0.1:70000").valid());
    CY_CHECK_FALSE(parse_udp_address("127.0.0:9000").valid());
    CY_CHECK_FALSE(parse_udp_address("127.0.0.1.5:9000").valid());
    CY_CHECK_FALSE(parse_udp_address("999.0.0.1:9000").valid());
    CY_CHECK_FALSE(parse_udp_address("localhost:9000").valid());
    CY_CHECK_FALSE(parse_udp_address(":9000").valid());
    CY_CHECK_FALSE(parse_udp_address(nullptr).valid());
}

CY_TEST_CASE("networking: the UDP transport says plainly what it does not provide") {
    UdpTransport transport(allocator());
    const TransportSecurity security = transport.security();
    CY_CHECK(security.replay_protected);
    CY_CHECK(security.sequence_validated);
    CY_CHECK_FALSE(security.encrypted);
    CY_CHECK_FALSE(security.authenticated);
    CY_CHECK_FALSE(security.in_process);

    // Correctness is provided, so development is admissible...
    CY_CHECK(admissible_for(security, Deployment::Development) == SecurityRefusal::None);
    // ...and a shipping build is refused, by name. If someone adds DTLS this goes to `None`; if
    // someone flips `encrypted` without adding it, the claim is the lie rather than this check.
    CY_CHECK(admissible_for(security, Deployment::Shipping) == SecurityRefusal::NotEncrypted);
    CY_CHECK_EQ(std::strcmp(transport.name(), "udp"), 0);
}

CY_TEST_CASE("networking: two sockets on the loopback exchange reliable and unreliable traffic") {
    if (!udp_available()) {
        // One operating system. This branch is not reachable on this host, and it exists so that a
        // build without sockets fails to run the case rather than reporting a pass it did not earn.
        CY_TEST_FAIL("this build has no socket implementation; the case is unverified here");
        return;
    }

    UdpTransport server(allocator());
    UdpTransport client(allocator());
    CY_REQUIRE(server.open(0).has_value());
    CY_REQUIRE(client.open(0).has_value());
    CY_CHECK_GT(server.bound_port(), cy::u16{0});
    CY_CHECK_NE(server.bound_port(), client.bound_port());

    char address[32] = {};
    std::snprintf(address, sizeof(address), "127.0.0.1:%u",
                  static_cast<unsigned>(server.bound_port()));
    const auto peer = client.connect(address);
    CY_REQUIRE(peer.has_value());
    CY_CHECK(client.state(peer.value()) == ConnectionState::Connected);

    UdpAddress client_address;
    client_address.ipv4 = 0x7F00'0001U;
    client_address.port = client.bound_port();
    const auto inbound = server.accept(client_address);
    CY_REQUIRE(inbound.has_value());

    for (u32 index = 0; index < 8; ++index) {
        CY_REQUIRE(client
                       .send(peer.value(), 0, DeliveryMode::ReliableOrdered,
                             cy::Span<const u8>(kMessage, 12))
                       .has_value());
        CY_REQUIRE(
            client.send(peer.value(), 1, DeliveryMode::Unreliable, cy::Span<const u8>(kMessage, 12))
                .has_value());
    }

    u32 reliable = 0;
    u32 unreliable = 0;
    for (u64 now = 0; now <= 400 && reliable < 8; now += 10) {
        client.advance(now);
        server.advance(now);
        Datagram datagram;
        while (server.receive(datagram)) {
            CY_REQUIRE_EQ(datagram.bytes.size(), cy::usize{12});
            CY_CHECK_EQ(datagram.bytes[0], kMessage[0]);
            if (datagram.channel == 0) {
                ++reliable;
            } else {
                ++unreliable;
            }
        }
        Datagram back;
        while (client.receive(back)) {
        }
    }

    // Reliable arrives in full; unreliable arrives on the loopback, where nothing is lost. The two
    // are counted separately so that a run in which only one path worked is a failure rather than a
    // total that happens to add up.
    CY_CHECK_EQ(reliable, 8U);
    CY_CHECK_EQ(unreliable, 8U);
    CY_CHECK_GT(server.datagrams_received(), u64{0});
    CY_CHECK_GT(client.datagrams_sent(), u64{0});

    server.close();
    client.close();
    CY_CHECK_FALSE(server.is_open());
}

CY_TEST_CASE("networking: a datagram from an address no link names is counted and dropped") {
    if (!udp_available()) {
        CY_TEST_FAIL("this build has no socket implementation; the case is unverified here");
        return;
    }

    UdpTransport server(allocator());
    UdpTransport stranger(allocator());
    CY_REQUIRE(server.open(0).has_value());
    CY_REQUIRE(stranger.open(0).has_value());

    char address[32] = {};
    std::snprintf(address, sizeof(address), "127.0.0.1:%u",
                  static_cast<unsigned>(server.bound_port()));
    const auto peer = stranger.connect(address);
    CY_REQUIRE(peer.has_value());
    // The server never accepts this address, so it has no link for it.
    CY_REQUIRE(
        stranger.send(peer.value(), 0, DeliveryMode::Unreliable, cy::Span<const u8>(kMessage, 12))
            .has_value());

    for (u64 now = 0; now <= 100; now += 10) {
        stranger.advance(now);
        server.advance(now);
        Datagram datagram;
        CY_CHECK_FALSE(server.receive(datagram));
    }
    // Counted rather than answered: answering an unsolicited datagram is the cheapest amplification
    // there is.
    CY_CHECK_GT(server.datagrams_from_strangers(), u64{0});
    CY_CHECK_EQ(server.link_count(), 0U);
}

CY_TEST_CASE("networking: a transport that is not bound refuses rather than pretending") {
    UdpTransport transport(allocator());
    CY_CHECK_FALSE(transport.is_open());
    CY_CHECK_FALSE(transport.connect("not an address").has_value());

    if (!udp_available()) {
        CY_CHECK_FALSE(transport.open(0).has_value());
        return;
    }
    const auto peer = transport.connect("127.0.0.1:9");
    CY_REQUIRE(peer.has_value());
    // A link exists; the socket does not. The send fails with a reason rather than silently
    // succeeding into nothing.
    CY_CHECK_FALSE(
        transport.send(peer.value(), 0, DeliveryMode::Unreliable, cy::Span<const u8>(kMessage, 12))
            .has_value());

    CY_REQUIRE(transport.open(0).has_value());
    CY_CHECK_FALSE(transport.open(0).has_value());  // bound twice
}
