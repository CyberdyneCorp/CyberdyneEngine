// SPDX-License-Identifier: MIT
// THE LOCKSTEP RTS SCENARIO ON ONE HOST. openspec/changes/add-deterministic-math, tasks 6.1, 7.2.
//
// Two peers — two sessions built separately in one process — driven by one command log agree on
// every tick's state hash; a peer that misses one command does not; the mover on job workers
// computes the serial mover's bits; and the scenario's digest is the committed one, so a leg whose
// arithmetic moved fails on its own before the cross-leg comparator sees it.

#include <cy/core/jobs/job_system.h>

#include <cstdio>
#include <memory>

#include "movement_fixture.h"
#include "rts_scenario.h"

namespace {

using cy::u32;
using cy::u64;
using cy::movement_test::RtsConfig;
using cy::movement_test::RtsDigest;
using cy::movement_test::RtsSession;

/// A shorter run of the lockstep configuration, inside the integration tier's second.
[[nodiscard]] RtsConfig short_config() noexcept {
    RtsConfig config = cy::movement_test::lockstep_config();
    config.ticks = 150;
    config.order_every = 50;
    return config;
}

/// The committed digests of the two published scenarios, measured on linux-x86_64 and compared
/// across the four CI legs by `determinism.cross_leg` and tools/ci/cross_leg_digests.py.
constexpr u64 kShortDigest = 0x817f'0ffa'3fb6'86f0ULL;

}  // namespace

CY_TEST_CASE("movement lockstep: two peers driven by one command log agree on every tick") {
    const RtsConfig config = short_config();
    auto issuer = std::make_unique<RtsSession>(config);
    auto follower = std::make_unique<RtsSession>(config);
    CY_REQUIRE(issuer->setup());
    CY_REQUIRE(follower->setup());
    CY_CHECK_EQ(issuer->world_hash(), follower->world_hash());

    u32 cursor = 0;
    u32 disagreements = 0;
    for (u64 tick = 0; tick < config.ticks; ++tick) {
        CY_REQUIRE(issuer->issue(tick));
        CY_REQUIRE(issuer->advance(tick));
        CY_REQUIRE(follower->receive(issuer->log(), cursor, tick));
        CY_REQUIRE(follower->advance(tick));
        disagreements += issuer->state_hash() == follower->state_hash() ? 0U : 1U;
    }
    CY_CHECK_EQ(disagreements, 0U);
    CY_CHECK_EQ(cursor, issuer->log().size());
    CY_CHECK_EQ(issuer->log().hash(), follower->log().hash());

    // The run did something: orders were given, paths planned to them, and units moved.
    CY_CHECK_EQ(issuer->log().size(), 12U);
    CY_CHECK_EQ(issuer->paths_planned(), 3U * 256U);
    CY_CHECK_GT(issuer->paths_found(), issuer->paths_planned() / 2);
}

CY_TEST_CASE("movement lockstep: a peer that misses one command diverges, and the hash shows it") {
    // The negative control: the comparison above can fail. Drop the last command of the first
    // order round on the follower's side, and the two sessions stop agreeing at that tick.
    const RtsConfig config = short_config();
    auto issuer = std::make_unique<RtsSession>(config);
    auto follower = std::make_unique<RtsSession>(config);
    CY_REQUIRE(issuer->setup());
    CY_REQUIRE(follower->setup());
    u32 first_disagreement = config.ticks;
    for (u64 tick = 0; tick < config.ticks; ++tick) {
        CY_REQUIRE(issuer->issue(tick));
        CY_REQUIRE(issuer->advance(tick));
        // Everything the issuer committed this tick except, at tick 0, its last command.
        cy::gameplay::CommandLog partial(cy::system_allocator(cy::MemoryDomain::World));
        for (u32 index = 0; index < issuer->log().size(); ++index) {
            const bool dropped = tick == 0 && index + 1 == issuer->log().size();
            if (issuer->log().at(index).tick == tick && !dropped) {
                CY_REQUIRE(partial.append(issuer->log().at(index)).has_value());
            }
        }
        u32 cursor = 0;
        CY_REQUIRE(follower->receive(partial, cursor, tick));
        CY_REQUIRE(follower->advance(tick));
        if (first_disagreement == config.ticks && issuer->state_hash() != follower->state_hash()) {
            first_disagreement = static_cast<u32>(tick);
        }
    }
    CY_CHECK_EQ(first_disagreement, 0U);
}

CY_TEST_CASE("movement lockstep: the mover on job workers computes the serial mover's bits") {
    RtsConfig config = cy::movement_test::movement_config();
    config.ticks = 30;
    const RtsDigest serial = cy::movement_test::run_rts(config);
    CY_REQUIRE(serial.complete);

    cy::jobs::JobSystem jobs;
    cy::jobs::JobSystemConfig workers;
    workers.worker_count = 4;
    CY_REQUIRE(jobs.start(workers).has_value());
    const RtsDigest parallel = cy::movement_test::run_rts(config, &jobs);
    jobs.shutdown();
    CY_REQUIRE(parallel.complete);
    CY_CHECK_EQ(parallel.digest, serial.digest);
    CY_CHECK_EQ(parallel.final_hash, serial.final_hash);
    CY_CHECK_EQ(serial.units, 2000U);
}

CY_TEST_CASE("movement lockstep: the scenario digest is the committed one, on this leg alone") {
    const RtsDigest run = cy::movement_test::run_rts(short_config());
    CY_REQUIRE(run.complete);
    std::printf("movement lockstep: short digest %016llx\n",
                static_cast<unsigned long long>(run.digest));
    CY_CHECK_EQ(run.digest, kShortDigest);
}
