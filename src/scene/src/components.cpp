// Registering the scene layer's components in a world. Tasks 3.1.1, 3.1.3, 3.1.5, and M5's 1.3.
//
// TWO ROUTES, AND WHICH COMPONENT TAKES WHICH IS THE WHOLE OF THIS FILE'S DESIGN.
//
//   register_reflected()  seven of the twelve. The descriptor is the generated `TypeInfo`, the key
//                         is the manifest identifier, and everything that reads reflection — the
//                         state hash, the serializer, the ABI's field import, the editor's
//                         generated inspector — sees the component's fields.
//   register_builtin()    the other five, each for a reason components.h states at the declaration:
//                         a `cy::Name` is a private intern-table index, a `Presentation<>` value is
//                         behind an accessor rather than in a member, and a tag has no bytes.
//
// THE ID ORDER IS UNCHANGED, and that is load-bearing rather than tidy: the order below is the id
// order, the id order is what a snapshot's descriptor table is written in, and two worlds that both
// run this function have to agree on every number. Switching a component from one route to the
// other therefore happens IN PLACE — the route changes, the position does not.

#include <cy/scene/components.h>

// The generated `Reflected<T>` specialisations, and the aggregate that registers them.
#include <cy/scene/components.reflect.h>
#include <cy_reflect_generated_scene.h>

#include <string_view>

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

/// A reflected component: the route every describable one takes.
///
/// `name` is checked rather than used. The registry takes the name from the `TypeInfo`, and the
/// constant beside the struct is what a serialized stream and every test spell — so if the two ever
/// disagreed, a component would be registered under a name nothing looks it up by. Asserting is
/// free here and the failure it prevents is silent.
template <class T>
[[nodiscard]] Status bind_reflected(World& world, const char* name, ComponentTypeId& out) noexcept {
    const reflect::TypeInfo& info = reflect::type_of<T>();
    CY_ASSERT_MSG(std::string_view(info.name) == std::string_view(name),
                  "a reflected component's TypeInfo name is the name it is registered under");
    Expected<ComponentTypeId, Error> id = world.components().register_reflected(info);
    if (!id) {
        return make_unexpected(id.error());
    }
    out = *id;
    return ok();
}

}  // namespace

Expected<SceneComponents, Error> SceneComponents::register_all(World& world) noexcept {
    SceneComponents ids;

    // The reflected descriptors reach the process-wide registry here rather than in an engine
    // start-up path, because THIS is the point at which they are first needed and because
    // `TypeRegistry::add` is idempotent — the same descriptor registered twice is `ok()`, so a
    // second world, a second `SceneTree`, or a test that builds ten worlds all cost one probe each.
    // Without it a caller that looks a scene component up by `TypeId` finds nothing, which is the
    // half of reflection an inspector uses and a registration alone does not provide.
    if (Status registered = reflect::register_scene_types(); !registered) {
        return make_unexpected(registered.error());
    }

    // The order is the id order and therefore the order a snapshot's descriptor table is written
    // in; it is fixed deliberately, so two worlds that both run this function agree on every
    // number. Nothing else depends on it.
    struct Binding {
        Status (*bind)(World&, const char*, ComponentTypeId&) noexcept;
        const char* name;
        ComponentTypeId SceneComponents::* field;
    };

    const Binding bindings[] = {
        // By name: a `cy::Name` is a private index into a process-wide intern table.
        {&bind<NodeName>, kNodeNameComponentName, &SceneComponents::node_name},
        {&bind<NodeAlias>, kNodeAliasComponentName, &SceneComponents::node_alias},
        // Reflected.
        {&bind_reflected<ChildOrder>, kChildOrderComponentName, &SceneComponents::child_order},
        {&bind_reflected<LocalTransform>, kLocalTransformComponentName,
         &SceneComponents::local_transform},
        {&bind_reflected<WorldTransform>, kWorldTransformComponentName,
         &SceneComponents::world_transform},
        // By name: its fields are `Presentation<>`, whose value is behind an accessor.
        {&bind<InterpolatedTransform>, kInterpolatedTransformComponentName,
         &SceneComponents::interpolated_transform},
        // Reflected.
        {&bind_reflected<NodeFlags>, kNodeFlagsComponentName, &SceneComponents::flags},
        {&bind_reflected<NodeState>, kNodeStateComponentName, &SceneComponents::state},
        {&bind_reflected<SceneRef>, kSceneRefComponentName, &SceneComponents::scene_ref},
        {&bind_reflected<BehaviourRef>, kBehaviourRefComponentName,
         &SceneComponents::behaviour_ref},
        // Tags: no bytes, so nothing to describe.
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
