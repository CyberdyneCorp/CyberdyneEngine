// Structure lifecycle, the build budget, geometry adapters and the diagnostics. Task 9.4.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/raytracing/acceleration.h>

#include <vector>

namespace {

using cy::rendering::rt::AccelerationService;
using cy::rendering::rt::adapt_mesh_particles;
using cy::rendering::rt::adapt_procedural;
using cy::rendering::rt::adapt_skinned_mesh;
using cy::rendering::rt::adapt_static_mesh;
using cy::rendering::rt::adapt_virtual_geometry;
using cy::rendering::rt::adapter_for;
using cy::rendering::rt::BuildInput;
using cy::rendering::rt::Consumer;
using cy::rendering::rt::GeometrySource;
using cy::rendering::rt::InstanceDescriptor;
using cy::rendering::rt::MaintenancePolicy;
using cy::rendering::rt::ProxyPolicy;
using cy::rendering::rt::QueryKind;
using cy::rendering::rt::RayQuery;
using cy::rendering::rt::TriangleGeometry;

/// A unit quad in the XZ plane at y = 0, facing up.
struct Quad {
    std::vector<cy::Vec3> positions{
        {-1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 1.0F}, {-1.0F, 0.0F, 1.0F}};
    std::vector<cy::u32> indices{0, 1, 2, 0, 2, 3};

    [[nodiscard]] TriangleGeometry geometry() const noexcept {
        TriangleGeometry triangles;
        triangles.positions = {positions.data(), positions.size()};
        triangles.indices = {indices.data(), indices.size()};
        return triangles;
    }
};

AccelerationService::ServiceConfig active_config() noexcept {
    AccelerationService::ServiceConfig config;
    config.device_supports_ray_tracing = true;
    config.enabled_by_profile = true;
    return config;
}

}  // namespace

CY_TEST_CASE("the adapter table names a maintenance policy for every geometry source") {
    // `ray-tracing-infrastructure`'s table, row for row. The two that matter are the skinned row —
    // refit, never rebuild, with a quality floor — and the two proxy rows, because a consumer that
    // does not know it is looking at a proxy will present an approximate hit as an exact one.
    CY_CHECK_EQ(adapter_for(GeometrySource::StaticMesh).maintenance, MaintenancePolicy::Static);
    CY_CHECK_EQ(adapter_for(GeometrySource::SkinnedMesh).maintenance, MaintenancePolicy::Refit);
    CY_CHECK_GT(adapter_for(GeometrySource::SkinnedMesh).refit_quality_floor, 0.0F);
    CY_CHECK(adapter_for(GeometrySource::VirtualGeometry).uses_proxy);
    CY_CHECK(adapter_for(GeometrySource::Terrain).uses_proxy);
    CY_CHECK_EQ(adapter_for(GeometrySource::MeshParticles).maintenance,
                MaintenancePolicy::InstanceOnly);
    CY_CHECK_EQ(adapter_for(GeometrySource::Procedural).maintenance, MaintenancePolicy::Rebuild);
}

CY_TEST_CASE("the virtual geometry proxy carries its declared error onto every hit") {
    const Quad quad;
    ProxyPolicy policy;
    policy.detail_level = 3;
    policy.error_metres = 0.07F;

    AccelerationService service(active_config());
    const BuildInput input = adapt_virtual_geometry(quad.geometry(), policy);
    CY_REQUIRE(service.declare_geometry(1, input).has_value());

    InstanceDescriptor instance;
    instance.geometry = 1;
    instance.instance_id = 42;
    instance.material_id = 5;
    CY_REQUIRE(service.add_instance(instance).has_value());
    (void)service.update();

    RayQuery query;
    query.ray.origin = {0.0F, 2.0F, 0.0F};
    query.ray.direction = {0.0F, -1.0F, 0.0F};
    const auto hit = service.trace(query);
    CY_REQUIRE(hit.hit);
    CY_CHECK_EQ(hit.instance_id, 42U);
    CY_CHECK_EQ(hit.material_id, 5U);
    // The number that says how wrong this hit may be. Zero here would be the failure the
    // "documented rather than presented as exact" scenario exists to prevent.
    CY_CHECK_NEAR(hit.declared_error_metres, 0.07F, 1e-6F);
}

