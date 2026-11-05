// SPDX-License-Identifier: MIT
// ISSUE #82 — A DEDICATED SERVER AND ITS CLIENTS, END TO END.
//
// Every case runs `ServerHost` and `ClientHost` over `SecureTransport` over the seeded simulated
// network: the handshake, the encryption, fragmentation, the join, replication from the server's
// world into each client's, acknowledgements and the command stream. The clock is the test's, so a
// 200 ms round trip costs no wall time and a failure replays exactly.

#include "session_fixture.h"

#include <cstdio>
#include <cstdlib>

using namespace cy::net_test;
using cy::u32;
using cy::u64;

namespace {

[[nodiscard]] cy::net::NetworkConditions bad_network() noexcept {
    cy::net::NetworkConditions conditions;
    conditions.latency_ms = 100;  // 200 ms round trip
    conditions.jitter_ms = 20;
    conditions.loss_percent = 5;
    conditions.reorder_percent = 5;
    return conditions;
}

/// Every joined client steers its avatar a little each update.
void steer(HostedSession& session, u64 round) noexcept {
    for (u32 index = 0; index < session.client_count; ++index) {
        cy::net::ClientHost& client = session.client(index);
        if (client.phase() != cy::net::ClientPhase::Joined) {
            continue;
        }
        const cy::net::NetworkId avatar =
            cy::net::demo::avatar_of(session.clients[index]->world, client.self());
        if (!avatar.valid()) {
            continue;
        }
        const auto dx = static_cast<cy::i16>((round + index) % 3 == 0 ? 2 : -1);
        (void)client.submit(cy::net::demo::move(avatar, client.server_tick() + 1, dx, 1));
    }
}

}  // namespace

CY_TEST_CASE("session: at 200 ms, 5 per cent loss and reordering, two clients converge") {
    HostedSession session(21, 300);
    session.network.network.set_conditions(bad_network());
    session.add_client();
    session.add_client();
    for (u32 elapsed = 0; elapsed < 3000 && !session.all_joined(); elapsed += 5) {
        session.step();
    }
    CY_REQUIRE(session.all_joined());
    CY_CHECK_EQ(session.server->peer_count(), 2U);
    CY_CHECK_EQ(cy::net::admissible_for(session.network.host().security(),
                                        cy::net::Deployment::Development),
                cy::net::SecurityRefusal::None);

    // Four seconds of a moving world and steering players, then the world comes to rest.
    for (u64 round = 0; round < 800; ++round) {
        session.step();
        if (round % 6 == 0) {
            steer(session, round);
        }
    }
    CY_CHECK_NE(session.client_digest(0), session.server_digest());  // still in flight
    session.game.set_wandering(false);
    session.run(3000);

    const u64 server = session.server_digest();
    CY_CHECK_EQ(session.client_digest(0), server);
    CY_CHECK_EQ(session.client_digest(1), server);
    CY_CHECK_EQ(session.clients[0]->replica.entity_count(), 302U);  // 300 units and two avatars
    const cy::net::ServerPeerStats* stats = session.server->peer_stats(session.client(0).self());
    CY_REQUIRE(stats != nullptr);
    CY_CHECK_GT(stats->commands_accepted, 100U);
    CY_CHECK_EQ(stats->commands_refused, 0U);
    CY_TEST_MESSAGE("client 0: " << session.client(0).snapshots_applied() << " snapshots, "
                                 << session.client(0).snapshot_bytes() << " bytes; commands "
                                 << stats->commands_accepted);
}

