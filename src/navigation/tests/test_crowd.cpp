// Local avoidance and the crowd, including the milestone's own scale criterion. M8.b task 6.2.
//
// INTEGRATION. Two of these cases step eight thousand agents for sixty ticks, which is the exit
// criterion "8,000 agents and 100 concurrent effects hold their budgets" and is not a unit test by
// any reading. The cheap avoidance cases live here beside them so that one `ctest -R
// integration.navigation_crowd` is the whole of the crowd's evidence.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/crowd.h>
#include <cy/test/test.h>

#include <ctime>

#include <chrono>
#include <cmath>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] AvoidanceParams walker() noexcept {
    AvoidanceParams params;
    params.radius = 0.4F;
    params.max_speed = 2.0F;
    params.max_acceleration = 20.0F;
    params.neighbour_distance = 3.0F;
    params.max_neighbours = 6;
    params.time_horizon = 2.0F;
    return params;
}

/// This thread's CPU time, in milliseconds.
///
/// CPU TIME AND NOT WALL CLOCK, and that is the whole reason this helper exists. A budget asserted
/// on elapsed wall time measures whatever else the machine is running: on a host in the middle of a
/// fourteen-job build, one tick of this loop was descheduled long enough to fail a threshold with
/// six times the headroom, and a test that fails when the machine is busy is a test that gets
/// switched off. The test harness's own budget guard makes the same choice for the same reason —
/// see tests/harness/src/budget.cpp, whose `cpu_now_ns` this mirrors.
///
/// Per-thread rather than per-process: nothing here fans work across threads, and a process clock
/// would count every worker some other part of the process happened to start.
[[nodiscard]] f64 cpu_millis() noexcept {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0) {
        return (static_cast<f64>(now.tv_sec) * 1000.0) + (static_cast<f64>(now.tv_nsec) / 1e6);
    }
#endif
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<f64, std::milli>(since).count();
}

/// Sort ascending and answer the median. In this file rather than in a header because two suites
/// wanting it is not yet a reason for a shared one.
[[nodiscard]] f64 sort_and_median(Array<f64>& samples) noexcept {
    for (usize i = 0; i < samples.size(); ++i) {
        for (usize j = i + 1; j < samples.size(); ++j) {
            if (samples[j] < samples[i]) {
                const f64 swap = samples[i];
                samples[i] = samples[j];
                samples[j] = swap;
            }
        }
    }
    return samples[samples.size() / 2];
}

[[nodiscard]] f32 closest_approach(const Crowd& crowd, CrowdAgentId a, CrowdAgentId b) noexcept {
    const Vec3 offset = crowd.agent(a)->position - crowd.agent(b)->position;
    return std::sqrt((offset.x * offset.x) + (offset.z * offset.z));
}

}  // namespace

CY_TEST_CASE("an agent with no neighbours takes the velocity it asked for") {
    Crowd crowd(allocator(), 4.0F);
    const Expected<CrowdAgentId, Error> id = crowd.add(Vec3{}, walker());
    CY_REQUIRE(id.has_value());
    crowd.set_desired_velocity(*id, Vec3{2.0F, 0.0F, 0.0F});

    CrowdReport report;
    CY_REQUIRE(crowd.step(0.1F, report).has_value());
    CY_CHECK_EQ(report.agents, 1U);
    CY_CHECK_EQ(report.agents_adjusted, 0U);
    CY_CHECK_NEAR(crowd.agent(*id)->velocity.x, 2.0F, 0.01F);
    CY_CHECK_NEAR(crowd.agent(*id)->velocity.z, 0.0F, 0.01F);

    // `step()` writes velocity and NOTHING ELSE — positions are the controller's.
    CY_CHECK_NEAR(crowd.agent(*id)->position.x, 0.0F, 1e-6F);
    crowd.integrate(0.1F);
    CY_CHECK_NEAR(crowd.agent(*id)->position.x, 0.2F, 0.001F);
}

