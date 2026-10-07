// SPDX-License-Identifier: MIT
#pragma once
// The fixed-point kinematic mover: how authoritative units move in a `Lockstep` session. Design §8.
//
// ================================================================================================
// NOT A PHYSICS ENGINE, AND PHYSICS IS NOT ITS INPUT
// ================================================================================================
//
// Design §8 chose option (c): authoritative units move with a `Fixed` integrator, collide with
// static navigation and height data and with each other as circles on the plane, and Jolt runs
// debris, ragdolls and secondary effects as PRESENTATION (`PhysicsAuthority::Presentation`, which
// `cy::physics::validate_session()` already accepts under `Lockstep`). Nothing here reads a physics
// body, and nothing a physics body does reaches a unit.
//
// ONE TICK, IN ORDER:
//
//   1. integrate   steer each unit's velocity toward its desired velocity within its acceleration
//                  limit, cap it at its speed, and move it by velocity * dt
//   2. separate    resolve overlaps between units by deterministic pairwise separation over a grid
//                  keyed by Fixed cell coordinates (crowd_kernel.h): each unit takes a share of
//                  every overlap, along the line between the centres
//   3. obstacles   push each unit out of every static circle and capsule it overlaps, in the order
//                  the obstacles were added
//   4. clamp       keep the unit on the navigation surface (`FixedNavMesh::clamp_move`)
//   5. height      from the cooked height samples, or the polygon's height without them
//   6. heading     the yaw facing the velocity, when the unit is moving
//
// Units are processed in UNIT ORDER, which `add()` requires to be ascending entity order —
// `simulation-and-determinism`'s tie-breaking rule — and every pass but the grid's counting sort
// can run on job workers with the same result as one thread (each unit writes only its own slot).
//
// THE STATE IS HASHED. `state_hash()` folds every unit's position, height, velocity, heading and
// polygon, in unit order, and is what a lockstep session compares tick by tick.

#include <cy/core/base/expected.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/shapes.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/memory/array.h>
#include <cy/movement/crowd_kernel.h>
#include <cy/movement/height_field.h>
#include <cy/movement/nav_mesh.h>

namespace cy::jobs {
class JobSystem;
}  // namespace cy::jobs

namespace cy::movement {

using detmath::Angle;
using detmath::FixedCapsule2D;
using detmath::FixedCircle;
using detmath::FixedTransform;

/// The authoritative arithmetic of `CrowdKernel`.
struct FixedPolicy {
    using Scalar = Fixed;
    using Vec = FixedVec2;
    using Wide = WideFixed;

    [[nodiscard]] static Wide length_squared(Vec v) noexcept { return detmath::length_squared(v); }
    [[nodiscard]] static Wide square(Scalar s) noexcept { return WideFixed::product(s, s); }
    [[nodiscard]] static Scalar sqrt(Wide w) noexcept { return detmath::sqrt(w); }
    [[nodiscard]] static Vec clamp_length(Vec v, Scalar limit) noexcept {
        return detmath::clamp_length(v, limit);
    }
    /// floor(value / 2^shift): an arithmetic shift of the raw value, exact.
    [[nodiscard]] static i64 cell(Scalar value, int shift) noexcept {
        return value.raw >> (Fixed::kFractionBits + shift);
    }
    [[nodiscard]] static Scalar zero() noexcept { return Fixed::zero(); }
    [[nodiscard]] static Wide wide_zero() noexcept { return WideFixed{}; }
};

/// How the mover runs. Every field is part of the simulation's definition: peers must agree on it,
/// so a session folds it into its state at tick 0.
struct MoverParams {
    /// Ticks per second. `dt` is one over it, truncated.
    i32 tick_rate = 60;
    /// Grid cells are 2^`cell_shift` metres; at least the largest unit diameter. In [0, 16].
    int cell_shift = 1;
    /// The share of an overlap each unit of the pair takes. A half resolves a pair in one pass.
    Fixed separation_share = Fixed::half();
    /// The furthest separation may move a unit in one pass, so a pile-up relaxes over several
    /// ticks rather than scattering in one.
    Fixed max_push = Fixed::from_raw(Fixed::kOneRaw / 4);
    /// Separation passes per tick.
    u32 separation_passes = 1;
    /// The region the grid covers when no navigation mesh is bound. Units outside it are still
    /// separated, binned into the border cells.
    FixedVec2 grid_min{Fixed::from_int(-512), Fixed::from_int(-512)};
    FixedVec2 grid_max{Fixed::from_int(512), Fixed::from_int(512)};
    /// Units moving slower than this keep their heading.
    Fixed heading_speed = Fixed::from_raw(Fixed::kOneRaw / 64);
};

/// A unit, as `add()` takes it.
struct UnitDesc {
    /// The entity the unit is. Units are added in ascending entity order.
    u64 entity = 0;
    FixedVec2 position;
    Fixed radius = Fixed::half();
    Fixed max_speed = Fixed::from_int(4);
    Fixed max_acceleration = Fixed::from_int(16);
};

/// What one step did. Counts, so a budget or a test reads a number rather than a flag.
struct MoverReport {
    u32 units = 0;
    /// Overlapping pairs found by the separation passes, counted once from each side.
    u64 overlaps = 0;
    /// Unit-obstacle contacts pushed out.
    u32 obstacle_contacts = 0;
    /// Units whose move the navigation surface shortened.
    u32 clamped = 0;
};

/// Moves authoritative units one tick at a time in `Fixed`: the passes in the header comment, in
/// unit order, with the same bits on any number of job workers.
class KinematicMover {
public:
    KinematicMover(Allocator& allocator, const MoverParams& params) noexcept;

