// SPDX-License-Identifier: MIT
// `integration.movement_crowd`: navigation's crowd solver instantiated over `Fixed`.
// openspec/changes/add-deterministic-math task 6.2.
//
// The same behaviours `integration.navigation_crowd` asks of the f32 crowd — a head-on pair steers
// round and passes, the lower priority yields — asked of the `Fixed` instantiation of the same
// text, then what only the `Fixed` one can claim: a committed digest, the same on every leg, and
// avoidance feeding the kinematic mover. Integration rather than unit because hundreds of ticks of
// a wide-arithmetic solver do not fit a unit case's millisecond in an unoptimised build.

#include <cy/core/detmath/convert.h>
#include <cy/core/detmath/functions.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/movement/crowd.h>
#include <cy/movement/mover.h>
#include <cy/navigation/crowd.h>
#include <cy/test/test.h>

#include <cmath>
#include <ios>
#include <utility>

using namespace cy;
using namespace cy::movement;
using detmath::Fixed;
using detmath::FixedVec2;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr i64 kOne = Fixed::kOneRaw;

[[nodiscard]] Fixed metres(i64 thousandths) noexcept {
    return Fixed::from_raw((kOne / 1000) * thousandths);
}

/// `walker()` of src/navigation/tests/test_crowd.cpp, in `Fixed`.
[[nodiscard]] FixedAvoidanceParams walker() noexcept {
    FixedAvoidanceParams params;
    params.radius = metres(400);
    params.max_speed = Fixed::from_int(2);
    params.max_acceleration = Fixed::from_int(20);
    params.neighbour_distance = Fixed::from_int(3);
    params.max_neighbours = 6;
    params.time_horizon = Fixed::from_int(2);
    return params;
}

[[nodiscard]] Fixed sixtieth() noexcept {
    return Fixed::one() / Fixed::from_int(60);
}

void integrate(FixedCrowd& crowd) noexcept {
    crowd.integrate(sixtieth());
}

/// The head-on pair, `ticks` long, in the `Fixed` crowd: the nearest they came, and the largest
/// lateral excursion of each.
struct PairRun {
    Fixed nearest = Fixed::max();
    Fixed west_drift;
    Fixed east_drift;
    FixedVec2 west_end;
    FixedVec2 east_end;
    u32 adjusted_ticks = 0;
};

[[nodiscard]] PairRun run_pair(const FixedAvoidanceParams& west_params,
                               const FixedAvoidanceParams& east_params, i32 start,
                               u32 ticks) noexcept {
    PairRun run;
    FixedCrowd crowd(allocator(), Fixed::from_int(4));
    const auto west = crowd.add(FixedVec2{Fixed::from_int(-start), Fixed::zero()}, west_params);
    const auto east = crowd.add(FixedVec2{Fixed::from_int(start), Fixed::zero()}, east_params);
    if (!west || !east) {
        return run;
    }
    for (u32 tick = 0; tick < ticks; ++tick) {
        crowd.set_desired_velocity(*west, FixedVec2{Fixed::from_int(2), Fixed::zero()});
        crowd.set_desired_velocity(*east, FixedVec2{Fixed::from_int(-2), Fixed::zero()});
        navigation::CrowdReport report;
        if (!crowd.step(sixtieth(), report)) {
            return run;
        }
        integrate(crowd);
        run.adjusted_ticks += report.agents_adjusted > 0 ? 1U : 0U;
        const FixedVec2 w = crowd.agent(*west)->position;
        const FixedVec2 e = crowd.agent(*east)->position;
        const Fixed apart = detmath::distance(w, e);
        run.nearest = apart < run.nearest ? apart : run.nearest;
        run.west_drift = detmath::abs(w.y) > run.west_drift ? detmath::abs(w.y) : run.west_drift;
        run.east_drift = detmath::abs(e.y) > run.east_drift ? detmath::abs(e.y) : run.east_drift;
    }
    run.west_end = crowd.agent(*west)->position;
    run.east_end = crowd.agent(*east)->position;
    return run;
}