CY_TEST_CASE("two agents approaching head-on steer around each other and do not interpenetrate") {
    // `navigation`: "each SHALL receive an adjusted velocity steering around the other, with
    // reciprocity preventing oscillation".
    Crowd crowd(allocator(), 4.0F);
    const Expected<CrowdAgentId, Error> west = crowd.add(Vec3{-6.0F, 0.0F, 0.0F}, walker());
    const Expected<CrowdAgentId, Error> east = crowd.add(Vec3{6.0F, 0.0F, 0.0F}, walker());
    CY_REQUIRE(west.has_value());
    CY_REQUIRE(east.has_value());

    f32 nearest = math::kInfinity;
    u32 adjusted_ticks = 0;
    // Twelve metres apart at 2 m/s is six seconds of walking, and avoidance costs some of that, so
    // four hundred ticks is the run rather than the margin.
    for (u32 tick = 0; tick < 400; ++tick) {
        crowd.set_desired_velocity(*west, Vec3{2.0F, 0.0F, 0.0F});
        crowd.set_desired_velocity(*east, Vec3{-2.0F, 0.0F, 0.0F});
        CrowdReport report;
        CY_REQUIRE(crowd.step(1.0F / 60.0F, report).has_value());
        crowd.integrate(1.0F / 60.0F);
        adjusted_ticks += (report.agents_adjusted > 0) ? 1u : 0u;
        nearest = std::fmin(nearest, closest_approach(crowd, *west, *east));
    }

    CY_CHECK_GT(adjusted_ticks, 0U);
    // They pass. The combined radius is 0.8 m; a solver that did nothing would bring them to zero.
    CY_CHECK_GT(nearest, 0.4F);
    CY_CHECK_GT(crowd.agent(*west)->position.x, 0.0F);
    CY_CHECK_LT(crowd.agent(*east)->position.x, 0.0F);
}

CY_TEST_CASE("the lower-priority agent takes more of the avoidance") {
    // `navigation`: "lower-priority agents SHALL yield, avoiding deadlock". Priority is a number
    // where LOWER yields, so the important agent holds its line and the other goes round.
    Crowd crowd(allocator(), 4.0F);
    AvoidanceParams important = walker();
    important.priority = 8;
    AvoidanceParams ordinary = walker();
    ordinary.priority = 1;

    const Expected<CrowdAgentId, Error> vip = crowd.add(Vec3{-4.0F, 0.0F, 0.0F}, important);
    const Expected<CrowdAgentId, Error> other = crowd.add(Vec3{4.0F, 0.0F, 0.0F}, ordinary);
    CY_REQUIRE(vip.has_value());
    CY_REQUIRE(other.has_value());

    f32 vip_drift = 0.0F;
    f32 other_drift = 0.0F;
    for (u32 tick = 0; tick < 120; ++tick) {
        crowd.set_desired_velocity(*vip, Vec3{2.0F, 0.0F, 0.0F});
        crowd.set_desired_velocity(*other, Vec3{-2.0F, 0.0F, 0.0F});
        CrowdReport report;
        CY_REQUIRE(crowd.step(1.0F / 60.0F, report).has_value());
        crowd.integrate(1.0F / 60.0F);
        vip_drift = std::fmax(vip_drift, std::fabs(crowd.agent(*vip)->position.z));
        other_drift = std::fmax(other_drift, std::fabs(crowd.agent(*other)->position.z));
    }
    CY_CHECK_GT(other_drift, vip_drift);
}

CY_TEST_CASE("a reduced tier scores fewer candidates over fewer neighbours") {
    // `navigation`: "agents at reduced AI LOD tiers SHALL use cheaper avoidance, with the fidelity
    // difference documented". The documentation is the table in crowd.h; this is the measurement.
    u32 scored[3] = {};
    const CrowdTier tiers[3] = {CrowdTier::Full, CrowdTier::Reduced, CrowdTier::Minimal};
    for (u32 index = 0; index < 3; ++index) {
        Crowd crowd(allocator(), 4.0F);
        for (u32 agent = 0; agent < 12; ++agent) {
            const f32 angle = (6.28318F * static_cast<f32>(agent)) / 12.0F;
            const Expected<CrowdAgentId, Error> id =
                crowd.add(Vec3{std::cos(angle) * 3.0F, 0.0F, std::sin(angle) * 3.0F}, walker());
            CY_REQUIRE(id.has_value());
            crowd.set_tier(*id, tiers[index]);
            crowd.set_desired_velocity(*id, Vec3{-std::cos(angle), 0.0F, -std::sin(angle)});
        }
        CrowdReport report;
        CY_REQUIRE(crowd.step(1.0F / 60.0F, report).has_value());
        scored[index] = report.candidates_scored;
        CY_CHECK_EQ(report.agents_by_tier[static_cast<usize>(tiers[index])], 12U);
    }
    CY_CHECK_GT(scored[0], scored[1]);
    CY_CHECK_GT(scored[1], scored[2]);
    CY_CHECK_EQ(scored[2], 0U);
}

