// SPDX-License-Identifier: MIT
#pragma once
// PARTICLE COLLISION: the contact query and the response, as the CPU path computes them.
//
// ================================================================================================
// A CONTACT IS A QUERY; A RESPONSE IS ARITHMETIC OVER IT
// ================================================================================================
//
// `vfx-system`: "Particles SHALL support collision against: analytic primitives (plane, sphere,
// box, capsule), the scene signed distance field, the depth buffer in screen space, and physics
// queries for the CPU path", and "Collision response SHALL support: bounce with restitution,
// friction, sliding, kill, and raising a GPU event."
//
// The IR spells that as four operations (`ir.h`): `Contact` asks the world one question —
// `float4(normal, separation)` — and `Bounce`, `Resolve` and `Impact` are pure functions of a
// particle's state and that answer. Kill and the event are NOT operations: they are the kernel's
// existing kill root and event raise, wired to the contact's `began` output. So an author who wants
// sparks to die on the floor writes `kill_if(collide.contact)`, and one who wants impact debris
// writes `emit_event(collide.impact)` — the graph says what happens rather than a response enum.
//
// ================================================================================================
// EACH MODE'S ACCURACY, COST AND BLIND SPOT — the specification requires all three be stated
// ================================================================================================
//
//   plane, sphere, box, capsule   exact, a handful of flops, and no knowledge of the scene at all.
//                                 Authored in the effect's own space, so they move with it.
//   sdf                           the GI distance field's clipmaps: world space, so geometry off
//                                 screen and behind the camera is solid. Accurate to the field's
//                                 voxel (a quarter metre at level 0 by default), one field sample
//                                 when clear and six more (four at quality 2) for the normal when
//                                 touching. A thin wall finer than a voxel can be missed.
//   depth                         the frame's depth buffer: five texels a particle on screen
//                                 (three at quality 2), none off it. CHEAP AND BLIND — it cannot
//                                 see anything off screen, behind another surface, or behind the
//                                 camera, so a particle passes through all of those. That is the
//                                 mode working as specified, not a defect; use `sdf` where it
//                                 matters. A surface is taken to be `thickness` deep behind its
//                                 depth, and a particle further behind than that is not touching
//                                 it — which is what lets one pass behind a pillar.
//
// "Physics queries for the CPU path" is the `physics_query` data interface a graph samples, and
// physics' own analytic shapes reach a particle as the parameters of a `vfx.collide.<shape>` node
// a host writes from the body — the same four primitives.
//
// ================================================================================================
// THE BUDGET CONTROLLER'S LEVER
// ================================================================================================
//
// `BudgetLevers::collision_quality`: 3 is authored; 2 takes the cheaper normals (four field
// samples instead of six, forward depth differences instead of central ones); 1 keeps only the
// analytic primitives; 0 turns collision off. `CollisionInputs::quality` is where it arrives, on
// both paths, and a contact that quality turns off reports `kNoContact`.
//
// ================================================================================================
// TWO SPELLINGS, CHECKED AGAINST EACH OTHER
// ================================================================================================
//
// Everything below is written a second time as Slang in `emit_slang.cpp`'s collision prelude, in
// the same order, because the GPU path cannot call C++. `render.vfx_gpu` runs one effect through
// both and compares them particle by particle; a reordering that is equal in exact arithmetic is
// a test failure there, which is the point of it.

#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/vfx/ir.h>

namespace cy::vfx {

/// The scene signed distance field as the CPU path reads it. A host adapts its field — the GI
/// clipmaps, `gi::DistanceField::distance` — to this; `cy::vfx` names no renderer.
class CollisionField {
public:
    CollisionField() = default;
    virtual ~CollisionField();

    CollisionField(const CollisionField&) = delete;
    CollisionField& operator=(const CollisionField&) = delete;
    CollisionField(CollisionField&&) = delete;
    CollisionField& operator=(CollisionField&&) = delete;

    /// Metres to the nearest surface from a WORLD-space point, positive outside.
    [[nodiscard]] virtual f32 distance(Vec3 point) const noexcept = 0;
};

/// The depth buffer a screen-space contact reads.
struct DepthCollisionView {
    /// World to clip, and back. The frame's own — jitter included, if the frame jittered.
    Mat4 view_projection = Mat4::identity();
    Mat4 inverse_view_projection = Mat4::identity();
    /// Where the camera is, in world space: a surface's normal is turned to face it.
    Vec3 camera{0.0F, 0.0F, 0.0F};
    u32 width = 0;
    u32 height = 0;
    /// Reversed-Z device depth, a row at a time from the TOP of the picture — the frame's own
    /// orientation, `ndc.y = +1` at row zero. Zero is the far plane: sky, nothing to touch. The CPU
    /// path reads this; the GPU path binds the frame's depth texture instead.
    Span<const f32> depths;
};

/// What a contact may read, and how hard it may try.
struct CollisionInputs {
    /// Null: an `sdf` contact finds nothing.
    const CollisionField* field = nullptr;
    /// Null: a `depth` contact finds nothing.
    const DepthCollisionView* depth = nullptr;
    /// Added to a particle's position before a world-space query: the effect instance's origin,
    /// since a particle's position is stored relative to it. Analytic primitives are authored in
    /// the effect's own space and do not use it.
    Vec3 origin{0.0F, 0.0F, 0.0F};
    /// `BudgetLevers::collision_quality`. See the note at the top of this file.
    u8 quality = 3;
};

/// `Contact`: `float4(normal, separation)`. See `ColliderShape` in ir.h for what `a` and `b` mean.
[[nodiscard]] Vec4 collision_contact(ColliderShape shape, Vec3 position, f32 radius, Vec4 a, Vec4 b,
                                     const CollisionInputs& inputs) noexcept;

/// `Bounce`: the velocity after a contact. `response` is `(restitution, friction, stick,
/// rest_speed)`.
///
/// Touching and closing: the normal component reverses and is scaled by the restitution — zero
/// below `rest_speed`, so a resting particle rests instead of chattering — and the tangential
/// component loses at most `friction` times the normal impulse (Coulomb), which is what makes a
/// particle on a slope SLIDE when the friction is low and stop when it is high. `stick` above one
/// half stops the particle dead on contact. Not touching, or already leaving: unchanged.
[[nodiscard]] Vec3 collision_bounce(Vec3 velocity, Vec4 contact, Vec4 response) noexcept;

/// `Resolve`: an overlapping particle moved out along the normal to separation zero.
[[nodiscard]] Vec3 collision_resolve(Vec3 position, Vec4 contact) noexcept;

/// `Impact`: the closing speed along the normal while touching, zero otherwise.
[[nodiscard]] f32 collision_impact(Vec3 velocity, Vec4 contact) noexcept;

}  // namespace cy::vfx
