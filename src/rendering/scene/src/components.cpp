// Registering the renderer's components in a world. See cy/rendering/scene/components.h.

#include <cy/rendering/scene/components.h>

namespace cy::rendering {
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

Expected<RenderComponents, Error> RenderComponents::register_all(World& world) noexcept {
    RenderComponents ids;

    // The order is the id order and therefore the order a serialized world's descriptor table is
    // written in. Fixed deliberately, so two worlds that both run this function agree on every
    // number; nothing else depends on it.
    if (Status bound = bind<MeshRenderer>(world, kMeshRendererComponentName, ids.mesh_renderer);
        !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<LightSource>(world, kLightSourceComponentName, ids.light_source);
        !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<Camera>(world, kCameraComponentName, ids.camera); !bound) {
        return make_unexpected(bound.error());
    }
    return ids;
}

}  // namespace cy::rendering
