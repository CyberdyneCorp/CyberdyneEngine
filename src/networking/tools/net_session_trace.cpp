// SPDX-License-Identifier: MIT
// cy_net_session_trace — where every unit is, on the server and on each client, over time. Issue
// #82.
//
//   cy_net_session_trace <out.csv> [units]
//
// The bench's session — a dedicated server and two clients over the secure transport, 200 ms round
// trip, 20 ms jitter, 5 % loss, 5 % reordering — sampled every 33 ms: six seconds of a wandering
// world, then three seconds at rest. Each row is `time_ms,phase,view,id,x,y`, view 0 being the
// server and 1 and 2 the clients. `session_video.py` draws the three views side by side from it, so
// the video shows what the engine replicated and nothing a script made up.

#include "net_sim_session.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/query.h>

#include <cstdio>
#include <cstdlib>

namespace {

using namespace cy;
using namespace cy::net;

constexpr u32 kStepMs = 5;
constexpr u32 kSampleMs = 33;

void sample(std::FILE* out, u64 time_ms, u32 phase, u32 view, ecs::World& world) noexcept {
    const ecs::ComponentInfo* identity = world.components().find(kNetworkIdentityComponent);
    const ecs::ComponentInfo* unit = world.components().find(demo::unit_type().id);
    if (identity == nullptr || unit == nullptr) {
        return;
    }
    ecs::QueryDesc desc(world.allocator());
    if (!desc.read(identity->id) || !desc.read(unit->id)) {
        return;
    }
    ecs::Query query(world, std::move(desc));
    (void)query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        const Span<const NetworkIdentity> ids = chunk.read<NetworkIdentity>(identity->id);
        const Span<const demo::Unit> units = chunk.read<demo::Unit>(unit->id);
        for (u32 row = 0; row < chunk.count(); ++row) {
            std::fprintf(out, "%llu,%u,%u,%llu,%.3f,%.3f\n",
                         static_cast<unsigned long long>(time_ms), phase, view,
                         static_cast<unsigned long long>(ids[row].id & 0xFFFF'FFFFULL),
                         static_cast<double>(units[row].position[0]),
                         static_cast<double>(units[row].position[1]));
        }
    });
}

void record(std::FILE* out, demo::SimulatedSession& session, u64 start_ms, u32 phase,
            u32 milliseconds) noexcept {
    u32 since = kSampleMs;
    for (u32 elapsed = 0; elapsed < milliseconds; elapsed += kStepMs) {
        session.step(kStepMs);
        since += kStepMs;
        if (since < kSampleMs) {
            continue;
        }
        since = 0;
        const u64 time_ms = (session.now_us() / 1000) - start_ms;
        sample(out, time_ms, phase, 0, session.world());
        for (u32 index = 0; index < session.client_count(); ++index) {
            sample(out, time_ms, phase, index + 1, session.client(index).world);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cy_net_session_trace <out.csv> [units]\n");
        return 2;
    }
    std::FILE* out = std::fopen(argv[1], "w");
    if (out == nullptr) {
        std::fprintf(stderr, "cy_net_session_trace: cannot write %s\n", argv[1]);
        return 1;
    }
    demo::SimulatedSessionConfig config;
    config.units = argc > 2 ? static_cast<u32>(std::strtoul(argv[2], nullptr, 10)) : 400;
    config.seed = 82;
    config.conditions.latency_ms = 100;
    config.conditions.jitter_ms = 20;
    config.conditions.loss_percent = 5;
    config.conditions.reorder_percent = 5;
    demo::SimulatedSession session(system_allocator(MemoryDomain::World), config);
    if (!session.start()) {
        std::fclose(out);
        return 1;
    }
    for (u32 elapsed = 0; elapsed < 3000; elapsed += kStepMs) {
        session.step(kStepMs);
    }
    std::fprintf(out, "time_ms,phase,view,id,x,y\n");
    const u64 start_ms = session.now_us() / 1000;
    record(out, session, start_ms, 0, 6000);
    session.game().set_wandering(false);
    record(out, session, start_ms, 1, 3000);
    std::fclose(out);
    const u64 digest = replicated_digest(session.world(), session.schemas());
    const bool same = replicated_digest(session.client(0).world, session.schemas()) == digest &&
                      replicated_digest(session.client(1).world, session.schemas()) == digest;
    std::printf("cy_net_session_trace: %u units, the clients %s the server at the end\n",
                config.units, same ? "match" : "DO NOT match");
    return same ? 0 : 1;
}
