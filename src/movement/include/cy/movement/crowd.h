// SPDX-License-Identifier: MIT
#pragma once
// Crowd avoidance in a `Fixed` world: `navigation`'s sampled reciprocal-velocity-obstacle solver,
// instantiated over `Fixed`. openspec/changes/add-deterministic-math task 6.2 (design §9.1).
//
// ONE ALGORITHM, TWO KINDS OF ARITHMETIC. `navigation::BasicCrowd<Policy>`
// (cy/navigation/crowd_solver.h) is the solver — the grid, the (distance, id) neighbour order, the
// candidate lattice, the reciprocal share tilted by priority, the side bias, the time-to-collision
// score and the acceleration limit — written once. `navigation::Crowd` instantiates it over f32 for
// every float world; `FixedCrowd` here instantiates the same text over `FixedCrowdPolicy`:
//
//   Scalar   Fixed        Vec   FixedVec2 (x, world Z)        Wide   WideFixed
//
// so every squared distance and every dot product the solver compares is EXACT (Q64.64), every
// root is the correctly rounded wide square root, every lattice direction is `detmath::sincos` of a
// binary angle (a spoke of `count` is exactly `spoke / count` of a turn), and every tie is broken
// by agent index. The same scenario gives the same bits on every architecture.
//
// AVOIDANCE PRODUCES A VELOCITY. As in `navigation`, `step()` writes velocities and touches no
// position. In a lockstep world the kinematic mover moves units (mover.h): `avoid()` below mirrors
// the mover's units into the crowd, solves, and hands each avoided velocity back to the mover as
// the unit's desired velocity — so the mover's own separation stays what keeps units from
// interpenetrating, and the crowd is what keeps them from walking into each other in the first
// place.

#include <cy/core/base/expected.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>
#include <cy/navigation/crowd_solver.h>

namespace cy::movement {

class KinematicMover;

using detmath::Fixed;
using detmath::FixedVec2;
using detmath::WideFixed;

/// `navigation::AvoidanceParams` in `Fixed`: the parameters the solver reads. The float struct's
/// height and obstacle horizon are absent because the solver reads neither.
struct FixedAvoidanceParams {
    Fixed radius = Fixed::half();
    Fixed max_speed = Fixed::from_raw((Fixed::kOneRaw * 7) / 2);
    Fixed max_acceleration = Fixed::from_int(8);
    Fixed neighbour_distance = Fixed::from_int(4);
    u32 max_neighbours = 6;
    Fixed time_horizon = Fixed::from_int(2);
    /// Higher yields less.
    u8 priority = 0;
    /// How hard the agent pushes away from its neighbours regardless of where they are heading.
    Fixed separation_weight = Fixed::from_raw((Fixed::kOneRaw * 3) / 5);
};

/// One agent: `navigation::CrowdAgent` in `Fixed`, on the plane.
struct FixedCrowdAgent {
    FixedVec2 position;
    FixedVec2 velocity;
    FixedVec2 desired_velocity;
    FixedAvoidanceParams params;
    navigation::CrowdTier tier = navigation::CrowdTier::Full;
    bool active = false;
};

/// The solver's arithmetic in `Fixed`. Each constant is the f32 policy's, as the nearest raw value
/// below it; each comparison of a square is exact. Defined in src/crowd.cpp.
struct FixedCrowdPolicy {
    using Scalar = Fixed;
    using Vec = FixedVec2;
    using Wide = WideFixed;
    using Agent = FixedCrowdAgent;
    using Params = FixedAvoidanceParams;