    KinematicMover(const KinematicMover&) = delete;
    KinematicMover& operator=(const KinematicMover&) = delete;

    /// The world the units stand in. Either may be null: without a mesh units are not clamped,
    /// without heights a unit's height is its polygon's. Bind before the first step; binding snaps
    /// every unit onto the mesh.
    void bind(const FixedNavMesh* mesh, const FixedHeightField* heights) noexcept;

    /// Add a unit. Refuses an entity not above the last one added: the unit order is the entity
    /// order, which is the tie-break every pass relies on.
    [[nodiscard]] Expected<u32, Error> add(const UnitDesc& unit) noexcept;
    /// Stop moving a unit. It keeps its slot, so no other unit's index or order changes.
    void deactivate(u32 unit) noexcept;

    /// A static obstacle, resolved in the order added.
    [[nodiscard]] Status add_obstacle(const FixedCircle& circle) noexcept;
    [[nodiscard]] Status add_obstacle(const FixedCapsule2D& capsule) noexcept;

    void set_desired_velocity(u32 unit, FixedVec2 velocity) noexcept {
        kernel_.desired[unit] = velocity;
    }

    /// One tick. With `jobs`, the per-unit passes run as parallel ranges; the result is the same
    /// bits as without.
    [[nodiscard]] Expected<MoverReport, Error> step(jobs::JobSystem* jobs = nullptr) noexcept;

    [[nodiscard]] u32 size() const noexcept { return kernel_.size(); }
    [[nodiscard]] bool active(u32 unit) const noexcept { return kernel_.active[unit] != 0; }
    [[nodiscard]] u64 entity(u32 unit) const noexcept { return entities_[unit]; }
    [[nodiscard]] FixedVec2 position(u32 unit) const noexcept { return kernel_.position[unit]; }
    [[nodiscard]] FixedVec2 velocity(u32 unit) const noexcept { return kernel_.velocity[unit]; }
    [[nodiscard]] FixedVec2 desired_velocity(u32 unit) const noexcept {
        return kernel_.desired[unit];
    }
    [[nodiscard]] Fixed radius(u32 unit) const noexcept { return kernel_.radius[unit]; }
    [[nodiscard]] Fixed max_speed(u32 unit) const noexcept { return kernel_.max_speed[unit]; }
    [[nodiscard]] Fixed height(u32 unit) const noexcept { return heights_[unit]; }
    [[nodiscard]] Angle heading(u32 unit) const noexcept { return headings_[unit]; }
    [[nodiscard]] FixedPolyIndex poly(u32 unit) const noexcept { return polys_[unit]; }
    /// The unit's placement: (x, height, z), facing its heading.
    [[nodiscard]] FixedTransform transform(u32 unit) const noexcept;

    [[nodiscard]] const MoverParams& params() const noexcept { return params_; }
    [[nodiscard]] Fixed dt() const noexcept { return dt_; }

    /// Every unit's state folded in unit order.
    [[nodiscard]] u64 state_hash() const noexcept;
    /// The parameters folded: what a session puts in its tick-0 state.
    [[nodiscard]] u64 params_hash() const noexcept;

private:
    struct Obstacle {
        FixedCapsule2D shape;  ///< a circle is a capsule whose ends coincide
    };

    void settle(u32 begin, u32 end) noexcept;
    [[nodiscard]] CrowdGridParams grid() const noexcept;

    MoverParams params_;
    Fixed dt_;
    CrowdKernel<FixedPolicy> kernel_;
    Array<u64> entities_;
    Array<Fixed> heights_;
    Array<Angle> headings_;
    Array<FixedPolyIndex> polys_;
    Array<u32> contacts_;
    Array<u8> clamped_;
    Array<Obstacle> obstacles_;
    const FixedNavMesh* mesh_ = nullptr;
    const FixedHeightField* height_field_ = nullptr;
};

/// The yaw that faces `velocity` on the (x, z) plane: the rotation about +Y that takes -Z to it.
[[nodiscard]] Angle heading_of(FixedVec2 velocity) noexcept;

}  // namespace cy::movement