CY_TEST_CASE("a crowd is deterministic: the same scenario twice is the same crowd") {
    const auto run = [](Array<Vec3>& out) noexcept {
        Crowd crowd(allocator(), 4.0F);
        for (u32 index = 0; index < 64; ++index) {
            const f32 angle = (6.28318F * static_cast<f32>(index)) / 64.0F;
            const Expected<CrowdAgentId, Error> id =
                crowd.add(Vec3{std::cos(angle) * 8.0F, 0.0F, std::sin(angle) * 8.0F}, walker());
            CY_REQUIRE(id.has_value());
            crowd.set_desired_velocity(
                *id, Vec3{-std::cos(angle) * 2.0F, 0.0F, -std::sin(angle) * 2.0F});
        }
        for (u32 tick = 0; tick < 40; ++tick) {
            CrowdReport report;
            CY_REQUIRE(crowd.step(1.0F / 60.0F, report).has_value());
            crowd.integrate(1.0F / 60.0F);
        }
        for (const CrowdAgent& agent : crowd.agents()) {
            CY_REQUIRE(out.push_back(agent.position).has_value());
        }
    };

    Array<Vec3> first(allocator());
    Array<Vec3> second(allocator());
    run(first);
    run(second);
    CY_REQUIRE_EQ(first.size(), second.size());
    for (usize index = 0; index < first.size(); ++index) {
        CY_REQUIRE_EQ(first[index].x, second[index].x);
        CY_REQUIRE_EQ(first[index].z, second[index].z);
    }
}

CY_TEST_CASE("a path follower and a field follower both produce a desired velocity") {
    NavMesh mesh = testing::single_tile_mesh(allocator(), 16.0F, 8);
    FlowFieldParams params;
    params.region = Aabb::from_min_max(Vec3{0.0F, -1.0F, 0.0F}, Vec3{16.0F, 1.0F, 16.0F});
    params.cell_size = 1.0F;
    FlowField field(allocator(), params);
    const Vec3 destination{15.0F, 0.0F, 8.0F};
    CY_REQUIRE(field.build(mesh, {&destination, 1}).has_value());
    const Vec3 velocity = follow_field(field, Vec3{1.0F, 0.0F, 8.0F}, 2.0F);
    CY_CHECK_GT(velocity.x, 0.5F);

    Array<PathPoint> path(allocator());
    for (const Vec3 point : {Vec3{0.0F, 0.0F, 0.0F}, Vec3{4.0F, 0.0F, 0.0F}}) {
        PathPoint entry;
        entry.position = point;
        CY_REQUIRE(path.push_back(entry).has_value());
    }
    u32 cursor = 1;
    const Vec3 along = follow_path(path.span(), Vec3{1.0F, 0.0F, 0.0F}, 2.0F, 0.2F, cursor);
    CY_CHECK_NEAR(along.x, 2.0F, 0.001F);
    // Past the last point, the follower asks for nothing rather than for the last direction.
    cursor = 2;
    const Vec3 done = follow_path(path.span(), Vec3{4.0F, 0.0F, 0.0F}, 2.0F, 0.2F, cursor);
    CY_CHECK_NEAR(length(done), 0.0F, 1e-6F);
}

