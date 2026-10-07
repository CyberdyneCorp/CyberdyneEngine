// SPDX-License-Identifier: MIT
// THE FIXED-POINT KINEMATIC MOVER, PASS BY PASS. Task 6.1, design §8.
//
// Each case puts a handful of units where one pass's rule decides the answer exactly: the
// acceleration limit, the half share of an overlap, the touching boundary, the coincident split,
// the push out of a static capsule, the clamp at the mesh's edge, and the heading.

#include <cy/movement/mover.h>

#include "movement_fixture.h"

namespace {

using cy::u32;
using cy::u64;
using cy::detmath::Angle;
using cy::detmath::FixedCapsule2D;
using cy::movement::KinematicMover;
using cy::movement::MoverParams;
using cy::movement::MoverReport;
using cy::movement::UnitDesc;
using namespace cy::movement_test;

[[nodiscard]] MoverParams small_world() noexcept {
    MoverParams params;
    params.grid_min = at(-16, -16);
    params.grid_max = at(16, 16);
    return params;
}

[[nodiscard]] UnitDesc unit(u64 entity, FixedVec2 position) noexcept {
    UnitDesc desc;
    desc.entity = entity;
    desc.position = position;
    return desc;
}

[[nodiscard]] MoverReport step(KinematicMover& mover) noexcept {
    const auto report = mover.step();
    CY_REQUIRE(report.has_value());
    return *report;
}

[[nodiscard]] cy::i64 ulps(Fixed a, Fixed b) noexcept {
    const cy::i64 difference = a.raw - b.raw;
    return difference < 0 ? -difference : difference;
}

}  // namespace

CY_TEST_CASE("movement mover: velocity steers within the acceleration limit and caps at speed") {
    KinematicMover mover(allocator(), small_world());
    CY_REQUIRE(mover.add(unit(1, at(0, 0))).has_value());
    mover.set_desired_velocity(0, at(10, 0));
    (void)step(mover);
    // 16 m/s^2 for one sixtieth of a second.
    const Fixed limit = metres(16) * mover.dt();
    CY_CHECK(mover.velocity(0).x == limit);
    CY_CHECK(mover.velocity(0).y == Fixed::zero());
    CY_CHECK(mover.position(0).x == mover.velocity(0).x * mover.dt());

    for (int tick = 0; tick < 60; ++tick) {
        (void)step(mover);
    }
    // Capped at 4 m/s from the desired 10, exactly: 4 * |v| / |v| through the 128-bit product.
    CY_CHECK(mover.velocity(0).x == metres(4));
}

CY_TEST_CASE("movement mover: an overlapping pair separates, each unit taking half") {
    KinematicMover mover(allocator(), small_world());
    const FixedVec2 right{fraction(3, 0) / metres(5), Fixed::zero()};  // 0.6 m
    CY_REQUIRE(mover.add(unit(1, at(0, 0))).has_value());
    CY_REQUIRE(mover.add(unit(2, right)).has_value());
    const MoverReport report = step(mover);
    CY_CHECK_EQ(report.overlaps, 2U);
    // Radii 0.5 each: an overlap of 0.4, of which each takes 0.2 along the line of centres.
    const Fixed share = (metres(1) - right.x) * Fixed::half();
    CY_CHECK(mover.position(0) == (FixedVec2{-share, Fixed::zero()}));
    CY_CHECK(mover.position(1) == (FixedVec2{right.x + share, Fixed::zero()}));
}

CY_TEST_CASE("movement mover: units that exactly touch are left where they are") {
    KinematicMover mover(allocator(), small_world());
    CY_REQUIRE(mover.add(unit(1, at(0, 0))).has_value());
    CY_REQUIRE(mover.add(unit(2, at(1, 0))).has_value());
    const MoverReport report = step(mover);
    CY_CHECK_EQ(report.overlaps, 0U);
    CY_CHECK(mover.position(0) == at(0, 0));
    CY_CHECK(mover.position(1) == at(1, 0));
}

CY_TEST_CASE("movement mover: coincident units split along x, the lower index toward -x") {
    KinematicMover mover(allocator(), small_world());
    CY_REQUIRE(mover.add(unit(7, at(2, 2))).has_value());
    CY_REQUIRE(mover.add(unit(9, at(2, 2))).has_value());
    (void)step(mover);
    // The full share would be 0.5 each; one pass moves a unit at most `max_push`, a quarter metre.
    const Fixed push = mover.params().max_push;
    CY_CHECK(mover.position(0) == (FixedVec2{metres(2) - push, metres(2)}));
    CY_CHECK(mover.position(1) == (FixedVec2{metres(2) + push, metres(2)}));
}

