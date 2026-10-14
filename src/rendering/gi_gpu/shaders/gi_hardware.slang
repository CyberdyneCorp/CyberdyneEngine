// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 3: the hardware tier — ray queries over the acceleration service's
// structures as `cy::rendering-raytracing-gpu` built them. Issue #35.
//
//   cyGiGatherProbesHardware   `RadianceCache::gather_probe` with `HardwareTracer` as the tier.
//   cyGiTraceHardware          the hardware world tier alone: `HardwareTracer::trace`/`::radiance`.
//   cyGiTraceTieredHardware    `TieredTracer::trace` with the hardware tier as its world tier.
//
// SET 2 IS THE ONLY THING HERE A DEVICE WITHOUT RAY QUERIES COULD NOT CREATE, and it is why these
// are a file of their own: `GpuRadianceCache` creates these pipelines only on a device that reports
// `Capability::RayTracing`, and selects them per dispatch with the same predicate the host's
// `HardwareTracer::available()` is — the service active and a top level built.
//
// `HardwareTracer::trace`, transcribed: a closest-hit query from `t_min` 1e-3 (`RayQuery`'s
// default) to the ray's reach, no face culling, and on a hit the TRIANGLE'S normal — the cross
// product of its edges in the geometry's own space through the instance's normal matrix, turned to
// face the ray — because that is what `AccelerationService::trace_instance` reports. The hit then
// resolves through the surface cache exactly as a software hit does; nothing here reads a material.

#include "gi_probe_common.slang"
#include "gi_hardware_tier.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiGatherProbesHardware(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID) {
    gpGatherProbe<GiHardwareTier>(group.x, lane.x);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiTraceHardware(uint3 thread : SV_DispatchThreadID) {
    const uint ray = thread.x;
    if (ray >= giDispatch.count) {
        return;
    }
    const float4 origin = giProbes.tierRays[ray * 2u];
    const float3 unit =
        giNormalisedOr(giProbes.tierRays[(ray * 2u) + 1u].xyz, float3(0.0f, 0.0f, -1.0f));
    gpWriteTierResult(ray, gpWorldTrace<GiHardwareTier>(origin.xyz, unit, origin.w), 0u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiTraceTieredHardware(uint3 thread : SV_DispatchThreadID) {
    const uint ray = thread.x;
    if (ray >= giDispatch.count) {
        return;
    }
    const float4 origin = giProbes.tierRays[ray * 2u];
    const float4 direction = giProbes.tierRays[(ray * 2u) + 1u];
    const bool screenPresent = giProbes.constants[kGpScreen].x != 0u;
    uint reads = 0u;
    const GiTierSample sample =
        gpTieredTrace<GiHardwareTier>(origin.xyz, direction.xyz, origin.w, screenPresent, reads);
    gpCount(kGpCountDepthReads, reads);
    gpWriteTierResult(ray, sample, reads);
}
