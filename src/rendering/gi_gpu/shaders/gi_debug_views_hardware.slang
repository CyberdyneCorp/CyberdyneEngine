// SPDX-License-Identifier: MIT
// CyberGI on the device: the GI debug views through the hardware world tier — ray queries over
// the structures `cy::rendering-raytracing-gpu` built. Issue #35.
//
//   cyGiDebugViewHardware   `cyGiDebugView` with `HardwareTracer` as the world tier: the tier and
//                           ray-count views trace the frame's own tier.
//
// Set 3 is the top level, after this file's set 2. Created only on a device that reports
// `Capability::RayTracing`, and selected by `GpuRadianceCache::world_tier()`.

#define GI_HARDWARE_SET 3
#include "gi_debug_views_common.slang"
#include "gi_hardware_tier.slang"

[shader("compute")]
[numthreads(8, 8, 1)]
void cyGiDebugViewHardware(uint3 thread : SV_DispatchThreadID) {
    gdDrawPixel<GiHardwareTier>(thread);
}