/// A ring of agents on a circle of 6 m, each walking through the centre: 24 of them, 1.57 m apart
/// and so clear of each other at the start, which keeps every candidate's time to collision a real
/// question rather than "already touching" — the share, the side bias and the lattice all decide.
/// Every quantity comes from an integer or a detmath function, so the run is the same bits
/// everywhere.
[[nodiscard]] u64 ring_digest(u32 agents, u32 ticks) noexcept {
    FixedCrowd crowd(allocator(), Fixed::from_int(4));
    for (u32 index = 0; index < agents; ++index) {
        const auto turn = static_cast<u32>((static_cast<u64>(index) << 32U) / agents);
        const detmath::SinCos at = detmath::sincos(detmath::Angle::from_raw(turn));
        FixedAvoidanceParams params = walker();
        params.priority = static_cast<u8>(index % 3);
        const auto id =
            crowd.add(FixedVec2{at.cos * Fixed::from_int(6), at.sin * Fixed::from_int(6)}, params);
        if (!id) {
            return 0;
        }
        crowd.set_tier(*id, static_cast<navigation::CrowdTier>(index % 3));
        crowd.set_desired_velocity(
            *id, FixedVec2{-at.cos * Fixed::from_int(2), -at.sin * Fixed::from_int(2)});
    }
    u64 fold = 0;
    for (u32 tick = 0; tick < ticks; ++tick) {
        navigation::CrowdReport report;
        if (!crowd.step(sixtieth(), report)) {
            return 0;
        }
        integrate(crowd);
        fold = hash_combine(fold, crowd_state_hash(crowd));
        fold = hash_combine(fold, report.candidates_scored);
        fold = hash_combine(fold, report.agents_adjusted);
    }
    return fold;
}

/// The ring's digest, committed. Computed by this file on x86-64 with GCC 13; every leg must
/// reproduce it, and a change to the solver, the policy or detmath that moves any bit moves it.
constexpr u64 kRingDigest = 0x2FC8'A66A'0AB2'5CBCULL;

}  // namespace

CY_TEST_CASE("a Fixed crowd steers a head-on pair around each other, and they pass") {
    const PairRun run = run_pair(walker(), walker(), 6, 400);
    CY_CHECK_GT(run.adjusted_ticks, 0U);
    // The combined radius is 0.8 m; a solver that did nothing would bring them to zero.
    CY_CHECK_GT(run.nearest.raw, metres(400).raw);
    CY_CHECK_GT(run.west_end.x.raw, 0);
    CY_CHECK_LT(run.east_end.x.raw, 0);
}

CY_TEST_CASE("in a Fixed crowd the lower-priority agent takes more of the avoidance") {
    FixedAvoidanceParams important = walker();
    important.priority = 8;
    FixedAvoidanceParams ordinary = walker();
    ordinary.priority = 1;
    const PairRun run = run_pair(important, ordinary, 4, 120);
    CY_CHECK_GT(run.east_drift.raw, run.west_drift.raw);
}

CY_TEST_CASE("the Fixed and the f32 crowd are one algorithm: they walk the same paths") {
    // The priority pair of `integration.navigation_crowd`, run in both instantiations side by side.
    // Priority breaks the symmetry, so no tie is left for rounding noise to decide, and the two
    // kinds of arithmetic must trace the same paths — within a millimetre, which is f32's own error
    // over a few metres and a few hundred steps, and far below any decision the solver makes.
    FixedAvoidanceParams important = walker();
    important.priority = 8;
    FixedAvoidanceParams ordinary = walker();
    ordinary.priority = 1;
    FixedCrowd fixed(allocator(), Fixed::from_int(4));
    const auto fixed_west = fixed.add(FixedVec2{Fixed::from_int(-4), Fixed::zero()}, important);
    const auto fixed_east = fixed.add(FixedVec2{Fixed::from_int(4), Fixed::zero()}, ordinary);

    navigation::AvoidanceParams params;
    params.radius = 0.4F;
    params.max_speed = 2.0F;
    params.max_acceleration = 20.0F;
    params.neighbour_distance = 3.0F;
    params.time_horizon = 2.0F;
    navigation::Crowd floats(allocator(), 4.0F);
    params.priority = 8;
    const auto float_west = floats.add(Vec3{-4.0F, 0.0F, 0.0F}, params);
    params.priority = 1;
    const auto float_east = floats.add(Vec3{4.0F, 0.0F, 0.0F}, params);
    CY_REQUIRE(fixed_west.has_value());
    CY_REQUIRE(fixed_east.has_value());
    CY_REQUIRE(float_west.has_value());
    CY_REQUIRE(float_east.has_value());

    f64 worst = 0.0;
    f64 drift = 0.0;
    for (u32 tick = 0; tick < 200; ++tick) {
        fixed.set_desired_velocity(*fixed_west, FixedVec2{Fixed::from_int(2), Fixed::zero()});
        fixed.set_desired_velocity(*fixed_east, FixedVec2{Fixed::from_int(-2), Fixed::zero()});
        floats.set_desired_velocity(*float_west, Vec3{2.0F, 0.0F, 0.0F});
        floats.set_desired_velocity(*float_east, Vec3{-2.0F, 0.0F, 0.0F});
        navigation::CrowdReport report;
        CY_REQUIRE(fixed.step(sixtieth(), report).has_value());
        CY_REQUIRE(floats.step(1.0F / 60.0F, report).has_value());
        integrate(fixed);
        floats.integrate(1.0F / 60.0F);
        for (const auto& [a, b] :
             {std::pair{*fixed_west, *float_west}, std::pair{*fixed_east, *float_east}}) {
            const FixedVec2 at = fixed.agent(a)->position;
            const Vec3 there = floats.agent(b)->position;
            const f64 dx =
                detmath::to_f64_relative(at.x, Fixed::zero()) - static_cast<f64>(there.x);
            const f64 dz =
                detmath::to_f64_relative(at.y, Fixed::zero()) - static_cast<f64>(there.z);
            worst = std::fmax(worst, std::fmax(std::fabs(dx), std::fabs(dz)));
            drift = std::fmax(drift, std::fabs(static_cast<f64>(there.z)));
        }
    }
    CY_TEST_MESSAGE("worst disagreement " << worst << " m over a lateral excursion of " << drift
                                          << " m");
    // The pair really did avoid, so the comparison is of decisions, not of two straight lines.
    CY_CHECK_GT(drift, 0.5);
    CY_CHECK_LT(worst, 0.001);
}

