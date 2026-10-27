// SPDX-License-Identifier: MIT
#pragma once
// Foot placement on uneven ground: two-bone legs reaching the ground under each foot, the hips
// lowered so the lower foot can reach it, each foot turned to the surface it stands on, and all of
// it followed smoothly rather than snapped. Issue #92.
//
// ================================================================================================
// THE GROUND IS THE HOST'S
// ================================================================================================
//
// `animation-and-skinning` keeps the runtime backend-independent: it names no physics server, no
// terrain and no world. Foot placement needs all three — "is there ground under this foot, and
// which way does it face" — so it ASKS: `GroundProbe` is a downward ray the host answers, from
// physics (`cy::animation-ground`'s `PhysicsGroundProbe`), from terrain (`TerrainGroundProbe`), or
// from a plane in a test or an editor preview (`PlaneGround` below). The solver never sees which.
//
// A probe is called from job workers, several at once, during `Stage::Animation`: an
// implementation must be safe to call concurrently and must not mutate the world. Physics queries
// already are (`physics` — "Parallel queries").
//
// ================================================================================================
// WHAT THE SOLVE DOES, IN ORDER
// ================================================================================================
//
//   1. Each ankle's animated position is taken to the world through the character's placement and
//      a ray is cast down through it, from `probe_above` over it to `probe_below` under it.
//   2. A foot's OFFSET is how far the ground under it is above the character's own ground plane
//      (model y = 0). The animated lift of a foot in its swing is kept: a foot the clip raises
//      10 cm is raised 10 cm above the step, not flattened onto it.
//   3. The PELVIS drops by the lowest offset (and rises by it when both feet are above the
//      character's plane), within `max_pelvis_drop` and `max_foot_raise`, so the leg over the lower
//      ground can reach it without straightening past its length.
//   4. Each leg is solved by `solve_two_bone` to its ankle's animated position plus its offset,
//      bending towards `pole`.
//   5. Each foot is turned from the character's up to the ground's normal, at most `max_tilt`.
//
// Offsets and normals follow their targets with a time constant (`smoothing`, seconds) over the
// SIMULATION time since the previous solve, so the result is a function of the ticks and not of
// the frame rate. The first solve snaps.
//
// WEIGHT. Zero leaves the pose untouched, bit for bit — the same bytes as a graph with no foot
// placement node at all — which is the "off" a game fades from.

#include <cy/animation/ik.h>
#include <cy/animation/skeleton.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>

namespace cy::animation {

/// One downward probe.
struct GroundQuery {
    /// Where the ray starts, in the world.
    Vec3 origin{0.0F, 0.0F, 0.0F};
    /// How far down it reaches, in metres.
    f32 length = 1.0F;
    /// The identity of whoever is asking — an entity's bits — so a probe can ignore the
    /// character's own capsule. Zero when there is none.
    u64 owner = 0;
};

/// What a probe found below: whether there is ground, where, and which way it faces.
struct GroundHit {
    bool hit = false;
    Vec3 point{0.0F, 0.0F, 0.0F};
    /// Unit length, pointing away from the ground.
    Vec3 normal{0.0F, 1.0F, 0.0F};
};

/// The ground, as the host answers for it. Thread-safe: see the header.
class GroundProbe {
public:
    GroundProbe() = default;
    virtual ~GroundProbe() = default;
    GroundProbe(const GroundProbe&) = delete;
    GroundProbe& operator=(const GroundProbe&) = delete;
    GroundProbe(GroundProbe&&) = delete;
    GroundProbe& operator=(GroundProbe&&) = delete;

    [[nodiscard]] virtual GroundHit cast_down(const GroundQuery& query) const noexcept = 0;
};

/// An infinite plane through `point` facing `normal`: the ground of a test and of the editor's
/// preview, where there is no physics world.
class PlaneGround final : public GroundProbe {
public:
    PlaneGround(Vec3 point, Vec3 normal) noexcept;
    [[nodiscard]] GroundHit cast_down(const GroundQuery& query) const noexcept override;

private:
    Vec3 point_;
    Vec3 normal_;
};

/// A resolved foot placement setup: the joints by index, for one skeleton.
struct FootPlacementSetup {
    u16 pelvis = kInvalidJoint;
    /// Per leg: the two-bone chain from the hip to the ankle. `pole` is the knees' direction.
    IkChain legs[2];
    f32 ankle_height = 0.08F;
    f32 probe_above = 0.5F;
    f32 probe_below = 0.6F;
    f32 max_pelvis_drop = 0.35F;
    f32 max_foot_raise = 0.45F;
    f32 smoothing = 0.08F;
    f32 max_tilt = 0.6F;
    [[nodiscard]] bool valid() const noexcept {
        return pelvis != kInvalidJoint && legs[0].valid() && legs[1].valid();
    }
};

/// What a solve followed, per character: the smoothed offsets and normals. Zero-initialised is
/// "never solved", and the next solve snaps.
struct FootPlacementState {
    f32 pelvis = 0.0F;
    f32 offset[2] = {0.0F, 0.0F};
    Vec3 normal[2] = {Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}};
    bool primed = false;
};

/// What one solve did, for a test, a debug view and `animation_foot_placement` in the ABI.
struct FootPlacementReport {
    bool solved = false;
    /// The hips' vertical offset applied, in metres (model space).
    f32 pelvis = 0.0F;
    bool hit[2] = {false, false};
    /// The ground under each foot, in the world.
    Vec3 ground[2] = {Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 0.0F}};
    Vec3 normal[2] = {Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}};
    /// Each foot's vertical offset applied, in metres (model space).
    f32 offset[2] = {0.0F, 0.0F};
    /// Whether each leg could reach its target.
    bool reached[2] = {false, false};
};

/// Where the character is and how long since the previous solve.
struct FootPlacementContext {
    /// Model space to the world: the character's placement.
    Transform model_to_world;
    /// Simulation seconds since the previous solve of this character. Zero snaps nothing and
    /// moves nothing towards a new target.
    f32 elapsed = 0.0F;
    /// Passed to the probe as `GroundQuery::owner`.
    u64 owner = 0;
};

/// Place the feet of the pose `local` on the ground `probe` answers for, weighted by `weight`.
/// See the header for the steps. Refused for a setup that does not name a pelvis and two legs of
/// this skeleton; a foot with no ground under it is left where the animation put it.
[[nodiscard]] Status solve_foot_placement(const Skeleton& skeleton, const FootPlacementSetup& setup,
                                          const FootPlacementContext& context,
                                          const GroundProbe& probe, f32 weight,
                                          FootPlacementState& state, Span<Transform> local,
                                          FootPlacementReport* report = nullptr) noexcept;

}  // namespace cy::animation