CY_TEST_CASE("eight thousand agents hold their declared budget") {
    // THE MILESTONE'S OWN EXIT CRITERION, measured rather than asserted: "Cost is bounded by
    // configuration: 8,000 agents and 100 concurrent effects hold their budgets."
    //
    // The budget is stated as a per-agent cost so it scales with the machine rather than being a
    // wall-clock number that fails on a slow host and passes vacuously on a fast one, and the
    // headline below is the MEDIAN tick, not the fastest — hard rule 6 of this milestone's brief.
    constexpr u32 kAgents = 8000;
    // THIRTY TICKS, AND THE NUMBER IS THE HARNESS'S. Sixty of them cost about 700 ms of CPU on an
    // idle machine and over a second on a busy one, against the `integration` tier's budget of one
    // second — so the case failed its own budget guard occasionally, which is a flaky test however
    // right its subject is. Thirty samples still give a stable median (three runs agree to within
    // five per cent) and cost a third of the tier's budget. The agent count is what the exit
    // criterion names and it does not move.
    // TEN TICKS WHERE NOTHING IS INLINED, THIRTY WHERE IT IS, and the agent count never moves.
    // Thirty ticks of eight thousand agents cost 3.06 s of CPU in the Debug configuration against
    // the `integration` tier's one-second budget — the case was measuring the optimiser and failing
    // its own tier for it. Ten cost 0.95 s, a quarter of what the tier allows an unoptimised build
    // once tests/harness/src/budget.cpp accounts for one. What Debug
    // still checks is everything that is a COUNT rather than a time: the tier distribution and the
    // neighbour-test total, which are pure functions of the agents' positions and need a handful
    // of ticks rather than thirty. The population, which is what the exit criterion actually names,
    // is the same eight thousand in every configuration.
#if defined(CY_UNOPTIMISED)
    constexpr u32 kTicks = 10;
#else
    constexpr u32 kTicks = 30;
#endif
    constexpr f32 kDt = 1.0F / 60.0F;

    Crowd crowd(allocator(), 4.0F);
    AvoidanceParams params = walker();
    params.neighbour_distance = 2.5F;
    for (u32 index = 0; index < kAgents; ++index) {
        // A 100 x 80 lattice at one metre, which is a dense crowd rather than a sparse field: at
        // this spacing every agent has neighbours every tick, so nothing is measured empty.
        const u32 row = index / 100u;
        const f32 x = static_cast<f32>(index % 100u);
        const f32 z = static_cast<f32>(row);
        const Expected<CrowdAgentId, Error> id = crowd.add(Vec3{x, 0.0F, z}, params);
        CY_REQUIRE(id.has_value());
        // Two halves walking into each other, so avoidance actually has to resolve something.
        crowd.set_desired_velocity(*id, Vec3{(z < 40.0F) ? 1.5F : -1.5F, 0.0F, 0.0F});
        crowd.set_tier(*id, (index % 4u == 0u) ? CrowdTier::Full : CrowdTier::Reduced);
    }
    CY_REQUIRE_EQ(crowd.size(), kAgents);

    Array<f64> milliseconds(allocator());
    Array<f64> neighbour_tests(allocator());
    CrowdReport last;
    for (u32 tick = 0; tick < kTicks; ++tick) {
        const f64 started = cpu_millis();
        CY_REQUIRE(crowd.step(kDt, last).has_value());
        const f64 finished = cpu_millis();
        crowd.integrate(kDt);
        CY_REQUIRE(milliseconds.push_back(finished - started).has_value());
        CY_REQUIRE(neighbour_tests.push_back(static_cast<f64>(last.neighbour_tests)).has_value());
    }

    // The median and the worst, by a selection sort over sixty values.
    const f64 work_median = sort_and_median(neighbour_tests);
    const f64 work_worst = neighbour_tests[neighbour_tests.size() - 1];
    const f64 median = sort_and_median(milliseconds);
    const f64 worst = milliseconds[milliseconds.size() - 1];
    const f64 per_agent_us = (median * 1000.0) / static_cast<f64>(kAgents);

    CY_TEST_MESSAGE("8000 agents, " << kTicks << " ticks: median " << median
                                    << " ms/tick of CPU, worst " << worst << " ms, " << per_agent_us
                                    << " us/agent/tick, " << work_median
                                    << " neighbour tests/tick");
    CY_CHECK_EQ(last.agents, kAgents);
    CY_CHECK_EQ(last.agents_by_tier[static_cast<usize>(CrowdTier::Full)], kAgents / 4);
    CY_CHECK_GT(last.neighbour_tests, kAgents);

    // THE BUDGET. Eight thousand agents at 60 Hz have 16.6 ms of frame; navigation's declared share
    // of it is a quarter, so ~2 us per agent per tick. The threshold is set at four times that so
    // it is a REGRESSION DETECTOR rather than a benchmark of this machine: a change that doubles
    // the solver's cost still passes, one that makes it an order of magnitude worse does not.
    //
    // AND IT IS A CLAIM ABOUT AN OPTIMISED BUILD, which is what `CY_UNOPTIMISED` records. The same
    // solver costs 5.07 us/agent/tick in Development and 12.35 in Debug, so asserting eight in a
    // `-O0` build asserts something the exit criterion never claimed and that no shipped game will
    // ever run. The figure is still MEASURED and still PRINTED there — the message above is
    // unconditional — and the structural assertions below, which are counts rather than times, hold
    // in every configuration. `tests/harness/`'s calibrated budget does not cover this, and
    // cmake/profiles.cmake says why beside the macro.
#if !defined(CY_UNOPTIMISED)
    CY_CHECK_LT(per_agent_us, 8.0);
#endif
    // AND THE WORK ITSELF IS STABLE TICK TO TICK. This is the structural form of "the grid did not
    // degrade": the number of neighbour tests is what the spatial partition actually cost, it is a
    // pure function of the agents' positions, and it is not a wall-clock measurement — so it says
    // what a `worst < median * k` on elapsed time was trying to say without also measuring
    // whatever else this machine was running. A grid that stopped partitioning would show here as
    // a tick testing every agent against every other.
    CY_CHECK_LT(work_worst, work_median * 2.0);
    // And it is a small fraction of what testing every pair would cost, which is the property the
    // uniform grid exists to have. Eight thousand agents against each other is 64 million tests;
    // the grid does this in about a fiftieth of that, and the bound is set at a twentieth so it
    // fails on a partition that stopped partitioning rather than on a denser crowd.
    CY_CHECK_LT(work_median, static_cast<f64>(kAgents) * static_cast<f64>(kAgents) * 0.05);
    // The worst tick is REPORTED and not asserted. Even on the CPU clock it carries the cost of a
    // cold cache and of whatever the scheduler did on the first tick, and turning that into a
    // threshold would be measuring the host rather than the code. `work_worst` above is the
    // regression detector it would have been standing in for.
}

