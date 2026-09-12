#pragma once
// Buoyancy from MULTIPLE SAMPLE POINTS, on the same authoritative displacement the renderer draws.
// M10 task 2.3.
//
// `water` — "Buoyancy and physics interaction": "The engine SHALL provide buoyancy through the
// physics interface, computed from submerged volume or from MULTIPLE SAMPLE POINTS on a body, not
// from a single centre point. Buoyancy SHALL include: displacement force, linear and angular drag
// through water, and the effect of the surface velocity and current, so a boat is carried by a
// river as well as floated by it. Buoyancy SHALL use the SAME AUTHORITATIVE DISPLACEMENT as
// rendering."
//
// --- WHY THIS LIVES IN water/ AND NOT IN physics/ ----------------------------------------------
//
// "Through the physics interface" is about where the force is APPLIED. What is computed here is the
// force, from samples the caller supplies and water the caller queried — no rigid body, no solver,
// no physics world. A physics bridge calls `compute_buoyancy()` and applies the result; a boat
// controller, an editor preview and a test can call it too, and all four get the same number.
// Putting the arithmetic behind the physics interface would have made "the same authoritative
// displacement as rendering" unverifiable without a simulation running.
//
// --- WHY A CENTRE POINT IS NOT ENOUGH, MEASURED RATHER THAN ASSERTED ---------------------------
//
// A single sample yields a force through the centre of mass and therefore NO TORQUE: a vessel
// longer than the wavelength translates rigidly up and down instead of pitching. The specification
// names that outcome, and `BuoyancyResult::torque` is what makes it checkable — the suite floats a
// hull longer than the swell and requires a torque that a centre-point solve could not produce.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/water/displacement.h>
#include <cy/water/query.h>

namespace cy::water {

/// One point of a hull. `offset` is in the floating body's own frame; `volume` is the volume this
/// point stands for, so the sum over a hull is the hull's displaced volume when fully submerged.
struct BuoyancySample {
    Vec3 offset{0.0F, 0.0F, 0.0F};
    f32 volume = 0.0F;
};

/// The floating object, as buoyancy needs to see it.
struct BuoyancyState {
    /// The body's origin, absolute metres.
    world::WorldVec3d position;
    /// Linear velocity, m/s, and angular velocity, rad/s, both in world axes.
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    Vec3 angular_velocity{0.0F, 0.0F, 0.0F};
};

/// The coefficients. Declared per object because a barge and a dinghy drag differently, and
/// because a project tuning them should not be editing this module.
struct BuoyancyParams {
    /// How far below the surface a sample is fully submerged, metres. Without it a sample crosses
    /// from no force to full force in one step and the hull rings; with it the transition is the
    /// smooth one a partially submerged volume actually has.
    f32 submersion_softness = 0.25F;
    /// Linear drag against the water, per unit volume, per (m/s).
    f32 linear_drag = 220.0F;
    /// Angular drag, per unit volume, per (rad/s).
    f32 angular_drag = 90.0F;
    /// How strongly the water's own velocity carries the object. One is "moves with the current".
    f32 current_coupling = 1.0F;
};

/// What the solve produced.
struct BuoyancyResult {
    /// Total force, N, in world axes — displacement, drag and current together.
    Vec3 force{0.0F, 0.0F, 0.0F};
    /// Total torque about the body's origin, N m. Non-zero exactly when the samples are submerged
    /// unequally, which is what makes a long hull pitch.
    Vec3 torque{0.0F, 0.0F, 0.0F};
    /// The displacement force alone, before drag and current. Reported because it is the term a
    /// designer compares against the object's weight to see whether it floats.
    Vec3 displacement_force{0.0F, 0.0F, 0.0F};
    /// The mean velocity of the water over the submerged samples, m/s. What carries a raft
    /// downstream, and what a diagnostic prints when it does not.
    Vec3 water_velocity{0.0F, 0.0F, 0.0F};
    f32 submerged_volume = 0.0F;
    u32 submerged_samples = 0;
    u32 samples = 0;
};

/// Compute the buoyancy of one object.
///
/// `water` is the sample at each of `samples`, in the same order, and it must have been taken with
/// the AUTHORITATIVE selection — which `WaterSystem::query()` is, always, for every caller. The two
/// spans must be the same length; a mismatch is refused rather than truncated.
[[nodiscard]] Expected<BuoyancyResult, Error> compute_buoyancy(
    const BuoyancyState& state, Span<const BuoyancySample> samples, Span<const WaterSample> water,
    const BuoyancyParams& params) noexcept;

/// Where each hull sample is in the world, so a caller can query the water at them. A free function
/// rather than something `compute_buoyancy()` does internally, because the caller owns the
/// orientation: an unrotated hull is `position + offset` and a rotated one is the caller's own
/// transform applied first, and this module refuses to own a second convention for rotations.
[[nodiscard]] Status hull_positions(const BuoyancyState& state, Span<const BuoyancySample> samples,
                                    Span<world::WorldVec3d> out) noexcept;

}  // namespace cy::water
