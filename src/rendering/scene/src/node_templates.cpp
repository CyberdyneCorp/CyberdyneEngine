// The catalogue's renderer half. See cy/rendering/scene/node_templates.h for the argument.

#include <cy/rendering/scene/node_templates.h>

namespace cy::rendering {
namespace {

using scene::NodeTemplateDesc;
using scene::TemplateComponent;

/// The five declarations, their default bytes, and the spans that point at them.
///
/// ONE STATIC OBJECT, because `NodeTemplateDesc::components` is BORROWED — document.h's registry
/// "does not copy the defaults into the registry", so every pointer here has to outlive the
/// `redeclare` call and the world after it. A `constexpr` array would have been the shape
/// src/scene/'s own catalogue uses, and it is not available: `MeshRenderer` holds a
/// `determinism::Presentation<>` whose constructor is not constexpr.
///
/// The values are DEFAULT-CONSTRUCTED STRUCTS rather than hand-written blobs. The point of a
/// template is that a node created from one is indistinguishable from a node an author built by
/// writing the component, and two spellings of "the default" would drift the first time somebody
/// changed a member's initialiser.
struct Catalogue {
    MeshRenderer mesh;
    Camera camera;
    LightSource lights[3];
    TemplateComponent components[5];
    NodeTemplateDesc descriptions[5];

    Catalogue() noexcept {
        lights[0].kind = render::LightKind::Directional;
        // A directional light is measured in lux and a point or a spot in candela, so the intensity
        // the component defaults to is not the right number for the sun. `LightSource`'s own
        // comment states the unit; this states the value, once.
        lights[0].intensity = 100'000.0F;
        lights[0].range = 0.0F;
        lights[1].kind = render::LightKind::Point;
        lights[2].kind = render::LightKind::Spot;

        bind(0, kMeshRendererComponentName, &mesh, sizeof(mesh), scene::kMeshRendererTemplate);
        bind(1, kCameraComponentName, &camera, sizeof(camera), scene::kCameraTemplate);
        bind(2, kLightSourceComponentName, &lights[0], sizeof(lights[0]),
             scene::kDirectionalLightTemplate);
        bind(3, kLightSourceComponentName, &lights[1], sizeof(lights[1]),
             scene::kPointLightTemplate);
        bind(4, kLightSourceComponentName, &lights[2], sizeof(lights[2]),
             scene::kSpotLightTemplate);
    }

private:
    void bind(u32 index, const char* component, const void* value, usize size,
              const char* name) noexcept {
        components[index].component_name = component;
        components[index].defaults = value;
        components[index].defaults_size = static_cast<u32>(size);
        descriptions[index].name = name;
        descriptions[index].components = Span<const TemplateComponent>(&components[index], 1);
        descriptions[index].behaviour = "";
    }
};

[[nodiscard]] const Catalogue& catalogue() noexcept {
    static const Catalogue value;
    return value;
}

}  // namespace

Status declare_render_templates(scene::SceneTree& tree) noexcept {
    const ecs::ComponentRegistry& registry = tree.world().components();
    if (registry.find(kMeshRendererComponentName) == nullptr ||
        registry.find(kLightSourceComponentName) == nullptr ||
        registry.find(kCameraComponentName) == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "the renderer's components are not registered in this world; redeclaring the "
                    "catalogue against it would rebind every template to nothing");
    }

    for (const NodeTemplateDesc& description : catalogue().descriptions) {
        if (Status declared = tree.templates().redeclare(tree.world(), description); !declared) {
            return declared;
        }
    }
    return ok();
}

}  // namespace cy::rendering