CY_TEST_CASE("movement mover: a unit is pushed out of a static capsule along the normal") {
    KinematicMover mover(allocator(), small_world());
    CY_REQUIRE(mover.add_obstacle(FixedCapsule2D{at(2, 0), at(2, 8), fraction(1, 1)}).has_value());
    CY_REQUIRE(mover.add(unit(1, FixedVec2{fraction(11, 2), metres(1)})).has_value());  // x 2.75
    // Adding settles the unit: 0.75 from the wall's core, inside reach 1.0, so out to exactly 3.
    CY_CHECK(mover.position(0) == at(3, 1));
    const MoverReport report = step(mover);
    CY_CHECK_EQ(report.obstacle_contacts, 0U);
    CY_CHECK(mover.position(0) == at(3, 1));
}

CY_TEST_CASE("movement mover: the navigation surface stops a unit at its edge") {
    const cy::movement::FixedNavMesh mesh = converted(open_mesh(4));
    KinematicMover mover(allocator(), small_world());
    CY_REQUIRE(mover.add(unit(1, FixedVec2{fraction(1, 1), fraction(3, 1)})).has_value());
    mover.bind(&mesh, nullptr);
    CY_CHECK_EQ(mover.poly(0), 4U);
    mover.set_desired_velocity(0, at(-4, 0));
    u32 clamped = 0;
    for (int tick = 0; tick < 30; ++tick) {
        clamped += step(mover).clamped;
    }
    CY_CHECK_GT(clamped, 0U);
    CY_CHECK(mover.position(0).x == Fixed::zero());
    CY_CHECK(mover.position(0).y == fraction(3, 1));
    CY_CHECK_EQ(mover.poly(0), 4U);
    CY_CHECK(mover.height(0) == Fixed::zero());
}

CY_TEST_CASE("movement mover: units are added in ascending entity order") {
    KinematicMover mover(allocator(), small_world());
    CY_REQUIRE(mover.add(unit(5, at(0, 0))).has_value());
    CY_CHECK_FALSE(mover.add(unit(3, at(4, 0))).has_value());
    CY_CHECK_FALSE(mover.add(unit(5, at(4, 0))).has_value());
    CY_CHECK(mover.add(unit(6, at(4, 0))).has_value());
    CY_CHECK_EQ(mover.size(), 2U);
}

CY_TEST_CASE("movement mover: the heading faces the velocity, and -Z is a heading of zero") {
    KinematicMover mover(allocator(), small_world());
    CY_REQUIRE(mover.add(unit(1, at(0, 0))).has_value());
    CY_REQUIRE(mover.add(unit(2, at(8, 0))).has_value());
    mover.set_desired_velocity(0, at(0, -2));
    mover.set_desired_velocity(1, at(2, 0));
    for (int tick = 0; tick < 4; ++tick) {
        (void)step(mover);
    }
    CY_CHECK(mover.heading(0) == Angle{});
    // A yaw of minus a quarter turn takes -Z to +X.
    CY_CHECK(mover.heading(1) == -Angle::quarter());
    const cy::detmath::FixedVec3 facing = mover.transform(1).forward();
    CY_CHECK(ulps(facing.x, Fixed::one()) <= 4);
    CY_CHECK(ulps(facing.z, Fixed::zero()) <= 4);
}

CY_TEST_CASE("movement mover: the state hash follows the state and nothing else") {
    KinematicMover first(allocator(), small_world());
    KinematicMover second(allocator(), small_world());
    for (KinematicMover* mover : {&first, &second}) {
        CY_REQUIRE(mover->add(unit(1, at(0, 0))).has_value());
        CY_REQUIRE(mover->add(unit(2, FixedVec2{fraction(1, 1), Fixed::zero()})).has_value());
        mover->set_desired_velocity(0, at(1, 1));
    }
    CY_CHECK_EQ(first.state_hash(), second.state_hash());
    CY_CHECK_EQ(first.params_hash(), second.params_hash());
    (void)step(first);
    CY_CHECK_NE(first.state_hash(), second.state_hash());
    (void)step(second);
    CY_CHECK_EQ(first.state_hash(), second.state_hash());

    MoverParams other = small_world();
    other.max_push = fraction(1, 3);
    const KinematicMover different(allocator(), other);
    CY_CHECK_NE(different.params_hash(), first.params_hash());
}
