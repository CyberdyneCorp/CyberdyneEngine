// SPDX-License-Identifier: MIT
// The device mirror seam: what `AccelerationService` tells a `StructureMirror`, in the order it
// decided it. Issue #35, stage 3.
//
// `cy::rendering-raytracing-gpu` builds what this seam reports and nothing else, so every rule the
// service keeps — the build budget, refit until the quality floor, eviction, the instances a top
// level traces, and the inactive service that builds nothing — has to reach a mirror exactly. These
// cases hold the seam to that with a mirror that records, and no device.

#include <cy/test/test.h>

#include <cy/rendering/raytracing/acceleration.h>

#include <vector>

namespace {

using cy::u32;
using cy::rendering::rt::AccelerationService;
using cy::rendering::rt::adapt_procedural;
using cy::rendering::rt::adapt_skinned_mesh;
using cy::rendering::rt::adapt_static_mesh;
using cy::rendering::rt::BottomLevelView;
using cy::rendering::rt::InstanceDescriptor;
using cy::rendering::rt::MaintenancePolicy;
using cy::rendering::rt::StructureMirror;
using cy::rendering::rt::TopLevelInstance;
using cy::rendering::rt::TriangleGeometry;

struct Quad {
    std::vector<cy::Vec3> positions{
        {-1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 1.0F}, {-1.0F, 0.0F, 1.0F}};
    std::vector<u32> indices{0, 1, 2, 0, 2, 3};

    [[nodiscard]] TriangleGeometry geometry() const noexcept {
        TriangleGeometry triangles;
        triangles.positions = {positions.data(), positions.size()};
        triangles.indices = {indices.data(), indices.size()};
        return triangles;
    }
};

/// What a mirror was told, frame by frame.
struct Recorder final : StructureMirror {
    struct Built {
        u32 slot = 0;
        cy::u64 id = 0;
        MaintenancePolicy maintenance = MaintenancePolicy::Static;
        std::vector<cy::Vec3> positions;
        bool refit = false;
    };
    std::vector<Built> built;
    std::vector<u32> evicted;
    std::vector<std::vector<TopLevelInstance>> tops;

    void bottom_built(u32 slot, const BottomLevelView& view) noexcept override {
        built.push_back(Built{slot, view.id, view.maintenance,
                              std::vector<cy::Vec3>(view.positions.begin(), view.positions.end()),
                              false});
    }
    void bottom_refit(u32 slot, const BottomLevelView& view) noexcept override {
        built.push_back(Built{slot, view.id, view.maintenance,
                              std::vector<cy::Vec3>(view.positions.begin(), view.positions.end()),
                              true});
    }
    void bottom_evicted(u32 slot) noexcept override { evicted.push_back(slot); }
    void top_level(cy::Span<const TopLevelInstance> instances) noexcept override {
        tops.emplace_back(instances.begin(), instances.end());
    }
    void clear() {
        built.clear();
        evicted.clear();
        tops.clear();
    }
};

AccelerationService::ServiceConfig active_config() noexcept {
    AccelerationService::ServiceConfig config;
    config.device_supports_ray_tracing = true;
    return config;
}

}  // namespace

CY_TEST_CASE("a mirror is told every build, refit and eviction the service decides") {
    Quad quad;
    AccelerationService service(active_config());
    Recorder mirror;
    service.set_mirror(&mirror);

    CY_REQUIRE(service.declare_geometry(1, adapt_static_mesh(quad.geometry(), 0)).has_value());
    CY_REQUIRE(service.declare_geometry(2, adapt_skinned_mesh(quad.geometry())).has_value());
    InstanceDescriptor first;
    first.geometry = 1;
    first.instance_id = 10;
    InstanceDescriptor second;
    second.geometry = 2;
    second.instance_id = 20;
    second.transform = cy::Mat4::from_translation(cy::Vec3{0.0F, 2.0F, 0.0F});
    const auto handle = service.add_instance(first);
    CY_REQUIRE(handle.has_value());
    CY_REQUIRE(service.add_instance(second).has_value());

    // Frame one: both built, then a top level of both, in the service's own order.
    const auto built = service.update();
    CY_CHECK_EQ(built.builds, 2U);
    CY_REQUIRE_EQ(mirror.built.size(), 2U);
    CY_CHECK_FALSE(mirror.built[0].refit);
    CY_CHECK_FALSE(mirror.built[1].refit);
    CY_CHECK_EQ(mirror.built[0].positions.size(), quad.positions.size());
    CY_REQUIRE_EQ(mirror.tops.size(), 1U);
    CY_REQUIRE_EQ(mirror.tops[0].size(), 2U);
    CY_CHECK_EQ(mirror.tops[0][0].instance_id, 10U);
    CY_CHECK_EQ(mirror.tops[0][1].instance_id, 20U);
    CY_CHECK_EQ(mirror.tops[0][1].transform.columns[3].y, 2.0F);
    // The top level names the slot the build was reported under.
    for (const TopLevelInstance& instance : mirror.tops[0]) {
        bool named = false;
        for (const Recorder::Built& entry : mirror.built) {
            named = named || entry.slot == instance.bottom;
        }
        CY_CHECK(named);
    }

    // A pose: the skinned geometry is REFIT, with the posed positions, and nothing is built.
    mirror.clear();
    std::vector<cy::Vec3> posed = quad.positions;
    for (cy::Vec3& position : posed) {
        position.y += 0.25F;
    }
    CY_REQUIRE(service.refit_geometry(2, {posed.data(), posed.size()}).has_value());
    const auto refit = service.update();
    CY_CHECK_EQ(refit.refits, 1U);
    CY_REQUIRE_EQ(mirror.built.size(), 1U);
    CY_CHECK(mirror.built[0].refit);
    CY_CHECK_EQ(mirror.built[0].id, 2U);
    CY_CHECK_EQ(mirror.built[0].maintenance, MaintenancePolicy::Refit);
    CY_CHECK_EQ(mirror.built[0].positions[0].y, 0.25F);

    // A pose far outside the built bounds: the service REBUILDS, and the mirror is told a build.
    mirror.clear();
    for (cy::usize index = 0; index < posed.size(); ++index) {
        posed[index] = quad.positions[index] * 4.0F;
    }
    CY_REQUIRE(service.refit_geometry(2, {posed.data(), posed.size()}).has_value());
    const auto rebuilt = service.update();
    CY_CHECK_EQ(rebuilt.rebuilds, 1U);
    CY_REQUIRE_EQ(mirror.built.size(), 1U);
    CY_CHECK_FALSE(mirror.built[0].refit);

    // A still frame still reports the top level — the service rebuilds it every frame — and no
    // bottom level: deciding that nothing changed below is the mirror's job, not a silence.
    mirror.clear();
    (void)service.update();
    CY_CHECK(mirror.built.empty());
    CY_CHECK_EQ(mirror.tops.size(), 1U);

    // Evicted only when undeclared AND unreferenced, and the top level loses it the same frame.
    mirror.clear();
    service.remove_instance(handle.value());
    service.release_geometry(1);
    (void)service.update();
    CY_CHECK_EQ(mirror.evicted.size(), 1U);
    CY_REQUIRE_EQ(mirror.tops.size(), 1U);
    CY_CHECK_EQ(mirror.tops[0].size(), 1U);
}