CY_TEST_CASE("a crowd torn down mid-step releases everything it holds") {
    // Hard rule 4 of this milestone's brief: teardown under load. Twelve crowds, each built to a
    // thousand agents, stepped, and destroyed with agents still in them — under a sanitizer this
    // is what finds a leak or a use-after-free in the grid's scratch arrays.
    for (u32 round = 0; round < 12; ++round) {
        Crowd crowd(allocator(), 4.0F);
        for (u32 index = 0; index < 1000; ++index) {
            const u32 row = index / 40u;
            const Expected<CrowdAgentId, Error> id = crowd.add(
                Vec3{static_cast<f32>(index % 40u), 0.0F, static_cast<f32>(row)}, walker());
            CY_REQUIRE(id.has_value());
            crowd.set_desired_velocity(*id, Vec3{1.0F, 0.0F, 0.0F});
        }
        CrowdReport report;
        CY_REQUIRE(crowd.step(1.0F / 60.0F, report).has_value());
        // Half of them removed while the grid still names them, then stepped again: a step that
        // read a removed agent through the grid would be caught here.
        for (u32 index = 0; index < 1000; index += 2) {
            CY_REQUIRE(crowd.remove(index).has_value());
        }
        CY_REQUIRE(crowd.step(1.0F / 60.0F, report).has_value());
        CY_CHECK_EQ(report.agents, 500U);
    }
}
