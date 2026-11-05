// SPDX-License-Identifier: MIT
#pragma once
// A dedicated server and clients, each with its own world, over the secure transport on the seeded
// simulated network. Issue #82.

#include "../tools/net_demo.h"
#include "secure_fixture.h"

#include <cy/networking/session.h>

#include <cstdlib>
#include <memory>

namespace cy::net_test {

inline constexpr cy::u64 kBuildId = 0xB111'D000'0082ULL;

/// One client: a world, a replica of the server's, and a host driving both.
struct HostedClient {
    cy::ecs::World world;
    cy::net::ReplicaWorld replica;
    cy::net::ClientHost host;

    HostedClient(cy::net::Transport& transport, const cy::net::SchemaSet& schemas,
                 const cy::net::CompatibilityScope& scope) noexcept
        : world(allocator()),
          replica(allocator(), world, schemas),
          host(allocator(), transport, replica, cy::net::ClientHostConfig{scope}) {
        fixture_requires(world.initialize().has_value());
        fixture_requires(cy::net::demo::register_client_components(world).has_value());
        fixture_requires(replica.initialize().has_value());
    }

    static void fixture_requires(bool condition) noexcept {
        if (!condition) {
            std::abort();
        }
    }
};

struct HostedSession {
    SecureSession network;
    cy::net::demo::DemoSchemas schemas;
    cy::net::CompatibilityScope scope{};
    cy::ecs::World world;
    cy::net::WorldReplicator replicator;
    cy::net::demo::DemoServerGame game;
    std::unique_ptr<cy::net::ServerHost> server;
    std::unique_ptr<HostedClient> clients[4];
    cy::u32 client_count = 0;
    cy::u64 now_us = 1'000'000;

    HostedSession(cy::u64 seed, cy::u32 units, cy::net::ServerHostConfig config = {}) noexcept
        : network(seed),
          schemas(allocator()),
          world(allocator()),
          replicator(allocator(), world, schemas.set()),
          game(world, replicator) {
        HostedClient::fixture_requires(schemas.valid());
        HostedClient::fixture_requires(world.initialize().has_value());
        HostedClient::fixture_requires(game.initialize().has_value());
        HostedClient::fixture_requires(game.populate(units).has_value());
        scope.platform = "linux";
        scope.architecture = "x86_64";
        scope.build_id = kBuildId;
        scope.schema_set_hash = schemas.set().identity();
        config.scope = scope;
        config.simulate = &cy::net::demo::DemoServerGame::simulate;
        config.simulate_user = &game;
        config.on_peer = &cy::net::demo::DemoServerGame::on_peer;
        config.on_peer_user = &game;
        server = std::make_unique<cy::net::ServerHost>(allocator(), network.host(), world,
                                                       replicator, config);
    }

    /// A client with this session's scope, or with `scope` when one is given. Connects at once.
    cy::u32 add_client(const cy::net::CompatibilityScope* override_scope = nullptr) noexcept {
        const cy::u32 index = network.add_client();
        clients[index] =
            std::make_unique<HostedClient>(network.client(index), schemas.set(),
                                           override_scope == nullptr ? scope : *override_scope);
        HostedClient::fixture_requires(clients[index]->host.connect("server").has_value());
        ++client_count;
        return index;
    }

    [[nodiscard]] cy::net::ClientHost& client(cy::u32 index) noexcept {
        return clients[index]->host;
    }

    void step(cy::u32 milliseconds = 5) noexcept {
        now_us += static_cast<cy::u64>(milliseconds) * 1000;
        server->update(now_us);
        for (cy::u32 index = 0; index < client_count; ++index) {
            clients[index]->host.update(now_us);
        }
    }

    void run(cy::u32 milliseconds, cy::u32 step_ms = 5) noexcept {
        for (cy::u32 elapsed = 0; elapsed < milliseconds; elapsed += step_ms) {
            step(step_ms);
        }
    }

    [[nodiscard]] bool all_joined() const noexcept {
        for (cy::u32 index = 0; index < client_count; ++index) {
            if (clients[index]->host.phase() != cy::net::ClientPhase::Joined) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] cy::u64 server_digest() const noexcept {
        return cy::net::replicated_digest(world, schemas.set());
    }
    [[nodiscard]] cy::u64 client_digest(cy::u32 index) const noexcept {
        return cy::net::replicated_digest(clients[index]->world, schemas.set());
    }
};

}  // namespace cy::net_test
