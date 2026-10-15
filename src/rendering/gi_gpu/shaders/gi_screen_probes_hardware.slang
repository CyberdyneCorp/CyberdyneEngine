// SPDX-License-Identifier: MIT
// CyberGI on the device: the screen-probe final gather through the hardware world tier — ray
// queries over the structures `cy::rendering-raytracing-gpu` built. Issue #35.
//
//   cyGiProbeTraceHardware       `trace_probe` with `HardwareTracer` as the world tier.
//   cyGiProbeIntegrateHardware   `integrate`, whose smooth surfaces' lobe rays are ray queries.
//
// Set 3 is the top level, after the resolve's set 2, as gi_resolve_hardware.slang's: created only on
// a device that reports `Capability::RayTracing`, selected by `GpuRadianceCache::world_tier()`.

#define GI_HARDWARE_SET 3
#include "gi_screen_probes_common.slang"
#include "gi_hardware_tier.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiProbeTraceHardware(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID) {
    spTrace<GiHardwareTier>(group.x, lane.x);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiProbeIntegrateHardware(uint3 thread : SV_DispatchThreadID) {
    spIntegrate<GiHardwareTier>(thread.x);
}
