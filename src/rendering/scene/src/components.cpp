// Registering the renderer's components in a world. See cy/rendering/scene/components.h.

#include <cy/core/base/assert.h>
#include <cy/core/reflect/registry.h>
#include <cy/rendering/scene/components.h>

#include <cy/rendering/scene/components.reflect.h>
#include <cy_reflect_generated_rendering.h>

#include <string_view>

namespace cy::rendering {
namespace {

/// A reflected component: the route all three take since M8.b's task 11.3.
///
/// `name` is checked rather than used — the registry takes the name from the `TypeInfo`, and the
/// constant beside the struct is what a serialized stream and every test spell, so if the two ever
/// disagreed a component would be registered under a name nothing looks it up by. src/scene/'s own
/// `bind_reflected` makes the same check for the same reason.
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

Expected<RenderComponents, Error> RenderComponents::register_all(World& world) noexcept {
    RenderComponents ids;

    // The reflected descriptors reach the process-wide registry HERE rather than in a start-up
    // path, for the reason src/scene/src/components.cpp gives: this is the point at which they are
    // first needed, and `TypeRegistry::add` is idempotent, so a second world costs one probe each.
    // Without it a caller that looks `cy::rendering::MeshRenderer` up by name — which is what
    // `build_authoring_schema` and therefore `resolve_against` do — finds nothing, and an authored
    // mesh reaches no renderer. That was the state at M8.a.
    if (Status registered = reflect::register_rendering_types(); !registered) {
        return make_unexpected(registered.error());
    }

    // The order is the id order and therefore the order a serialized world's descriptor table is
    // written in. Fixed deliberately, so two worlds that both run this function agree on every
    // number; nothing else depends on it.
    if (Status bound =
            bind_reflected<MeshRenderer>(world, kMeshRendererComponentName, ids.mesh_renderer);
        !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound =
            bind_reflected<LightSource>(world, kLightSourceComponentName, ids.light_source);
        !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind_reflected<Camera>(world, kCameraComponentName, ids.camera); !bound) {
        return make_unexpected(bound.error());
    }
    return ids;
}

}  // namespace cy::rendering
