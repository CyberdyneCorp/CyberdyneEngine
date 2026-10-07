// SPDX-License-Identifier: MIT
#pragma once
// The authoritative transform of a unit, and the presentation sync that derives the scene's from
// it. openspec/changes/add-deterministic-math, design §7.2 and §9.2, task 6.1.
//
// TWO TRANSFORMS, ONE DIRECTION. In a `Lockstep` session a unit's `AuthoritativeTransform` — a
// `FixedTransform` — is the state that is simulated, hashed and snapshotted. The scene's
// `LocalTransform` stays `f32` and becomes DERIVED: once per tick, after the mover has stepped,
// `sync_presentation()` converts each authoritative transform relative to a camera origin
// (`detmath::to_f32_relative`, design §7.2) and writes it into the node, which marks it for
// propagation; propagation then rolls `InterpolatedTransform` forward, so rendering interpolates
// between ticks exactly as it does for any other node. Nothing reads the `f32` value back into the
// simulation — the authoritative side never names the scene at all.
//
//   KinematicMover --publish_units()--> AuthoritativeTransform --sync_presentation()-->
//   LocalTransform
//        Fixed                                Fixed (hashed)                               f32
//        (derived)
//
// presentation.cpp is the module's one float translation unit that runs every tick, and it runs on
// the presentation side of that line.

#include <cy/core/base/expected.h>
#include <cy/core/detmath/vec.h>
#include <cy/ecs/world.h>
#include <cy/movement/mover.h>

namespace cy::scene {
class SceneTree;
}  // namespace cy::scene

namespace cy::movement {

/// A unit's authoritative placement in a `Lockstep` session.
struct AuthoritativeTransform {
    detmath::FixedTransform value;
};

inline constexpr const char* kAuthoritativeTransformComponentName =
    "cy::movement::AuthoritativeTransform";

/// Register `AuthoritativeTransform` in `world`. Idempotent by name.
[[nodiscard]] Expected<ecs::ComponentTypeId, Error> register_authoritative_transform(
    ecs::World& world) noexcept;

/// Write every active unit's transform into its entity's `AuthoritativeTransform`. A unit whose
/// entity is dead or lacks the component is counted, not created: adding a component is a
/// structural change, which belongs to whoever spawns units. Returns how many were written.
struct PublishReport {
    u32 written = 0;
    u32 skipped = 0;
};
[[nodiscard]] Expected<PublishReport, Error> publish_units(const KinematicMover& mover,
                                                           ecs::World& world,
                                                           ecs::ComponentTypeId component) noexcept;

/// What one presentation sync did.
struct PresentationSyncReport {
    u32 synced = 0;
    /// Entities with an authoritative transform and no scene node: nothing to present.
    u32 without_node = 0;
};

/// Derive every scene node's `LocalTransform` from its entity's `AuthoritativeTransform`, relative
/// to `origin` (the camera's position, so the result is camera-relative, which is what core-math's
/// large-world rule asks rendering for). The rotation's components convert one by one. Expects each
/// such node to be a root, as an RTS unit is: its `LocalTransform` is its world placement.
[[nodiscard]] Expected<PresentationSyncReport, Error> sync_presentation(
    scene::SceneTree& tree, ecs::ComponentTypeId component, detmath::FixedVec3 origin) noexcept;

}  // namespace cy::movement
