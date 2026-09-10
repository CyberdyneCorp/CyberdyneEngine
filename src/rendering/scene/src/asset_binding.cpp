// An authored asset reference becomes a renderer handle.
// See cy/rendering/scene/asset_binding.h for why this takes a resolver and loads nothing.

#include <cy/rendering/scene/asset_binding.h>

#include <cy/ecs/query.h>

#include <utility>

namespace cy::rendering {
namespace {

/// One component's mesh reference, resolved. Split out so the chunk loop below stays a loop: the
/// three outcomes — nil, resolved, unresolved — are the whole of the decision and they are made
/// once, here, for both references.
void bind_mesh(MeshRenderer& renderer, const AssetResolver& resolver, BindReport& out) noexcept {
    if (renderer.mesh.is_nil()) {
        out.unreferenced_meshes += 1;
        return;
    }
    const MeshResolution resolved = resolver.mesh(renderer.mesh, resolver.user);
    if (!resolved.found) {
        renderer.mesh_handle = render::MeshHandle{};
        out.unresolved_meshes += 1;
        return;
    }
    renderer.mesh_handle = resolved.mesh;
    renderer.local_bounds = resolved.local_bounds;
    out.meshes_bound += 1;
}

void bind_material(MeshRenderer& renderer, const AssetResolver& resolver,
                   BindReport& out) noexcept {
    // A world with no material resolver keeps whatever material handle it was given. That is not
    // the mesh case: a material reference the caller cannot resolve is a shading question, and an
    // instance with no mesh draws nothing either way.
    if (resolver.material == nullptr) {
        return;
    }
    if (renderer.material.is_nil()) {
        out.unreferenced_materials += 1;
        return;
    }
    const MaterialResolution resolved = resolver.material(renderer.material, resolver.user);
    if (!resolved.found) {
        renderer.material_handle = render::MaterialHandle{};
        out.unresolved_materials += 1;
        return;
    }
    renderer.material_handle = resolved.material;
    out.materials_bound += 1;
}

}  // namespace

Status bind_render_assets(World& world, const RenderComponents& components,
                          const AssetResolver& resolver, BindReport& out) noexcept {
    out = BindReport{};
    if (resolver.mesh == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a binding pass with no mesh resolver would clear every handle in the world "
                    "and report that it had bound them");
    }
    if (components.mesh_renderer == ecs::kInvalidComponent ||
        !world.components().registered(components.mesh_renderer)) {
        return fail(ErrorCode::InvalidArgument,
                    "cy::rendering::MeshRenderer is not a component of this world; bind it against "
                    "the world RenderComponents::register_all was called on");
    }

    ecs::QueryDesc desc(world.allocator());
    if (Status declared = desc.write(components.mesh_renderer); !declared) {
        return declared;
    }
    ecs::Query query(world, std::move(desc));
    return query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        const Span<MeshRenderer> renderers = chunk.write<MeshRenderer>(components.mesh_renderer);
        for (MeshRenderer& renderer : renderers) {
            out.examined += 1;
            bind_mesh(renderer, resolver, out);
            bind_material(renderer, resolver, out);
        }
    });
}

}  // namespace cy::rendering