    [[nodiscard]] static FixedVec2 flatten(FixedVec2 v) noexcept { return v; }
    [[nodiscard]] static Fixed across(FixedVec2 v) noexcept { return v.x; }
    [[nodiscard]] static Fixed along(FixedVec2 v) noexcept { return v.y; }
    [[nodiscard]] static WideFixed length_squared(FixedVec2 v) noexcept;
    [[nodiscard]] static WideFixed dot(FixedVec2 a, FixedVec2 b) noexcept;
    [[nodiscard]] static WideFixed square(Fixed s) noexcept;
    /// The product of two wide values, through their nearest `Fixed` (saturating): what the
    /// time-to-collision discriminant multiplies, whose factors are squared speeds and distances.
    [[nodiscard]] static WideFixed product(WideFixed a, WideFixed b) noexcept;
    [[nodiscard]] static Fixed narrow(WideFixed w) noexcept { return w.saturating_narrow(); }
    [[nodiscard]] static Fixed sqrt(WideFixed w) noexcept;
    [[nodiscard]] static Fixed length(FixedVec2 v) noexcept;
    [[nodiscard]] static FixedVec2 clamp_speed(FixedVec2 v, Fixed max_speed) noexcept;
    [[nodiscard]] static FixedVec2 perpendicular(FixedVec2 v) noexcept { return {v.y, -v.x}; }
    /// `magnitude` along `spoke / count` of a turn: `sincos` of an exact binary angle.
    [[nodiscard]] static FixedVec2 lattice(u32 spoke, u32 count, Fixed magnitude) noexcept;
    [[nodiscard]] static Fixed ratio(u32 numerator, u32 denominator) noexcept;
    [[nodiscard]] static Fixed lesser(Fixed a, Fixed b) noexcept { return b < a ? b : a; }
    [[nodiscard]] static Fixed greater(Fixed a, Fixed b) noexcept { return a < b ? b : a; }
    [[nodiscard]] static i32 cell(Fixed coordinate, Fixed size) noexcept;
    [[nodiscard]] static i32 cell_span(Fixed range, Fixed size) noexcept;
    [[nodiscard]] static Fixed responsibility(u8 mine, u8 theirs) noexcept;
    [[nodiscard]] static FixedVec2 zero_vec() noexcept { return FixedVec2{}; }
    [[nodiscard]] static Fixed zero() noexcept { return Fixed::zero(); }
    [[nodiscard]] static WideFixed wide_zero() noexcept { return WideFixed{}; }
    [[nodiscard]] static Fixed one() noexcept { return Fixed::one(); }
    [[nodiscard]] static Fixed two() noexcept { return Fixed::from_int(2); }
    [[nodiscard]] static Fixed half() noexcept { return Fixed::half(); }
    /// "Never": the largest value, which no time compares below.
    [[nodiscard]] static Fixed infinity() noexcept { return Fixed::max(); }
    [[nodiscard]] static Fixed default_cell_size() noexcept { return Fixed::from_int(4); }
    /// 0.15 of the distance.
    [[nodiscard]] static Fixed side_bias() noexcept {
        return Fixed::from_raw((Fixed::kOneRaw * 15) / 100);
    }
    /// 10^-4 m.
    [[nodiscard]] static Fixed separation_floor() noexcept {
        return Fixed::from_raw(Fixed::kOneRaw / 10000);
    }
    /// 2^-30 m²/s²: four raw units, so the narrowed speed it guards never divides as zero.
    [[nodiscard]] static WideFixed still_speed_squared() noexcept {
        return WideFixed::from_fixed(Fixed::from_raw(4));
    }
    /// 10^-6 m/s.
    [[nodiscard]] static Fixed change_floor() noexcept {
        return Fixed::from_raw(Fixed::kOneRaw / 1000000);
    }
    /// 10^-3 m/s.
    [[nodiscard]] static Fixed adjusted_floor() noexcept {
        return Fixed::from_raw(Fixed::kOneRaw / 1000);
    }
};

/// The crowd of a `Fixed` world.
using FixedCrowd = navigation::BasicCrowd<FixedCrowdPolicy>;

/// Every agent's position, velocity and desired velocity, folded in agent order.
[[nodiscard]] u64 crowd_state_hash(const FixedCrowd& crowd) noexcept;

/// One avoidance pass over the mover's units, before the mover steps: agent `i` is unit `i`.
///
///   1. a unit without an agent gets one, with `defaults` and the unit's own radius and speed;
///   2. each agent takes its unit's position, current velocity and desired velocity, and is active
///      exactly when the unit is;
///   3. the crowd steps over the mover's tick length;
///   4. each unit's desired velocity becomes its agent's avoided velocity.
///
/// Called every tick after path following has set the desired velocities.
[[nodiscard]] Status avoid(KinematicMover& mover, FixedCrowd& crowd,
                           const FixedAvoidanceParams& defaults,
                           navigation::CrowdReport& report) noexcept;

}  // namespace cy::movement

namespace cy::navigation {
extern template class BasicCrowd<movement::FixedCrowdPolicy>;
}  // namespace cy::navigation
