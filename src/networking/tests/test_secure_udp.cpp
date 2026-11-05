// SPDX-License-Identifier: MIT
// ISSUE #82 — THE SECURE TRANSPORT ON A REAL SOCKET.
//
// The simulated network proves the protocol; this proves the socket. A dedicated server and a
// client on the loopback, each with its own world, keys from the operating system's generator — so
// the transport is admitted for a shipping deployment — and a real clock, because datagrams through
// the kernel take the time they take.

#include "session_fixture.h"

#include <cy/networking/udp_transport.h>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace cy::net_test;
using cy::u32;
using cy::u64;

namespace {

[[nodiscard]] u64 microseconds() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

}  // namespace

CY_TEST_CASE("secure transport over UDP: a shipping-grade session on the loopback converges") {
    if (!cy::net::udp_available()) {
        CY_TEST_FAIL("this build has no socket implementation; the case is unverified here");
        return;
    }
    cy::net::UdpSocket server_socket(allocator());
    cy::net::UdpSocket client_socket(allocator());
    CY_REQUIRE(server_socket.open(0).has_value());
    CY_REQUIRE(client_socket.open(0).has_value());

    cy::net::crypto::EntropySource entropy = cy::net::crypto::system_entropy();
    const cy::net::crypto::KeyPair keys = cy::net::crypto::generate_key_pair(entropy);
    cy::net::SecureConfig server_config;
    server_config.accept_connections = true;
    server_config.server_secret = keys.secret;
    cy::net::SecureTransport server_transport(allocator(), server_socket, server_config);
    cy::net::SecureConfig client_config;
    client_config.server_public = keys.public_key;
    cy::net::SecureTransport client_transport(allocator(), client_socket, client_config);
    CY_CHECK_EQ(cy::net::admissible_for(server_transport.security(), cy::net::Deployment::Shipping),
                cy::net::SecurityRefusal::None);
    CY_CHECK_EQ(cy::net::admissible_for(client_transport.security(), cy::net::Deployment::Shipping),
                cy::net::SecurityRefusal::None);

    cy::net::demo::DemoSchemas schemas(allocator());
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    cy::net::WorldReplicator replicator(allocator(), world, schemas.set());
    cy::net::demo::DemoServerGame game(world, replicator);
    CY_REQUIRE(game.initialize().has_value());
    CY_REQUIRE(game.populate(200).has_value());
    cy::net::ServerHostConfig host_config;
    host_config.scope.build_id = kBuildId;
    host_config.scope.schema_set_hash = schemas.set().identity();
    host_config.simulate = &cy::net::demo::DemoServerGame::simulate;
    host_config.simulate_user = &game;
    host_config.on_peer = &cy::net::demo::DemoServerGame::on_peer;
    host_config.on_peer_user = &game;
    cy::net::ServerHost server(allocator(), server_transport, world, replicator, host_config);
    HostedClient client(client_transport, schemas.set(), host_config.scope);

    char address[32] = {};
    std::snprintf(address, sizeof(address), "127.0.0.1:%u",
                  static_cast<unsigned>(server_socket.bound_port()));
    CY_REQUIRE(client.host.connect(address).has_value());

    const u64 start = microseconds();
    auto pump = [&](u64 for_us) noexcept {
        const u64 until = microseconds() + for_us;
        while (microseconds() < until) {
            const u64 now = microseconds();
            server.update(now);
            client.host.update(now);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    while (client.host.phase() != cy::net::ClientPhase::Joined &&
           microseconds() - start < 5'000'000) {
        pump(5'000);
    }
    CY_REQUIRE_EQ(client.host.phase(), cy::net::ClientPhase::Joined);
    pump(600'000);
    game.set_wandering(false);
    pump(800'000);

    CY_CHECK_EQ(client.replica.entity_count(), 201U);
    CY_CHECK_EQ(cy::net::replicated_digest(client.world, schemas.set()),
                cy::net::replicated_digest(world, schemas.set()));
    const cy::net::ConnectionStats stats = client_transport.stats(client.host.server());
    CY_TEST_MESSAGE("loopback round trip " << stats.round_trip_us << " us, "
                                           << client.host.snapshots_applied() << " snapshots");
    CY_CHECK_EQ(stats.packets_rejected_authentication, 0U);
    CY_CHECK_GT(client.host.snapshots_applied(), 10U);
}
