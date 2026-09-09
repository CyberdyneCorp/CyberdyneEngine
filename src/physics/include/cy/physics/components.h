#pragma once
// The physics components' registration in an ECS world. M8.a task 4.1.
//
// `physics` — "Physics components": `RigidBody`, `StaticBody`, `KinematicBody`, `Collider`,
// `CharacterController`, `Constraint`, `Trigger` and `PhysicsMaterial`, "expressed as ECS
// components".
//
// ================================================================================================
// WHY THE LAYOUTS ARE AT LAYER 2 AND THE REGISTRATION IS HERE
// ================================================================================================
//
// `cy/servers/physics/components.h` says it, and this file is the other half it names: "A component
// is two things: a layout, and a registration in a world. The LAYOUT ... belongs beside the server
// whose vocabulary it is written in ... The REGISTRATION belongs with whoever owns the world ... It
// is `src/physics/` at layer 4, mirroring `src/rendering/scene/`, and it does not exist yet."
//
// It exists now. Nothing about the layouts moved: `RigidBody` is still declared at layer 2 in
// physics' vocabulary, and this header does not restate a single field of it. What is added is the
// eight `ComponentTypeId`s one world assigns them, which is exactly what
// `cy::rendering::RenderComponents` is for the renderer's three.
//
// ================================================================================================
// THE ORDER IS FIXED, AND IT IS THE SERIALIZED DESCRIPTOR TABLE'S ORDER
// ================================================================================================
//
// The same argument `RenderComponents::register_all` makes: two worlds that both run this function
// agree on every number, so a world serialized by one and read by another does not need a
// correspondence table for the ids. Nothing else depends on the order, and it must not change
// without the format change that would go with it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/ecs/world.h>
#include <cy/servers/physics/components.h>

namespace cy::physics {

using ecs::ComponentTypeId;
using ecs::Entity;
using ecs::kInvalidComponent;
using ecs::World;

/// Physics' component ids in ONE world.
///
/// Ids are per world (`ecs-core`), so this is a value the bridge holds and never a static. Two
/// worlds in one process assign different numbers to `RigidBody` and neither is wrong.
struct PhysicsComponents {
    ComponentTypeId rigid_body = kInvalidComponent;
    ComponentTypeId static_body = kInvalidComponent;
    ComponentTypeId kinematic_body = kInvalidComponent;
    ComponentTypeId collider = kInvalidComponent;
    ComponentTypeId trigger = kInvalidComponent;
    ComponentTypeId material = kInvalidComponent;
    ComponentTypeId joint = kInvalidComponent;
    ComponentTypeId character_body = kInvalidComponent;

    /// Register all eight in `world`, in this order, and return the ids.
    ///
    /// Idempotent: `ComponentRegistry::register_builtin` returns the existing id for a name it has
    /// already seen, so a second bridge over one world binds to the same numbers rather than
    /// registering a second set.
    [[nodiscard]] static Expected<PhysicsComponents, Error> register_all(World& world) noexcept;

    [[nodiscard]] bool registered() const noexcept {
        return rigid_body != kInvalidComponent && static_body != kInvalidComponent &&
               kinematic_body != kInvalidComponent && collider != kInvalidComponent &&
               trigger != kInvalidComponent && material != kInvalidComponent &&
               joint != kInvalidComponent && character_body != kInvalidComponent;
    }

    /// Which of the three body components `entity` carries, if any.
    ///
    /// One place that answers it, because "does this entity have a body" is asked by body creation,
    /// by the removal sweep and by every test, and three spellings of it would disagree the day a
    /// fourth body kind arrives.
    [[nodiscard]] ComponentTypeId body_component_of(const World& world,
                                                    Entity entity) const noexcept;
};

}  // namespace cy::physics
