// SPDX-License-Identifier: MIT
// cy_net_session_bench — bytes per second per client and server CPU per tick, against entity count.
// Issue #82.
//
// A dedicated server and two clients over the secure transport on the simulated network at a
// 200 ms round trip with 20 ms jitter, 5 % loss and 5 % reordering — the conditions
// `tests/test_session.cpp` converges under — run once per population. For each it prints one CSV
// row:
//
//   entities             the units in the world (plus one avatar per client)
//   wire_bytes_per_s     what the server's transport put on the wire to ONE client, per second,
//                        headers, tags, fragments, pings and retransmissions included
//   snapshot_bytes       the mean snapshot message the server sent that client, per tick
//   budget               the replication budget per tick, which `snapshot_bytes` may not exceed
//   server_us_mean/p95   wall time of one server update that ran a tick: transport, simulation,
//                        gather, and one snapshot build per client
//   converged_ms         after the world comes to rest, how long until both clients' replicated
//                        digests equal the server's
//
// Every unit moves every tick, which is the worst case for bandwidth: the budget is always full and
// the scheduler decides what waits. Exits non-zero if a population does not converge within ten
// seconds, or if any client's wire rate exceeds what its budget allows.

#include "net_sim_session.h"

#include <cy/core/memory/system_allocator.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

using namespace cy;
using namespace cy::net;

constexpr u32 kTickRate = 30;
constexpr u32 kBudget = 4096;
constexpr u32 kStepMs = 5;
/// The framing a full snapshot needs on the wire: four fragments at 49 bytes each.
constexpr u64 kFragmentFraming = u64{4} * 49;

struct Row {
    u32 entities = 0;
    u64 wire_bytes_per_second = 0;
    u64 snapshot_bytes = 0;
    u64 server_us_mean = 0;
    u64 server_us_p95 = 0;
    i64 converged_ms = -1;
};

void run_for(demo::SimulatedSession& session, u32 milliseconds,
             std::vector<u64>* tick_costs) noexcept {
    for (u32 elapsed = 0; elapsed < milliseconds; elapsed += kStepMs) {
        session.step(kStepMs);
        if (tick_costs != nullptr && session.last_step_ticked()) {
            tick_costs->push_back(session.last_server_us());
        }
    }
}

[[nodiscard]] bool converged(demo::SimulatedSession& session) noexcept {
    const u64 digest = replicated_digest(session.world(), session.schemas());
    for (u32 index = 0; index < session.client_count(); ++index) {
        if (replicated_digest(session.client(index).world, session.schemas()) != digest) {
            return false;
        }
    }
    return true;
}

void summarise(std::vector<u64>& costs, Row& row) noexcept {
    if (costs.empty()) {
        return;
    }
    std::ranges::sort(costs);
    u64 total = 0;
    for (const u64 cost : costs) {
        total += cost;
    }
    row.server_us_mean = total / costs.size();
    row.server_us_p95 = costs[(costs.size() * 95) / 100];
}

[[nodiscard]] bool measure(Allocator& allocator, u32 units, Row& row) noexcept {
    demo::SimulatedSessionConfig config;
    config.units = units;
    config.tick_rate_hz = kTickRate;
    config.budget_bytes_per_tick = kBudget;
    config.seed = 0x5E55'1011ULL + units;
    config.conditions.latency_ms = 100;
    config.conditions.jitter_ms = 20;
    config.conditions.loss_percent = 5;
    config.conditions.reorder_percent = 5;
    demo::SimulatedSession session(allocator, config);
    if (!session.start()) {
        return false;
    }
    run_for(session, 3000, nullptr);
    const PeerId measured = session.client(0).host.self();
    const ServerPeerStats* stats = session.server().peer_stats(measured);
    if (!session.all_joined() || stats == nullptr) {
        return false;
    }
    const u64 bytes_before = session.server_transport().stats(measured).bytes_out;
    const u64 snapshots_before = stats->snapshots_sent;
    const u64 snapshot_bytes_before = stats->snapshot_bytes;
    const u64 window_start = session.now_us();
    std::vector<u64> costs;
    run_for(session, 6000, &costs);
    const u64 window_us = session.now_us() - window_start;
    row.entities = units;
    row.wire_bytes_per_second =
        ((session.server_transport().stats(measured).bytes_out - bytes_before) * 1'000'000ULL) /
        window_us;
    const u64 snapshots = stats->snapshots_sent - snapshots_before;
    row.snapshot_bytes =
        snapshots == 0 ? 0 : (stats->snapshot_bytes - snapshot_bytes_before) / snapshots;
    summarise(costs, row);

    session.game().set_wandering(false);
    const u64 rest_start = session.now_us();
    for (u32 elapsed = 0; elapsed < 10'000 && row.converged_ms < 0; elapsed += kStepMs) {
        session.step(kStepMs);
        if (converged(session)) {
            row.converged_ms = static_cast<i64>((session.now_us() - rest_start) / 1000);
        }
    }
    return true;
}

}  // namespace

int main() {
    Allocator& allocator = system_allocator(MemoryDomain::World);
    const u32 populations[] = {100, 250, 500, 1000, 2000, 4000};
    std::printf(
        "entities,wire_bytes_per_s,snapshot_bytes,budget,tick_rate,server_us_mean,server_us_p95,"
        "converged_ms\n");
    int status = 0;
    for (const u32 units : populations) {
        Row row;
        if (!measure(allocator, units, row)) {
            std::fprintf(
                stderr, "cy_net_session_bench: the session for %u entities did not start\n", units);
            return 1;
        }
        std::printf("%u,%llu,%llu,%u,%u,%llu,%llu,%lld\n", row.entities,
                    static_cast<unsigned long long>(row.wire_bytes_per_second),
                    static_cast<unsigned long long>(row.snapshot_bytes), kBudget, kTickRate,
                    static_cast<unsigned long long>(row.server_us_mean),
                    static_cast<unsigned long long>(row.server_us_p95),
                    static_cast<long long>(row.converged_ms));
        std::fflush(stdout);
        // The budget, its framing, and the connection's own traffic, as in the session test.
        const u64 allowed = ((u64{kBudget} + 1 + kFragmentFraming) * kTickRate) + 2048;
        if (row.converged_ms < 0 || row.wire_bytes_per_second > allowed ||
            row.snapshot_bytes > kBudget + 1) {
            status = 1;
        }
    }
    return status;
}