CY_TEST_CASE(
    "an instance whose structure the budget deferred is left out of the mirror's top level "
    "too") {
    Quad quad;
    AccelerationService::ServiceConfig config = active_config();
    config.budget.max_builds_per_frame = 1;
    AccelerationService service(config);
    Recorder mirror;
    service.set_mirror(&mirror);
    for (u32 geometry = 1; geometry <= 3; ++geometry) {
        CY_REQUIRE(
            service.declare_geometry(geometry, adapt_static_mesh(quad.geometry(), 0)).has_value());
        InstanceDescriptor instance;
        instance.geometry = geometry;
        instance.instance_id = geometry;
        CY_REQUIRE(service.add_instance(instance).has_value());
    }
    const auto first = service.update();
    CY_CHECK_EQ(first.builds, 1U);
    CY_CHECK_EQ(first.excluded_instances, 2U);
    CY_CHECK_EQ(mirror.built.size(), 1U);
    CY_REQUIRE_EQ(mirror.tops.size(), 1U);
    // The device traces the same world the service traces: one instance, not three.
    CY_CHECK_EQ(mirror.tops[0].size(), 1U);
}

CY_TEST_CASE("an inactive service tells a mirror nothing, and a late mirror is caught up") {
    Quad quad;
    AccelerationService::ServiceConfig config = active_config();
    config.enabled_by_profile = false;
    AccelerationService service(config);
    Recorder mirror;
    service.set_mirror(&mirror);
    CY_REQUIRE(service.declare_geometry(1, adapt_static_mesh(quad.geometry(), 0)).has_value());
    InstanceDescriptor instance;
    instance.geometry = 1;
    instance.instance_id = 7;
    CY_REQUIRE(service.add_instance(instance).has_value());
    (void)service.update();
    // A profile that disabled ray tracing builds nothing, here or on a device.
    CY_CHECK(mirror.built.empty());
    CY_CHECK(mirror.tops.empty());

    // Enabled, with no mirror: the service builds on its own.
    service.set_enabled_by_profile(true);
    service.set_mirror(nullptr);
    CY_CHECK_EQ(service.update().builds, 1U);

    // A mirror installed now is told about the structure built before it, then the top level.
    service.set_mirror(&mirror);
    (void)service.update();
    CY_REQUIRE_EQ(mirror.built.size(), 1U);
    CY_CHECK_EQ(mirror.built[0].id, 1U);
    CY_CHECK_EQ(mirror.tops.size(), 1U);

    // Procedural geometry is reported as what it is, so a device that cannot intersect it knows.
    mirror.clear();
    CY_REQUIRE(
        service
            .declare_geometry(
                2, adapt_procedural(
                       cy::Aabb::from_center_extents(cy::Vec3{}, cy::Vec3{1.0F, 1.0F, 1.0F}), 3))
            .has_value());
    InstanceDescriptor box;
    box.geometry = 2;
    box.instance_id = 8;
    CY_REQUIRE(service.add_instance(box).has_value());
    (void)service.update();
    CY_REQUIRE_EQ(mirror.built.size(), 1U);
    CY_CHECK(mirror.built[0].positions.empty());
}