CY_TEST_CASE("a Fixed crowd is the same crowd twice, and the committed one on every leg") {
    const u64 first = ring_digest(24, 240);
    const u64 second = ring_digest(24, 240);
    CY_REQUIRE_NE(first, 0ULL);
    CY_CHECK_EQ(first, second);
    CY_TEST_MESSAGE("ring digest " << first);
    CY_CHECK_EQ(first, kRingDigest);
}

CY_TEST_CASE("avoidance through the mover keeps a crossing pair further apart than separation") {
    // Two units on a collision course in the kinematic mover. Without the crowd, only the mover's
    // separation acts, and only once they overlap; with `avoid()`, they steer round first.
    const auto nearest = [](bool with_crowd) noexcept {
        KinematicMover mover(allocator(), MoverParams{});
        UnitDesc west;
        west.entity = 1;
        west.position = FixedVec2{Fixed::from_int(-6), Fixed::zero()};
        west.radius = metres(400);
        UnitDesc east = west;
        east.entity = 2;
        east.position = FixedVec2{Fixed::from_int(6), Fixed::zero()};
        if (!mover.add(west) || !mover.add(east)) {
            return Fixed::zero();
        }
        FixedCrowd crowd(allocator(), Fixed::from_int(4));
        Fixed closest = Fixed::max();
        for (u32 tick = 0; tick < 240; ++tick) {
            mover.set_desired_velocity(0, FixedVec2{Fixed::from_int(2), Fixed::zero()});
            mover.set_desired_velocity(1, FixedVec2{Fixed::from_int(-2), Fixed::zero()});
            navigation::CrowdReport report;
            if (with_crowd && !avoid(mover, crowd, walker(), report)) {
                return Fixed::zero();
            }
            if (!mover.step()) {
                return Fixed::zero();
            }
            const Fixed apart = detmath::distance(mover.position(0), mover.position(1));
            closest = apart < closest ? apart : closest;
        }
        return closest;
    };
    const Fixed separated = nearest(false);
    const Fixed avoided = nearest(true);
    CY_TEST_MESSAGE("closest approach: separation only " << separated.raw << ", with the crowd "
                                                         << avoided.raw);
    CY_CHECK_GT(avoided.raw, separated.raw);
    // The combined radius is 0.8 m: with the crowd they never touch.
    CY_CHECK_GT(avoided.raw, metres(800).raw);
}

CY_TEST_CASE("a lattice direction is an exact binary angle") {
    // A quarter of eight spokes is a quarter turn, whose sine is exactly one.
    const FixedVec2 north = FixedCrowdPolicy::lattice(2, 8, Fixed::one());
    CY_CHECK_EQ(north.x.raw, 0);
    CY_CHECK_EQ(north.y.raw, kOne);
    const FixedVec2 west = FixedCrowdPolicy::lattice(4, 8, Fixed::from_int(2));
    CY_CHECK_EQ(west.x.raw, -2 * kOne);
    CY_CHECK_EQ(west.y.raw, 0);
    CY_CHECK_EQ(FixedCrowdPolicy::cell_span(Fixed::from_int(3), Fixed::from_int(4)), 1);
    CY_CHECK_EQ(FixedCrowdPolicy::cell_span(Fixed::from_int(8), Fixed::from_int(4)), 2);
    CY_CHECK_EQ(FixedCrowdPolicy::cell(Fixed::from_raw(-1), Fixed::from_int(4)), -1);
}