CY_TEST_CASE("a residency spike is spread across frames and nothing stalls") {
    const Quad quad;
    AccelerationService::ServiceConfig config = active_config();
    config.budget.max_builds_per_frame = 4;
    AccelerationService service(config);

    for (cy::u64 id = 1; id <= 20; ++id) {
        CY_REQUIRE(service.declare_geometry(id, adapt_static_mesh(quad.geometry(), 0)).has_value());
        InstanceDescriptor instance;
        instance.geometry = id;
        instance.instance_id = static_cast<cy::u32>(id);
        CY_REQUIRE(service.add_instance(instance).has_value());
    }

    const auto first = service.update();
    CY_CHECK_EQ(first.builds, 4U);
    CY_CHECK_EQ(first.deferred_builds, 16U);
    // The instances waiting on a structure are excluded from the traced world, not waited for.
    CY_CHECK_EQ(first.excluded_instances, 16U);
    CY_CHECK_EQ(first.traced_instances, 4U);

    cy::u32 frames = 1;
    while (service.diagnostics().pending_builds != 0 && frames < 20) {
        (void)service.update();
        frames += 1;
    }
    CY_CHECK_EQ(frames, 5U);
    CY_CHECK_EQ(service.diagnostics().last_frame.excluded_instances, 0U);
    CY_CHECK_EQ(service.diagnostics().bottom_level_count, 20U);
}

CY_TEST_CASE("the build queue is ordered by importance and screen coverage") {
    const Quad quad;
    AccelerationService::ServiceConfig config = active_config();
    config.budget.max_builds_per_frame = 1;
    AccelerationService service(config);

    for (cy::u64 id = 1; id <= 3; ++id) {
        CY_REQUIRE(service.declare_geometry(id, adapt_static_mesh(quad.geometry(), 0)).has_value());
        InstanceDescriptor instance;
        instance.geometry = id;
        instance.instance_id = static_cast<cy::u32>(id);
        instance.importance = (id == 2) ? 10.0F : 0.1F;
        instance.screen_coverage = (id == 2) ? 0.8F : 0.0F;
        CY_REQUIRE(service.add_instance(instance).has_value());
    }

    (void)service.update();
    CY_CHECK(service.has_structure(2));
    CY_CHECK_FALSE(service.has_structure(1));
    CY_CHECK_FALSE(service.has_structure(3));
}

CY_TEST_CASE("a skinned mesh refits and rebuilds only when the pose leaves its bounds") {
    Quad quad;
    AccelerationService service(active_config());
    CY_REQUIRE(service.declare_geometry(3, adapt_skinned_mesh(quad.geometry())).has_value());
    InstanceDescriptor instance;
    instance.geometry = 3;
    instance.instance_id = 1;
    CY_REQUIRE(service.add_instance(instance).has_value());
    CY_CHECK_EQ(service.update().builds, 1U);

    // A small pose change: a refit, and the traced surface follows it exactly.
    std::vector<cy::Vec3> posed = quad.positions;
    for (cy::Vec3& position : posed) {
        position.y += 0.25F;
    }
    CY_REQUIRE(service.refit_geometry(3, {posed.data(), posed.size()}).has_value());
    const auto refit_frame = service.update();
    CY_CHECK_EQ(refit_frame.refits, 1U);
    CY_CHECK_EQ(refit_frame.builds, 0U);

    RayQuery query;
    query.ray.origin = {0.0F, 3.0F, 0.0F};
    query.ray.direction = {0.0F, -1.0F, 0.0F};
    const auto hit = service.trace(query);
    CY_REQUIRE(hit.hit);
    CY_CHECK_NEAR(hit.t, 2.75F, 1e-4F);

    // A pose that has grown far past the bounds the tree was built for is a rebuild instead.
    for (cy::usize index = 0; index < posed.size(); ++index) {
        posed[index] = quad.positions[index] * 4.0F;
    }
    CY_REQUIRE(service.refit_geometry(3, {posed.data(), posed.size()}).has_value());
    const auto grown = service.update();
    CY_CHECK_EQ(grown.refits, 0U);
    CY_CHECK_EQ(grown.rebuilds, 1U);
}

