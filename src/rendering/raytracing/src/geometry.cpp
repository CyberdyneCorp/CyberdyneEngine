#include <cy/rendering/raytracing/geometry.h>

namespace cy::rendering::rt {

const char* geometry_source_name(GeometrySource source) noexcept {
    switch (source) {
        case GeometrySource::StaticMesh:
            return "StaticMesh";
        case GeometrySource::SkinnedMesh:
            return "SkinnedMesh";
        case GeometrySource::VirtualGeometry:
            return "VirtualGeometry";
        case GeometrySource::Terrain:
            return "Terrain";
        case GeometrySource::MeshParticles:
            return "MeshParticles";
        case GeometrySource::Procedural:
            return "Procedural";
        case GeometrySource::Count:
            break;
    }
    return "Unknown";
}

Adapter adapter_for(GeometrySource source) noexcept {
    Adapter adapter;
    adapter.source = source;
    switch (source) {
        case GeometrySource::StaticMesh:
            adapter.maintenance = MaintenancePolicy::Static;
            break;
        case GeometrySource::SkinnedMesh:
            // "A structure refit each frame from the GPU pose world", with a rebuild only when
            // refit quality degrades — which is what the floor below is.
            adapter.maintenance = MaintenancePolicy::Refit;
            adapter.refit_quality_floor = 0.5F;
            break;
        // The two proxy rows behave identically and are grouped rather than repeated. Virtual
        // geometry supplies a level of its cluster hierarchy; terrain supplies the collision or
        // proxy heightfield rather than the rendered surface. Both are static in their own space
        // and both are approximations of what was rasterised, which is the whole of what an adapter
        // has to say about them.
        case GeometrySource::VirtualGeometry:
        case GeometrySource::Terrain:
            adapter.maintenance = MaintenancePolicy::Static;
            adapter.uses_proxy = true;
            break;
        case GeometrySource::MeshParticles:
            adapter.maintenance = MaintenancePolicy::InstanceOnly;
            break;
        case GeometrySource::Procedural:
            adapter.maintenance = MaintenancePolicy::Rebuild;
            break;
        case GeometrySource::Count:
            break;
    }
    return adapter;
}

BuildInput adapt_static_mesh(const TriangleGeometry& triangles, u32 lod) noexcept {
    BuildInput input;
    input.source = GeometrySource::StaticMesh;
    input.maintenance = MaintenancePolicy::Static;
    input.triangles = triangles;
    input.detail_level = lod;
    return input;
}

BuildInput adapt_skinned_mesh(const TriangleGeometry& posed) noexcept {
    BuildInput input;
    input.source = GeometrySource::SkinnedMesh;
    input.maintenance = MaintenancePolicy::Refit;
    input.triangles = posed;
    return input;
}

BuildInput adapt_virtual_geometry(const TriangleGeometry& proxy,
                                  const ProxyPolicy& policy) noexcept {
    BuildInput input;
    input.source = GeometrySource::VirtualGeometry;
    input.maintenance = MaintenancePolicy::Static;
    input.triangles = proxy;
    input.detail_level = policy.detail_level;
    // The whole point of the proxy row: the error travels with the geometry so a hit can report it.
    input.declared_error_metres = policy.error_metres;
    return input;
}

BuildInput adapt_terrain(const TriangleGeometry& heightfield) noexcept {
    BuildInput input;
    input.source = GeometrySource::Terrain;
    input.maintenance = MaintenancePolicy::Static;
    input.triangles = heightfield;
    return input;
}

BuildInput adapt_mesh_particles() noexcept {
    BuildInput input;
    input.source = GeometrySource::MeshParticles;
    input.maintenance = MaintenancePolicy::InstanceOnly;
    return input;
}

BuildInput adapt_procedural(const Aabb& bounds, u32 intersection_shader_id) noexcept {
    BuildInput input;
    input.source = GeometrySource::Procedural;
    input.maintenance = MaintenancePolicy::Rebuild;
    input.procedural = true;
    input.procedural_bounds = bounds;
    input.intersection_shader_id = intersection_shader_id;
    return input;
}

}  // namespace cy::rendering::rt
