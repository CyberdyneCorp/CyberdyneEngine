// SPDX-License-Identifier: MIT
#include "net_sim_session.h"

#include <algorithm>
#include <chrono>

namespace cy::net::demo {
namespace {

[[nodiscard]] SecureConfig client_config(const crypto::Key& server_public,
                                         crypto::SeededEntropy& entropy) noexcept {
    SecureConfig out;
    out.server_public = server_public;
    out.entropy = entropy.source();
    return out;
}

[[nodiscard]] SecureConfig server_config(const crypto::KeyPair& keys,
                                         crypto::SeededEntropy& entropy) noexcept {
    SecureConfig out;
    out.accept_connections = true;
    out.server_secret = keys.secret;
    out.entropy = entropy.source();
    return out;
}

[[nodiscard]] u64 wall_microseconds() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count() /
                            1000);
}

constexpr const char* kClientNames[SimulatedSession::kMaxClients] = {"client-0", "client-1",
                                                                     "client-2", "client-3"};

}  // namespace

SimulatedClient::SimulatedClient(Allocator& allocator, LocalNetwork& network, PeerId host_id,
                                 u64 seed, const crypto::Key& server_public,
                                 const SchemaSet& schemas, const CompatibilityScope& scope) noexcept
    : entropy(seed),
      socket(network, host_id),
      transport(allocator, socket, client_config(server_public, entropy)),
      world(allocator),
      replica(allocator, world, schemas),
      host(allocator, transport, replica, ClientHostConfig{scope}) {}

SimulatedSession::SimulatedSession(Allocator& allocator,
                                   const SimulatedSessionConfig& config) noexcept
    : allocator_(&allocator),
      config_(config),
      network_(allocator, config.seed),
      server_entropy_(config.seed ^ 0x5E55'1011ULL),
      keys_(crypto::generate_key_pair(server_entropy_.source())),
      server_socket_(network_, network_.add_host("server").value()),
      server_transport_(allocator, server_socket_, server_config(keys_, server_entropy_)),
      schemas_(allocator),
      world_(allocator),
      replicator_(allocator, world_, schemas_.set()),
      game_(world_, replicator_) {
    network_.set_conditions(config_.conditions);
    config_.clients = std::min(config_.clients, kMaxClients);
}

bool SimulatedSession::start() noexcept {
    if (!schemas_.valid() || !world_.initialize() || !game_.initialize() ||
        !game_.populate(config_.units)) {
        return false;
    }
    ServerHostConfig host;
    host.scope.build_id = 1;
    host.scope.schema_set_hash = schemas_.set().identity();
    host.tick_rate_hz = config_.tick_rate_hz;
    host.budget.bytes_per_tick = config_.budget_bytes_per_tick;
    host.simulate = &DemoServerGame::simulate;
    host.simulate_user = &game_;
    host.on_peer = &DemoServerGame::on_peer;
    host.on_peer_user = &game_;
    server_ =
        std::make_unique<ServerHost>(*allocator_, server_transport_, world_, replicator_, host);
    for (u32 index = 0; index < config_.clients; ++index) {
        Expected<PeerId, Error> host_id = network_.add_host(kClientNames[index]);
        if (!host_id) {
            return false;
        }
        clients_[index] =
            std::make_unique<SimulatedClient>(*allocator_, network_, *host_id, 900 + index,
                                              keys_.public_key, schemas_.set(), host.scope);
        SimulatedClient& client = *clients_[index];
        if (!client.world.initialize() || !register_client_components(client.world) ||
            !client.replica.initialize() || !client.host.connect("server")) {
            return false;
        }
    }
    return true;
}

void SimulatedSession::step(u32 milliseconds) noexcept {
    now_us_ += static_cast<u64>(milliseconds) * 1000;
    const u64 tick_before = server_->tick();
    const u64 started = wall_microseconds();
    server_->update(now_us_);
    last_server_us_ = wall_microseconds() - started;
    last_ticked_ = server_->tick() != tick_before;
    for (u32 index = 0; index < config_.clients; ++index) {
        clients_[index]->host.update(now_us_);
    }
}

bool SimulatedSession::all_joined() const noexcept {
    for (u32 index = 0; index < config_.clients; ++index) {
        if (clients_[index]->host.phase() != ClientPhase::Joined) {
            return false;
        }
    }
    return true;
}

}  // namespace cy::net::demo
