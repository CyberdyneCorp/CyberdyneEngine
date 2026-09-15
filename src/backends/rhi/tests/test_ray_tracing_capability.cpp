// THE RAY-TRACING CAPABILITY REPORTS THE DEVICE, NOT THE BUILD. M11.c task 2.6.
//
// `rhi-and-render-graph` — "Every optional capability SHALL be queryable... The renderer SHALL
// branch on capabilities, never on backend identity." `ray-tracing-infrastructure` — "Capability
// gating and fallback".
//
// ================================================================================================
// THE DEFECT THIS FILE IS THE CONTROL FOR, AND IT WAS TRUE OF THIS TREE FOR EIGHT MILESTONES
// ================================================================================================
//
// `Capability::RayTracing` was an enumerator NOTHING SET from M3 until M11.c. Every device this
// engine could open reported no ray tracing — including the RTX 5060 the GI fallback suite was
// written on, whose driver lists `VK_KHR_ray_query` — so every consumer ran its software tier and
// `src/rendering/raytracing/README.md` carried the absence as a fact a reader could check.
//
// The fix has a failure mode of its own and it is the one worth testing: a backend that sets the
// capability because it was COMPILED against the extension's headers, or because the extension
// appears in a list, rather than because the DEVICE reported the feature and the engine asked for
// it. Listing an extension and enabling its feature are different answers and a driver gives both.
//
// So the decision is one function over a struct of what the backend observed, and this file drives
// it. Six answers, each removed in turn: a capability derived from any five of them is a capability
// that can be reported for a device which cannot trace.
//
// ================================================================================================
// WHY THIS SUITE NEEDS NO DEVICE, AND WHAT THAT COSTS
// ================================================================================================
//
// Because the claim is about a DERIVATION and a derivation is arithmetic. What it deliberately does
// NOT check is that the Vulkan backend fills the observation correctly — that needs a driver, it is
// `render.ray_tracing_capability`'s, and it carries `requires = "gpu"` for exactly that reason.
// The two halves are separate suites so that a green here cannot be read as a green there.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/capabilities.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>

#include <cstdio>

namespace {

using cy::rhi::Capability;
using cy::rhi::device_reports_ray_tracing;
using cy::rhi::RayTracingObservation;

/// A device that answered yes to everything. The only observation from which the capability may
/// follow, and the positive control for every case below.
[[nodiscard]] RayTracingObservation capable() noexcept {
    RayTracingObservation observed;
    observed.acceleration_structure_extension = true;
    observed.ray_query_extension = true;
    observed.deferred_host_operations_extension = true;
    observed.acceleration_structure_feature = true;
    observed.ray_query_feature = true;
    observed.enabled_on_the_device = true;
    return observed;
}

/// The six answers, each addressable by name so a failure says WHICH one stopped mattering.
struct Answer {
    const char* name;
    bool RayTracingObservation::*field;
};

constexpr Answer kAnswers[] = {
    {"VK_KHR_acceleration_structure listed", &RayTracingObservation::acceleration_structure_extension},
    {"VK_KHR_ray_query listed", &RayTracingObservation::ray_query_extension},
    {"VK_KHR_deferred_host_operations listed",
     &RayTracingObservation::deferred_host_operations_extension},
    {"accelerationStructure reported", &RayTracingObservation::acceleration_structure_feature},
    {"rayQuery reported", &RayTracingObservation::ray_query_feature},
    {"the features were asked for at device creation", &RayTracingObservation::enabled_on_the_device},
};

}  // namespace

CY_TEST_CASE("the ray-tracing capability reports the device: every answer is load-bearing") {
    CY_REQUIRE(device_reports_ray_tracing(capable()));

    // THE CONTROL THAT MAKES THE POSITIVE ONE MEAN SOMETHING. A function returning its argument's
    // conjunction is indistinguishable from one returning `true` until a false is put in front of
    // it, so every answer is withdrawn in turn and each must be enough on its own to stop the
    // claim. Five of six would be a capability reported for a device that cannot trace.
    for (const Answer& answer : kAnswers) {
        RayTracingObservation observed = capable();
        observed.*answer.field = false;
        const bool reported = device_reports_ray_tracing(observed);
        if (reported) {
            (void)std::printf(
                "  the capability is still reported with '%s' withdrawn — the derivation has "
                "stopped reading the device\n",
                answer.name);
        }
        CY_CHECK_FALSE(reported);
    }

    // And a device that said nothing at all. The state every device this engine could open was in
    // for eight milestones, and the one a default-constructed observation must reproduce.
    CY_CHECK_FALSE(device_reports_ray_tracing(RayTracingObservation{}));
}

CY_TEST_CASE("the ray-tracing capability reports the device: a backend cannot set it directly") {
    // `set_ray_tracing_observation` is the ONLY way the capability is written on the device this
    // engine ships, and it derives the bit rather than taking it. A backend that wanted to report
    // ray tracing without a device to back it would have to lie about the device's own answers,
    // which is a lie a bug report can read: the observation is published beside the bit.
    cy::rhi::DeviceCapabilities capabilities;
    CY_CHECK_FALSE(capabilities.has(Capability::RayTracing));

    RayTracingObservation partial = capable();
    partial.ray_query_feature = false;
    capabilities.set_ray_tracing_observation(partial);
    CY_CHECK_FALSE(capabilities.has(Capability::RayTracing));
    CY_CHECK_FALSE(capabilities.ray_tracing_observation().ray_query_feature);

    capabilities.set_ray_tracing_observation(capable());
    CY_CHECK(capabilities.has(Capability::RayTracing));
    CY_CHECK(capabilities.ray_tracing_observation().ray_query_feature);
}

CY_TEST_CASE("the ray-tracing capability reports the device: the null backend has none") {
    // The null backend is a reference for what the RHI requires and it has no device behind it, so
    // it must report no ray tracing — and it must do so through the same derivation rather than by
    // never having been asked. A null device that reported ray tracing would make every
    // continuous-integration machine claim a tier it cannot run.
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Gpu);
    cy::rhi::DeviceDescription desc;
    const auto device = cy::rhi::null::create_null_device(allocator, desc);
    CY_REQUIRE(device.has_value());

    const cy::rhi::DeviceCapabilities& capabilities = device.value()->capabilities();
    CY_CHECK_FALSE(capabilities.has(Capability::RayTracing));
    CY_CHECK_FALSE(capabilities.ray_tracing_observation().enabled_on_the_device);

    cy::rhi::null::destroy_null_device(allocator, device.value());
}