CY_TEST_CASE("session: the server is deterministic under the same command input") {
    // Run the networked session twice from the same seeds, then replay the first run's input log
    // into a server with no network at all. All three agree tick for tick.
    constexpr u32 kTicks = 150;
    struct Run {
        u64 log_hash = 0;
        u64 digest[kTicks + 1] = {};
        u64 last_tick = 0;
    };
    auto networked = [](Run& run, cy::Array<cy::net::AppliedCommand>* keep) noexcept {
        HostedSession session(33, 120);
        session.network.network.set_conditions(bad_network());
        session.add_client();
        session.add_client();
        u64 round = 0;
        while (session.server->tick() < kTicks) {
            const u64 before = session.server->tick();
            session.step();
            if (++round % 4 == 0) {
                steer(session, round);
            }
            for (u64 tick = before + 1; tick <= session.server->tick() && tick <= kTicks; ++tick) {
                run.digest[tick] = session.server_digest();
            }
        }
        run.last_tick = session.server->tick();
        run.log_hash = session.server->input_log().hash();
        if (keep != nullptr) {
            for (const cy::net::AppliedCommand& entry : session.server->input_log().entries()) {
                (void)keep->push_back(entry);
            }
        }
    };
    Run first;
    Run second;
    cy::Array<cy::net::AppliedCommand> log(allocator());
    networked(first, &log);
    networked(second, nullptr);
    CY_CHECK_EQ(first.log_hash, second.log_hash);
    u32 commands = 0;
    for (const cy::net::AppliedCommand& entry : log) {
        commands += entry.kind == cy::net::AppliedCommand::Kind::Command ? 1U : 0U;
    }
    CY_TEST_MESSAGE("the log holds " << log.size() << " entries, " << commands
                                     << " of them commands");
    CY_CHECK_GT(commands, 20U);

    // The replay: a fresh world, the same game, no transport traffic, the log fed tick by tick.
    cy::net::demo::DemoSchemas schemas(allocator());
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    cy::net::WorldReplicator replicator(allocator(), world, schemas.set());
    cy::net::demo::DemoServerGame game(world, replicator);
    CY_REQUIRE(game.initialize().has_value());
    CY_REQUIRE(game.populate(120).has_value());
    cy::net::LocalNetwork silent(allocator(), 1);
    cy::net::SimulatedSocket socket(silent, silent.add_host("nobody").value());
    cy::net::crypto::SeededEntropy entropy(1);
    cy::net::SecureConfig config;
    config.entropy = entropy.source();
    cy::net::SecureTransport transport(allocator(), socket, config);
    cy::net::ServerHostConfig host_config;
    host_config.simulate = &cy::net::demo::DemoServerGame::simulate;
    host_config.simulate_user = &game;
    host_config.on_peer = &cy::net::demo::DemoServerGame::on_peer;
    host_config.on_peer_user = &game;
    cy::net::ServerHost replay(allocator(), transport, world, replicator, host_config);
    u32 cursor = 0;
    u32 mismatches = 0;
    for (u64 tick = 1; tick <= kTicks; ++tick) {
        const u32 begin = cursor;
        while (cursor < log.size() && log[cursor].tick == tick) {
            ++cursor;
        }
        replay.replay_step(
            cy::Span<const cy::net::AppliedCommand>(log.data() + begin, cursor - begin));
        const u64 digest = cy::net::replicated_digest(world, schemas.set());
        mismatches += digest == first.digest[tick] ? 0U : 1U;
        mismatches += first.digest[tick] == second.digest[tick] ? 0U : 1U;
    }
    CY_CHECK_EQ(mismatches, 0U);
    CY_CHECK_EQ(replay.input_log().hash(), [&]() noexcept {
        cy::net::ServerInputLog prefix(allocator());
        for (u32 index = 0; index < cursor; ++index) {
            (void)prefix.append(log[index]);
        }
        return prefix.hash();
    }());
}

