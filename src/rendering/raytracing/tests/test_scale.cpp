// Structure sharing at scale: a thousand instances, one bottom-level structure. Task 9.4.
//
// An integration case rather than a unit one because a top-level build over a thousand instances
// costs more than the 1 ms of CPU the unit budget allows in a Debug build — and the scenario it
// checks ("a thousand instances reference one mesh") is written with a thousand in it, so shrinking
// the number to fit the budget would be shrinking the requirement.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/raytracing/acceleration.h>

#include <vector>

namespace {

using cy::rendering::rt::AccelerationService;
using cy::rendering::rt::adapt_static_mesh;
using cy::rendering::rt::InstanceDescriptor;
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

CY_TEST_CASE("a thousand instances of one mesh share one bottom-level structure") {
    const Quad quad;
    AccelerationService service(active_config());
    CY_REQUIRE(service.declare_geometry(7, adapt_static_mesh(quad.geometry(), 0)).has_value());

    for (cy::u32 index = 0; index < 1000; ++index) {
        InstanceDescriptor instance;
        instance.geometry = 7;
        instance.instance_id = index;
        instance.transform =
            cy::Mat4::from_translation(cy::Vec3{static_cast<cy::f32>(index) * 4.0F, 0.0F, 0.0F});
        CY_REQUIRE(service.add_instance(instance).has_value());
    }
    const auto report = service.update();
    CY_CHECK_EQ(report.builds, 1U);
    CY_CHECK_EQ(service.diagnostics().bottom_level_count, 1U);
    CY_CHECK_EQ(service.diagnostics().instance_count, 1000U);
    CY_CHECK_EQ(report.traced_instances, 1000U);
}
