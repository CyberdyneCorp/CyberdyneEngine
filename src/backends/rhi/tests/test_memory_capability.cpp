// SPDX-License-Identifier: MIT
// THE HOST-VISIBLE DEVICE-LOCAL CAPABILITY REPORTS THE DEVICE'S MEMORY TYPES. #77.
//
// ================================================================================================
// THE DEFECT THIS FILE IS THE CONTROL FOR
// ================================================================================================
//
// `Capability::HostVisibleDeviceLocalMemory` was an enumerator the Vulkan backend never set, so
// every Vulkan device reported it absent, the RTX 5060 included. A caller that asked before using
// `MemoryUse::HostVisibleDeviceLocal` was told to use `Upload` instead, which on a discrete GPU is
// system memory read across the bus by every draw. `samples/10-world` keeps its vertex streams
// there and draws them three times a frame with water shading on, and that bus traffic was most
// of the frame's device time (`samples/10-world/geometry_memory.h`).
//
// The decision is one function over what the backend counted, and this file drives it. That the
// Vulkan backend counts truthfully needs a driver and is `render.geometry_memory`'s.

#include <cy/test/test.h>

#include <cy/backends/rhi/capabilities.h>

namespace {

using cy::rhi::Capability;
using cy::rhi::device_offers_host_visible_device_local;
using cy::rhi::DeviceCapabilities;
using cy::rhi::MemoryObservation;

/// The RTX 5060's answer, as `render.geometry_memory` prints it: five types, one of them
/// device-local and mappable (the 256 MiB BAR window), two of them system memory alone.
[[nodiscard]] MemoryObservation discrete() noexcept {
    MemoryObservation observed;
    observed.types = 5;
    observed.device_local_mappable = 1;
    observed.host_only = 2;
    return observed;
}

}  // namespace

CY_TEST_CASE("memory capability: a device-local mappable type is reported") {
    CY_CHECK(device_offers_host_visible_device_local(discrete()));

    DeviceCapabilities capabilities;
    CY_CHECK(!capabilities.has(Capability::HostVisibleDeviceLocalMemory));
    capabilities.set_memory_observation(discrete());
    CY_CHECK(capabilities.has(Capability::HostVisibleDeviceLocalMemory));
    CY_CHECK_EQ(capabilities.memory_observation().device_local_mappable, 1U);
    CY_CHECK_EQ(capabilities.memory_observation().host_only, 2U);
}

CY_TEST_CASE("memory capability: no device-local mappable type, no capability") {
    MemoryObservation observed = discrete();
    observed.device_local_mappable = 0;
    CY_CHECK(!device_offers_host_visible_device_local(observed));

    DeviceCapabilities capabilities;
    capabilities.set(Capability::HostVisibleDeviceLocalMemory, true);
    capabilities.set_memory_observation(observed);
    CY_CHECK(!capabilities.has(Capability::HostVisibleDeviceLocalMemory));
}

CY_TEST_CASE("memory capability: an observation of nothing reports nothing") {
    // A backend that never counted the types has said nothing about them, whatever else it filled.
    MemoryObservation observed;
    observed.device_local_mappable = 1;
    CY_CHECK(!device_offers_host_visible_device_local(observed));
    CY_CHECK(!device_offers_host_visible_device_local(MemoryObservation{}));
}

CY_TEST_CASE("memory capability: a unified-memory device offers it too") {
    // Every type device-local and mappable, none system memory alone: the capability holds, and
    // `host_only` is what tells a caller that choosing it gains nothing.
    MemoryObservation observed;
    observed.types = 3;
    observed.device_local_mappable = 3;
    observed.host_only = 0;
    CY_CHECK(device_offers_host_visible_device_local(observed));
}
