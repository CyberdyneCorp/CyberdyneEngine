// Capability gating: disabled on capable hardware must be indistinguishable from absent. Task 9.4.
//
// `ray-tracing-infrastructure` — "Ray tracing disabled by profile": "behaviour SHALL match the
// unsupported case exactly, so the fallback path is exercised and testable". This file is the
// "testable" half of that sentence. It drives the same script through three services — unsupported,
// disabled by profile, and available — and asserts that the first two produce byte-identical
// observable state while the third differs, because a test that only checked the first two would
// pass on a service that never traced anything at all.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/raytracing/acceleration.h>

#include <vector>

namespace {

using cy::rendering::rt::AccelerationService;
using cy::rendering::rt::adapt_static_mesh;
using cy::rendering::rt::Availability;
using cy::rendering::rt::Consumer;
using cy::rendering::rt::InstanceDescriptor;
using cy::rendering::rt::is_active;
using cy::rendering::rt::RayHit;
using cy::rendering::rt::RayQuery;
using cy::rendering::rt::TriangleGeometry;

/// Everything a consumer can observe about a service after the same script has run through it.
struct Observation {
    Availability availability = Availability::Unsupported;
    bool active = false;
    bool has_structure = false;
    cy::u64 structure_bytes = 0;
    cy::u32 bottom_level_count = 0;
    cy::u32 traced_instances = 0;
    cy::u32 builds = 0;
    cy::u32 excluded_instances = 0;
    bool hit = false;
    cy::f32 hit_t = 0.0F;
    cy::u32 hit_instance = 0;
    bool occluded = false;
};

[[nodiscard]] bool same_behaviour(const Observation& a, const Observation& b) noexcept {
    // Availability is deliberately excluded: it is the CAUSE, which the two differ in, and the
    // requirement is about the BEHAVIOUR, which they must not.
    return a.active == b.active && a.has_structure == b.has_structure &&
           a.structure_bytes == b.structure_bytes && a.bottom_level_count == b.bottom_level_count &&
           a.traced_instances == b.traced_instances && a.builds == b.builds &&
           a.excluded_instances == b.excluded_instances && a.hit == b.hit && a.hit_t == b.hit_t &&
           a.hit_instance == b.hit_instance && a.occluded == b.occluded;
}

Observation run_script(bool device_supports, bool enabled_by_profile) {
    const std::vector<cy::Vec3> positions{
        {-1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 1.0F}, {-1.0F, 0.0F, 1.0F}};
    const std::vector<cy::u32> indices{0, 1, 2, 0, 2, 3};
    TriangleGeometry triangles;
    triangles.positions = {positions.data(), positions.size()};
    triangles.indices = {indices.data(), indices.size()};

    AccelerationService::ServiceConfig config;
    config.device_supports_ray_tracing = device_supports;
    config.enabled_by_profile = enabled_by_profile;
    AccelerationService service(config);

    // A consumer's residency path does not branch on the capability: it declares what is resident
    // whatever the service decides to do with it.
    (void)service.declare_geometry(1, adapt_static_mesh(triangles, 0));
    InstanceDescriptor instance;
    instance.geometry = 1;
    instance.instance_id = 77;
    (void)service.add_instance(instance);
    const auto report = service.update();

    RayQuery query;
    query.ray.origin = {0.0F, 2.0F, 0.0F};
    query.ray.direction = {0.0F, -1.0F, 0.0F};
    const RayHit hit = service.trace(query);

    Observation observation;
    observation.availability = service.availability();
    observation.active = service.active();
    observation.has_structure = service.has_structure(1);
    observation.structure_bytes = service.diagnostics().structure_bytes;
    observation.bottom_level_count = service.diagnostics().bottom_level_count;
    observation.traced_instances = report.traced_instances;
    observation.builds = report.builds;
    observation.excluded_instances = report.excluded_instances;
    observation.hit = hit.hit;
    observation.hit_t = hit.t;
    observation.hit_instance = hit.instance_id;
    observation.occluded = service.occluded(query.ray, 10.0F, Consumer::Shadows);
    return observation;
}

}  // namespace

CY_TEST_CASE("disabled by profile behaves exactly like a device without ray tracing") {
    const Observation unsupported = run_script(false, true);
    const Observation disabled = run_script(true, false);
    const Observation available = run_script(true, true);

    CY_CHECK_EQ(unsupported.availability, Availability::Unsupported);
    CY_CHECK_EQ(disabled.availability, Availability::DisabledByProfile);
    CY_CHECK_EQ(available.availability, Availability::Available);

    // The requirement.
    CY_CHECK(same_behaviour(unsupported, disabled));

    // And the control: a test that only compared the two inactive services would pass on a service
    // that never traced at all.
    CY_CHECK_FALSE(same_behaviour(unsupported, available));
    CY_CHECK(available.hit);
    CY_CHECK_FALSE(unsupported.hit);
}

CY_TEST_CASE("an inactive service builds nothing and holds no structure memory") {
    const Observation unsupported = run_script(false, true);
    CY_CHECK_FALSE(unsupported.active);
    CY_CHECK_FALSE(unsupported.has_structure);
    CY_CHECK_EQ(unsupported.structure_bytes, 0U);
    CY_CHECK_EQ(unsupported.bottom_level_count, 0U);
    CY_CHECK_EQ(unsupported.builds, 0U);
    // Not "excluded": there is no traced world to be excluded from. A service that reported
    // exclusions here would make an inactive frame look like a budget failure.
    CY_CHECK_EQ(unsupported.excluded_instances, 0U);
    CY_CHECK_EQ(unsupported.traced_instances, 0U);
}

CY_TEST_CASE("a consumer that asks an inactive service is counted, not hidden") {
    AccelerationService::ServiceConfig config;
    config.device_supports_ray_tracing = false;
    AccelerationService service(config);
    RayQuery query;
    query.consumer = Consumer::GlobalIllumination;
    (void)service.trace(query);
    (void)service.trace(query);
    // The number that says a consumer is issuing rays without selecting its fallback. Zero once the
    // fallbacks are wired; it is a diagnostic, not an error.
    CY_CHECK_EQ(service.diagnostics().queries_while_inactive, 2U);
    CY_CHECK_EQ(service.diagnostics().rays[static_cast<cy::u32>(Consumer::GlobalIllumination)], 0U);
}

CY_TEST_CASE("is_active is the only predicate a consumer may branch on") {
    CY_CHECK_FALSE(is_active(Availability::Unsupported));
    CY_CHECK_FALSE(is_active(Availability::DisabledByProfile));
    CY_CHECK(is_active(Availability::Available));
}