CY_TEST_CASE("an unreferenced structure is evicted and its memory goes back") {
    const Quad quad;
    AccelerationService service(active_config());
    CY_REQUIRE(service.declare_geometry(9, adapt_static_mesh(quad.geometry(), 0)).has_value());
    InstanceDescriptor instance;
    instance.geometry = 9;
    instance.instance_id = 1;
    const auto handle = service.add_instance(instance);
    CY_REQUIRE(handle.has_value());
    (void)service.update();
    CY_CHECK_GT(service.diagnostics().structure_bytes, 0U);

    // A declaration alone keeps the structure: it is the residency system's statement that the
    // geometry is loaded, and an instance may arrive next frame.
    service.remove_instance(handle.value());
    (void)service.update();
    CY_CHECK_GT(service.diagnostics().structure_bytes, 0U);

    service.release_geometry(9);
    (void)service.update();
    CY_CHECK_EQ(service.diagnostics().structure_bytes, 0U);
    CY_CHECK_EQ(service.diagnostics().bottom_level_count, 0U);
    CY_CHECK_FALSE(service.has_structure(9));
}

CY_TEST_CASE("structure memory is attributable to the geometry source that asked for it") {
    const Quad quad;
    AccelerationService service(active_config());
    CY_REQUIRE(service.declare_geometry(1, adapt_static_mesh(quad.geometry(), 0)).has_value());
    CY_REQUIRE(service.declare_geometry(2, adapt_virtual_geometry(quad.geometry(), ProxyPolicy{}))
                   .has_value());
    for (cy::u64 id = 1; id <= 2; ++id) {
        InstanceDescriptor instance;
        instance.geometry = id;
        instance.instance_id = static_cast<cy::u32>(id);
        CY_REQUIRE(service.add_instance(instance).has_value());
    }
    (void)service.update();

    const auto& diagnostics = service.diagnostics();
    const auto static_bytes =
        diagnostics.structure_bytes_by_source[static_cast<cy::u32>(GeometrySource::StaticMesh)];
    const auto proxy_bytes =
        diagnostics
            .structure_bytes_by_source[static_cast<cy::u32>(GeometrySource::VirtualGeometry)];
    CY_CHECK_GT(static_bytes, 0U);
    CY_CHECK_GT(proxy_bytes, 0U);
    CY_CHECK_EQ(static_bytes + proxy_bytes, diagnostics.structure_bytes);
}

CY_TEST_CASE("mesh particles own no structure of their own") {
    AccelerationService service(active_config());
    // The InstanceOnly row is refused rather than quietly building a second copy of the source
    // mesh's structure, which is the defect the row exists to prevent.
    CY_CHECK_FALSE(service.declare_geometry(1, adapt_mesh_particles()).has_value());
}

CY_TEST_CASE("procedural geometry is bounds plus an intersection shader") {
    AccelerationService service(active_config());
    const cy::Aabb bounds =
        cy::Aabb::from_min_max(cy::Vec3{-1.0F, -1.0F, -1.0F}, cy::Vec3{1.0F, 1.0F, 1.0F});
    CY_REQUIRE(service.declare_geometry(4, adapt_procedural(bounds, 11)).has_value());
    InstanceDescriptor instance;
    instance.geometry = 4;
    instance.instance_id = 3;
    instance.material_id = 6;
    CY_REQUIRE(service.add_instance(instance).has_value());
    (void)service.update();

    RayQuery query;
    query.ray.origin = {0.0F, 5.0F, 0.0F};
    query.ray.direction = {0.0F, -1.0F, 0.0F};
    const auto hit = service.trace(query);
    CY_REQUIRE(hit.hit);
    CY_CHECK_NEAR(hit.t, 4.0F, 1e-4F);
    CY_CHECK_EQ(hit.material_id, 6U);
}

