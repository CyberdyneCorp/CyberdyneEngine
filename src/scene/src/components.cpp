// Registering the scene layer's components in a world. Tasks 3.1.1, 3.1.3, 3.1.5.

#include <cy/scene/components.h>

namespace cy::scene {
namespace {

/// One registration, spelled once. Every scene component is either an ordinary data component or a
/// zero-sized tag, so the two calls below are the whole of the registration surface — and keeping
/// them in one place is what lets `register_all` read as a list rather than as thirteen copies of
/// the same four lines.
[[nodiscard]] Status bind_data(World& world, const char* name, u32 size, u32 alignment,
                               ComponentTypeId& out) noexcept {
    Expected<ComponentTypeId, Error> id =
        world.components().register_builtin(name, size, alignment);
    if (!id) {
        return make_unexpected(id.error());
    }
    out = *id;
    return ok();
}

[[nodiscard]] Status bind_tag(World& world, const char* name, ComponentTypeId& out) noexcept {
    ecs::ComponentOptions options;
    options.kind = ecs::ComponentKind::Tag;
    Expected<ComponentTypeId, Error> id = world.components().register_builtin(name, 0, 1, options);
    if (!id) {
        return make_unexpected(id.error());
    }
    out = *id;
    return ok();
}

template <class T>
[[nodiscard]] Status bind(World& world, const char* name, ComponentTypeId& out) noexcept {
    return bind_data(world, name, static_cast<u32>(sizeof(T)), static_cast<u32>(alignof(T)), out);
}

}  // namespace

Expected<SceneComponents, Error> SceneComponents::register_all(World& world) noexcept {
    SceneComponents ids;

    // The order is the id order and therefore the order a snapshot's descriptor table is written
    // in; it is fixed deliberately, so two worlds that both run this function agree on every
    // number. Nothing else depends on it.
    struct Binding {
        Status (*bind)(World&, const char*, ComponentTypeId&) noexcept;
        const char* name;
        ComponentTypeId SceneComponents::* field;
    };

    const Binding bindings[] = {
        {&bind<NodeName>, kNodeNameComponentName, &SceneComponents::node_name},
        {&bind<NodeAlias>, kNodeAliasComponentName, &SceneComponents::node_alias},
        {&bind<ChildOrder>, kChildOrderComponentName, &SceneComponents::child_order},
        {&bind<LocalTransform>, kLocalTransformComponentName, &SceneComponents::local_transform},
        {&bind<WorldTransform>, kWorldTransformComponentName, &SceneComponents::world_transform},
        {&bind<InterpolatedTransform>, kInterpolatedTransformComponentName,
         &SceneComponents::interpolated_transform},
        {&bind<NodeFlags>, kNodeFlagsComponentName, &SceneComponents::flags},
        {&bind<NodeState>, kNodeStateComponentName, &SceneComponents::state},
        {&bind<SceneRef>, kSceneRefComponentName, &SceneComponents::scene_ref},
        {&bind<BehaviourRef>, kBehaviourRefComponentName, &SceneComponents::behaviour_ref},
        {&bind_tag, kHiddenComponentName, &SceneComponents::hidden},
        {&bind_tag, kDisabledComponentName, &SceneComponents::disabled},
    };

    for (const Binding& binding : bindings) {
        if (Status bound = binding.bind(world, binding.name, ids.*binding.field); !bound) {
            return make_unexpected(bound.error());
        }
    }
    return ids;
}

}  // namespace cy::scene
