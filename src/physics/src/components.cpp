// Registering physics' components in a world. See cy/physics/components.h.

#include <cy/physics/components.h>

namespace cy::physics {
namespace {

template <class T>
[[nodiscard]] Status bind(World& world, const char* name, ComponentTypeId& out) noexcept {
    Expected<ComponentTypeId, Error> id = world.components().register_builtin(
        name, static_cast<u32>(sizeof(T)), static_cast<u32>(alignof(T)));
    if (!id) {
        return make_unexpected(id.error());
    }
    out = *id;
    return ok();
}

}  // namespace

Expected<PhysicsComponents, Error> PhysicsComponents::register_all(World& world) noexcept {
    PhysicsComponents ids;

    // The order is the id order and therefore the order a serialized world's descriptor table is
    // written in. Fixed deliberately; see the header.
    if (Status bound = bind<RigidBody>(world, kRigidBodyComponentName, ids.rigid_body); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<StaticBody>(world, kStaticBodyComponentName, ids.static_body); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<KinematicBody>(world, kKinematicBodyComponentName, ids.kinematic_body);
        !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<Collider>(world, kColliderComponentName, ids.collider); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<Trigger>(world, kTriggerComponentName, ids.trigger); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<PhysicsMaterial>(world, kPhysicsMaterialComponentName, ids.material);
        !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<Joint>(world, kJointComponentName, ids.joint); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<CharacterBody>(world, kCharacterBodyComponentName, ids.character_body);
        !bound) {
        return make_unexpected(bound.error());
    }
    return ids;
}

ComponentTypeId PhysicsComponents::body_component_of(const World& world,
                                                     Entity entity) const noexcept {
    // The order is the precedence: an entity carrying two body components is an authoring mistake,
    // and answering it deterministically is what keeps the mistake reproducible. `physics` has one
    // body per entity because a solver has one body per transform.
    if (world.has(entity, rigid_body)) {
        return rigid_body;
    }
    if (world.has(entity, static_body)) {
        return static_body;
    }
    if (world.has(entity, kinematic_body)) {
        return kinematic_body;
    }
    return kInvalidComponent;
}

}  // namespace cy::physics