CY_TEST_CASE("ray counts are attributed to the consumer that issued them") {
    const Quad quad;
    AccelerationService service(active_config());
    CY_REQUIRE(service.declare_geometry(1, adapt_static_mesh(quad.geometry(), 0)).has_value());
    InstanceDescriptor instance;
    instance.geometry = 1;
    instance.instance_id = 0;
    CY_REQUIRE(service.add_instance(instance).has_value());
    (void)service.update();

    RayQuery query;
    query.ray.origin = {0.0F, 1.0F, 0.0F};
    query.ray.direction = {0.0F, -1.0F, 0.0F};
    query.consumer = Consumer::Reflections;
    (void)service.trace(query);
    query.consumer = Consumer::Shadows;
    query.kind = QueryKind::Shadow;
    (void)service.trace(query);
    (void)service.trace(query);

    const auto& rays = service.diagnostics().rays;
    CY_CHECK_EQ(rays[static_cast<cy::u32>(Consumer::Reflections)], 1U);
    CY_CHECK_EQ(rays[static_cast<cy::u32>(Consumer::Shadows)], 2U);
    CY_CHECK_EQ(rays[static_cast<cy::u32>(Consumer::GlobalIllumination)], 0U);
}

CY_TEST_CASE("an instance transform moves the traced surface without touching the structure") {
    const Quad quad;
    AccelerationService service(active_config());
    CY_REQUIRE(service.declare_geometry(1, adapt_static_mesh(quad.geometry(), 0)).has_value());
    InstanceDescriptor instance;
    instance.geometry = 1;
    instance.instance_id = 0;
    const auto handle = service.add_instance(instance);
    CY_REQUIRE(handle.has_value());
    (void)service.update();

    CY_REQUIRE(service
                   .set_instance_transform(handle.value(),
                                           cy::Mat4::from_translation(cy::Vec3{0.0F, 1.5F, 0.0F}))
                   .has_value());
    const auto report = service.update();
    CY_CHECK_EQ(report.builds, 0U);
    CY_CHECK_EQ(report.rebuilds, 0U);
    CY_CHECK_EQ(report.refits, 0U);

    RayQuery query;
    query.ray.origin = {0.0F, 4.0F, 0.0F};
    query.ray.direction = {0.0F, -1.0F, 0.0F};
    const auto hit = service.trace(query);
    CY_REQUIRE(hit.hit);
    CY_CHECK_NEAR(hit.t, 2.5F, 1e-4F);
}

CY_TEST_CASE("a scaled instance reports distance in world metres") {
    const Quad quad;
    AccelerationService service(active_config());
    CY_REQUIRE(service.declare_geometry(1, adapt_static_mesh(quad.geometry(), 0)).has_value());
    InstanceDescriptor instance;
    instance.geometry = 1;
    instance.instance_id = 0;
    // A non-uniform scale, which is where a t left in object space stops being a distance.
    instance.transform = cy::Mat4::from_translation(cy::Vec3{0.0F, 2.0F, 0.0F}) *
                         cy::Mat4::from_scale(cy::Vec3{3.0F, 1.0F, 0.5F});
    CY_REQUIRE(service.add_instance(instance).has_value());
    (void)service.update();

    RayQuery query;
    query.ray.origin = {0.0F, 10.0F, 0.0F};
    query.ray.direction = {0.0F, -1.0F, 0.0F};
    const auto hit = service.trace(query);
    CY_REQUIRE(hit.hit);
    CY_CHECK_NEAR(hit.t, 8.0F, 1e-3F);
    CY_CHECK_NEAR(hit.position.y, 2.0F, 1e-3F);
    CY_CHECK_NEAR(hit.normal.y, 1.0F, 1e-3F);
}