CY_TEST_CASE("session: a client with another schema set is refused, by name, before any state") {
    HostedSession session(41, 50);
    session.add_client();
    cy::net::CompatibilityScope other = session.scope;
    other.schema_set_hash ^= 0x1ULL;
    session.add_client(&other);
    session.run(1500);
    CY_CHECK_EQ(session.client(0).phase(), cy::net::ClientPhase::Joined);
    CY_CHECK_EQ(session.client(1).phase(), cy::net::ClientPhase::Refused);
    CY_CHECK_EQ(session.client(1).refusal().refusal, cy::net::AdmissionRefusal::Incompatible);
    CY_CHECK_EQ(session.client(1).refusal().compatibility, cy::net::JoinRefusal::SchemaSetMismatch);
    CY_CHECK_EQ(session.clients[1]->replica.entity_count(), 0U);
    CY_CHECK_EQ(session.client(1).snapshots_applied(), 0U);
    CY_CHECK_EQ(session.server->peer_count(), 1U);
    CY_CHECK_EQ(session.server->refused_joins(), 1U);
}

CY_TEST_CASE(
    "session: a client sending commands faster than the tick rate is clamped and flagged") {
    cy::net::ServerHostConfig config;
    config.max_commands_per_tick = 4;
    HostedSession session(51, 10, config);
    session.add_client();
    session.run(1000);
    CY_REQUIRE(session.all_joined());
    session.run(300);
    const cy::net::NetworkId avatar =
        cy::net::demo::avatar_of(session.clients[0]->world, session.client(0).self());
    CY_REQUIRE(avatar.valid());
    for (u32 round = 0; round < 40; ++round) {
        for (u32 burst = 0; burst < 30; ++burst) {
            (void)session.client(0).submit(
                cy::net::demo::move(avatar, session.client(0).server_tick() + 1, 1, 0));
        }
        session.step(10);
    }
    session.run(500);
    const cy::net::ServerPeerStats* stats = session.server->peer_stats(session.client(0).self());
    CY_REQUIRE(stats != nullptr);
    CY_TEST_MESSAGE("accepted " << stats->commands_accepted << ", refused "
                                << stats->commands_refused);
    CY_CHECK(stats->flagged);
    CY_CHECK_GT(stats->commands_refused, 0U);
    // No tick was issued more than the limit: the server's own record says so.
    u64 tick = 0;
    u32 in_tick = 0;
    u32 worst = 0;
    for (const cy::net::AppliedCommand& entry : session.server->input_log().entries()) {
        if (entry.kind != cy::net::AppliedCommand::Kind::Command) {
            continue;
        }
        in_tick = entry.command.tick == tick ? in_tick + 1 : 1;
        tick = entry.command.tick;
        worst = in_tick > worst ? in_tick : worst;
    }
    CY_CHECK_LE(worst, config.max_commands_per_tick);
}

CY_TEST_CASE("session: what each client receives stays under its budget with N entities") {
    cy::net::ServerHostConfig config;
    config.tick_rate_hz = 30;
    config.budget.bytes_per_tick = 1200;
    HostedSession session(61, 600, config);
    session.network.network.set_conditions(bad_network());
    session.add_client();
    session.add_client();
    session.run(2000);
    CY_REQUIRE(session.all_joined());
    session.run(1000);

    const cy::net::PeerId peer = session.client(0).self();
    const cy::net::PeerId server_side = peer;  // the server's PeerId for the client
    const u64 bytes_before = session.network.host().stats(server_side).bytes_out;
    const u64 start = session.now_us;
    session.run(5000);
    const u64 elapsed_us = session.now_us - start;
    const u64 sent = session.network.host().stats(server_side).bytes_out - bytes_before;
    const u64 per_second = (sent * 1'000'000ULL) / elapsed_us;
    // The application's budget per tick, at the tick rate, plus the framing a snapshot of that size
    // needs on the wire (two fragments: 2 x 49 bytes) and the connection's own pings and acks.
    const u64 allowed =
        ((u64{config.budget.bytes_per_tick} + 1 + (2 * u64{49})) * config.tick_rate_hz) + 2048;
    CY_TEST_MESSAGE("600 entities, budget " << config.budget.bytes_per_tick << " B/tick at "
                                            << config.tick_rate_hz << " Hz: " << per_second
                                            << " B/s on the wire to one client (allowed " << allowed
                                            << ")");
    CY_CHECK_LE(per_second, allowed);
    CY_CHECK_GT(per_second, allowed / 3);
}
