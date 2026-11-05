// SPDX-License-Identifier: MIT
#pragma once
// The demo game as a whole session — a dedicated server and its clients — on the seeded simulated
// network. Issue #82. Shared by `cy_net_session_bench` and `cy_net_session_trace`, so the numbers
// and the video come from the same arrangement.

#include "net_demo.h"

#include <cy/networking/datagram_socket.h>
#include <cy/networking/local_transport.h>
#include <cy/networking/secure_transport.h>

#include <memory>

namespace cy::net::demo {

struct SimulatedClient {
    crypto::SeededEntropy entropy;
    SimulatedSocket socket;
    SecureTransport transport;
    ecs::World world;
    ReplicaWorld replica;
    ClientHost host;

    SimulatedClient(Allocator& allocator, LocalNetwork& network, PeerId host_id, u64 seed,
                    const crypto::Key& server_public, const SchemaSet& schemas,
                    const CompatibilityScope& scope) noexcept;
};

struct SimulatedSessionConfig {
    u32 units = 500;
    u32 clients = 2;
    u32 tick_rate_hz = 30;
    u32 budget_bytes_per_tick = 4096;
    NetworkConditions conditions{};
    u64 seed = 1;
};

/// The server's transport, world and host, and `clients` clients, all on one `LocalNetwork`. The
/// clock is the caller's: `step()` advances it and updates every host once.
class SimulatedSession {
public:
    static constexpr u32 kMaxClients = 4;

    SimulatedSession(Allocator& allocator, const SimulatedSessionConfig& config) noexcept;

    /// Build the world and connect the clients. False when anything refused.
    [[nodiscard]] bool start() noexcept;

    /// Advance the clock by `milliseconds` and update the server, then every client.
    void step(u32 milliseconds) noexcept;

    [[nodiscard]] u64 now_us() const noexcept { return now_us_; }
    [[nodiscard]] ServerHost& server() noexcept { return *server_; }
    [[nodiscard]] SecureTransport& server_transport() noexcept { return server_transport_; }
    [[nodiscard]] DemoServerGame& game() noexcept { return game_; }
    [[nodiscard]] ecs::World& world() noexcept { return world_; }
    [[nodiscard]] const SchemaSet& schemas() const noexcept { return schemas_.set(); }
    [[nodiscard]] SimulatedClient& client(u32 index) noexcept { return *clients_[index]; }
    [[nodiscard]] u32 client_count() const noexcept { return config_.clients; }
    [[nodiscard]] bool all_joined() const noexcept;
    /// Microseconds the last `step()` spent in the server's update, and whether it ran a tick.
    [[nodiscard]] u64 last_server_us() const noexcept { return last_server_us_; }
    [[nodiscard]] bool last_step_ticked() const noexcept { return last_ticked_; }

private:
    Allocator* allocator_;
    SimulatedSessionConfig config_;
    LocalNetwork network_;
    crypto::SeededEntropy server_entropy_;
    crypto::KeyPair keys_;
    SimulatedSocket server_socket_;
    SecureTransport server_transport_;
    DemoSchemas schemas_;
    ecs::World world_;
    WorldReplicator replicator_;
    DemoServerGame game_;
    std::unique_ptr<ServerHost> server_;
    std::unique_ptr<SimulatedClient> clients_[kMaxClients];
    u64 now_us_ = 1'000'000;
    u64 last_server_us_ = 0;
    bool last_ticked_ = false;
};

}  // namespace cy::net::demo