CY_TEST_CASE("the three query kinds are three questions, and a hit names the scene's own ids") {
    // `ray-tracing-infrastructure` — "Ray query interface". Two claims, and neither of them was
    // observed by any case in this suite before M11.c's requirements pass: that closest hit, any
    // hit and the shadow query are three DIFFERENT questions over one structure, and that a hit
    // resolves to the GPU scene's instance identifier and the GPU material table's entry rather
    // than to a slot private to this service — which is what lets a hit be shaded through the
    // material path that rasterisation uses.
    const Quad quad;
    // Two triangles, two materials. The per-triangle table is the mesh-with-several-materials case
    // and a hit must resolve to the TRIANGLE's entry where one is declared: an instance-level
    // answer here would shade half the quad with the other half's material.
    const std::vector<cy::u32> triangle_materials{30U, 31U};
    cy::rendering::rt::TriangleGeometry multi = quad.geometry();
    multi.triangle_materials = {triangle_materials.data(), triangle_materials.size()};

    AccelerationService service(active_config());
    CY_REQUIRE(service.declare_geometry(1, adapt_static_mesh(multi, 0)).has_value());
    CY_REQUIRE(service.declare_geometry(2, adapt_static_mesh(quad.geometry(), 0)).has_value());

    // The FAR surface is added first, so the service's own slot order is not the answer order and
    // an identifier taken from a slot index would be visibly wrong below.
    InstanceDescriptor lower;
    lower.geometry = 2;
    lower.instance_id = 8;
    lower.material_id = 4;
    CY_REQUIRE(service.add_instance(lower).has_value());

    InstanceDescriptor upper;
    upper.geometry = 1;
    upper.instance_id = 7;
    upper.material_id = 3;
    upper.transform = cy::Mat4::from_translation(cy::Vec3{0.0F, 2.0F, 0.0F});
    CY_REQUIRE(service.add_instance(upper).has_value());
    (void)service.update();

    // The ray crosses both surfaces: the upper at 3 metres, the lower at 5.
    RayQuery closest;
    // Off the quad's diagonal deliberately: the seam between the two triangles runs x = z, and a
    // ray down it would resolve to whichever of the two the epsilon fell on.
    closest.ray.origin = {0.5F, 5.0F, -0.2F};
    closest.ray.direction = {0.0F, -1.0F, 0.0F};
    closest.kind = QueryKind::ClosestHit;
    const auto nearest = service.trace(closest);
    CY_REQUIRE(nearest.hit);
    CY_CHECK_NEAR(nearest.t, 3.0F, 1e-4F);
    // THE IDENTITIES, and they are the caller's: the instance the GPU scene declared, and the
    // material table entry the triangle resolves to rather than the instance's own 3.
    CY_CHECK_EQ(nearest.instance_id, 7U);
    CY_CHECK_NE(nearest.instance_id, cy::rendering::rt::kInvalidInstance);
    CY_REQUIRE(nearest.primitive_index < triangle_materials.size());
    CY_CHECK_EQ(nearest.material_id, triangle_materials[nearest.primitive_index]);
    CY_CHECK_NE(nearest.material_id, upper.material_id);
    // And a geometry that declares no table resolves to the instance's material, which is the
    // ordinary single-material mesh and the other half of the same rule.
    RayQuery below;
    below.ray = closest.ray;
    below.t_min = 3.5F;
    const auto second = service.trace(below);
    CY_REQUIRE(second.hit);
    CY_CHECK_EQ(second.instance_id, 8U);
    CY_CHECK_EQ(second.material_id, 4U);

    // AN ANY-HIT IS A DIFFERENT QUESTION: "is there anything", answered by the first surface the
    // traversal meets rather than by the nearest. It must answer over the same structures, and its
    // answer must be one of the two surfaces — not an invented one and not a miss.
    RayQuery any;
    any.ray = closest.ray;
    any.kind = QueryKind::AnyHit;
    const auto anywhere = service.trace(any);
    CY_REQUIRE(anywhere.hit);
    CY_CHECK(anywhere.instance_id == 7U || anywhere.instance_id == 8U);
    CY_CHECK_GE(anywhere.t, 3.0F - 1e-4F);
    CY_CHECK_LE(anywhere.t, 5.0F + 1e-4F);

    // AND THE SHADOW QUERY RETURNS OCCLUSION AND NOTHING ELSE — a bool, from an interval. A segment
    // that stops short of the upper quad is not occluded; one that reaches it is.
    CY_CHECK_FALSE(service.occluded(closest.ray, 2.5F, Consumer::Shadows));
    CY_CHECK(service.occluded(closest.ray, 3.5F, Consumer::Shadows));
}
