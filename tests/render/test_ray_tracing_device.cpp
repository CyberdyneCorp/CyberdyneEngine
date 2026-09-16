// THE RAY-TRACING CAPABILITY, ON A REAL DEVICE. M11.c task 2.6 and 2.7.
//
// ================================================================================================
// WHY THIS SUITE EXISTS BESIDE A DEVICE-FREE ONE RATHER THAN INSTEAD OF IT
// ================================================================================================
//
// `unit.rhi` drives the DERIVATION — six answers a device can give and what the capability must be
// for each — and needs no GPU, which is M8.c's rule: nothing a milestone claims should be judgeable
// only on the machines with a device in them. What that suite cannot check is whether the Vulkan
// backend FILLS the observation truthfully, because a derivation over a struct a test wrote is a
// test of the struct the test wrote.
//
// This is the other half. It opens a real device, reads what the driver actually answered, and
// requires the capability to agree with it. It carries `requires = "gpu"` in the ledger and says
// why, and its green means one thing only: on THIS device, with THIS driver, the capability is what
// the device reported.
//
// ================================================================================================
// WHAT THIS DOES NOT CLAIM, STATED HERE BECAUSE A GREEN IS EASY TO OVER-READ
// ================================================================================================
//
//   * IT IS NOT A CLAIM ABOUT OTHER BACKENDS. `Capability::RayTracing` is filled in by the Vulkan
//     backend and by no other; Metal and D3D12 are `rhi-and-render-graph`'s, which is M11.d's row.
//     An honest reading of this capability today is "Vulkan only", and the ledger's criterion says
//     so in its own words rather than leaving it to be inferred from a passing suite.
//   * IT IS NOT A CLAIM THAT RAYS EXECUTE ON THE DEVICE. `rt::AccelerationService` builds `cy::Bvh`
//     and traces it on the processor; what changes with the capability set is that the service now
//     reports `Available` on a device that can trace, instead of running the unsupported path on
//     hardware that has the extension. The device tier itself — acceleration structures built by the
//     driver, ray queries issued from a shader — is not in this tree and this suite does not pretend
//     otherwise.
//   * ONE GPU VENDOR. design.md §1.3: an answer measured here is one driver's answer.

#include <cy/test/test.h>

#include <cy/rendering/raytracing/capability.h>

#include "device.h"

#include <cstdio>

namespace {

using cy::rhi::Capability;

}  // namespace

CY_TEST_CASE("the ray-tracing capability agrees with what the device reported") {
    cy::render_test::DeviceFixture fixture("vulkan", "cy_test_render_ray_tracing");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }

    const cy::rhi::DeviceCapabilities& capabilities = fixture.device().capabilities();
    const cy::rhi::RayTracingObservation& observed = capabilities.ray_tracing_observation();
    std::fprintf(stderr,
                 "%s: acceleration_structure=%d ray_query=%d deferred_host_operations=%d "
                 "accelerationStructure=%d rayQuery=%d enabled=%d -> RayTracing=%d\n",
                 capabilities.device_name(),
                 static_cast<int>(observed.acceleration_structure_extension),
                 static_cast<int>(observed.ray_query_extension),
                 static_cast<int>(observed.deferred_host_operations_extension),
                 static_cast<int>(observed.acceleration_structure_feature),
                 static_cast<int>(observed.ray_query_feature),
                 static_cast<int>(observed.enabled_on_the_device),
                 static_cast<int>(capabilities.has(Capability::RayTracing)));

    // THE AGREEMENT. Not "the capability is true" — that would be a claim about this machine's GPU
    // rather than about this engine — but that the bit the renderer branches on is exactly the
    // derivation applied to what the driver said. A backend that set the capability from anything
    // else, a compile-time define included, fails here on a device that answers differently.
    CY_CHECK_EQ(capabilities.has(Capability::RayTracing), device_reports_ray_tracing(observed));

    // AND THE REFUSAL IS READABLE EITHER WAY. A device that cannot trace says which of its answers
    // stopped it, and one that can says nothing — so a bug report about a machine that should have
    // traced has a sentence in it rather than a boolean.
    const char* refusal = cy::rendering::rt::ray_tracing_refusal(capabilities);
    if (capabilities.has(Capability::RayTracing)) {
        CY_CHECK(refusal[0] == '\0');
    } else {
        std::fprintf(stderr, "this device does not report ray tracing: %s\n", refusal);
        CY_CHECK(refusal[0] != '\0');
    }

    // A device that does NOT list the extensions must not report the features either: a driver
    // cannot answer a question it was never asked, and a backend that read a zeroed feature struct
    // as an honest "no" would be indistinguishable from one that asked. This is the guard on the
    // chaining in `create_logical_device`.
    if (!observed.acceleration_structure_extension) {
        CY_CHECK_FALSE(observed.acceleration_structure_feature);
    }
    if (!observed.ray_query_extension) {
        CY_CHECK_FALSE(observed.ray_query_feature);
    }

    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the ray-tracing service is configured from the device and not from a guess") {
    // `ray-tracing-infrastructure`'s README has carried the closing act since M7: *"the RHI reports
    // the capability, and `ServiceConfig::device_supports_ray_tracing` is set from it"*. This is the
    // second of those two edits, run against a real device.
    cy::render_test::DeviceFixture fixture("vulkan", "cy_test_render_ray_tracing_service");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    const cy::rhi::DeviceCapabilities& capabilities = fixture.device().capabilities();

    const auto enabled = cy::rendering::rt::service_config_for(capabilities, true);
    CY_CHECK_EQ(enabled.device_supports_ray_tracing, capabilities.has(Capability::RayTracing));
    CY_CHECK(enabled.enabled_by_profile);

    // THE PROFILE IS A SECOND INPUT AND STAYS ONE. The specification requires a profile that
    // disables ray tracing on CAPABLE hardware to behave exactly like hardware without it, so the
    // two causes must still be two all the way down to `Availability` — collapsing them here would
    // delete the case the requirement is about.
    const auto by_profile = cy::rendering::rt::service_config_for(capabilities, false);
    CY_CHECK_EQ(by_profile.device_supports_ray_tracing, enabled.device_supports_ray_tracing);
    CY_CHECK_FALSE(by_profile.enabled_by_profile);

    cy::rendering::rt::AccelerationService live(enabled);
    cy::rendering::rt::AccelerationService off(by_profile);
    std::fprintf(stderr, "availability with the profile on: %s; off: %s\n",
                 cy::rendering::rt::availability_name(live.availability()),
                 cy::rendering::rt::availability_name(off.availability()));
    CY_CHECK_FALSE(off.active());
    CY_CHECK_EQ(live.active(), capabilities.has(Capability::RayTracing));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
