// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 4: the per-pixel resolve through the hardware world tier — ray
// queries over the structures `cy::rendering-raytracing-gpu` built. Issue #35.
//
//   cyGiResolveHardware   `ScreenResolve::produce` with `HardwareTracer` as the world tier.
//
// Set 3 is the top level, after the resolve's own set 2: the only thing here a device without ray
// queries could not create, which is why it is a file of its own — `GpuScreenResolve` creates this
// pipeline only on a device that reports `Capability::RayTracing`, and selects it with
// `GpuRadianceCache::world_tier()`, the one predicate every device tier is chosen by.

#define GI_HARDWARE_SET 3
#include "gi_resolve_common.slang"
#include "gi_hardware_tier.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiResolveHardware(uint3 thread : SV_DispatchThreadID) {
    grResolvePixel<GiHardwareTier>(thread.x);
}
