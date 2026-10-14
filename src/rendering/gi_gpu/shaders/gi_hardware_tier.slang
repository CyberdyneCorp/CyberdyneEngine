// SPDX-License-Identifier: MIT
// CyberGI on the device: the hardware world tier — a ray query over the structures
// `cy::rendering-raytracing-gpu` built — as an `IGiWorldTier`. Issue #35, stages 3 and 4.
// Include-only: gi_hardware.slang's entry points (stage 3) and gi_resolve_hardware.slang's (stage
// 4) are the ones that trace through it.
//
// `GI_HARDWARE_SET` is the descriptor set the top level is bound at: 2 for the probe gather and the
// tier batches, whose sets are the scene and the probe set, and 3 for the per-pixel resolve, whose
// third set is the resolve's own. Every other line is the tier exactly as stage 3 wrote it.

#ifndef GI_HARDWARE_SET
#define GI_HARDWARE_SET 2
#endif

// Included after gi_probe_common.slang, whose `IGiWorldTier` and probe set it uses.

struct GiHardwareSet {
    RaytracingAccelerationStructure topLevel;
    /// `DeviceStructureBinding::instances`: four float4 per traced instance.
    StructuredBuffer<uint4> instances;
    StructuredBuffer<float> positions;
    StructuredBuffer<uint> indices;
};

[[vk::binding(0, GI_HARDWARE_SET)]] ParameterBlock<GiHardwareSet> giHardware;

float3 ghVertex(uint vertex) {
    return float3(giHardware.positions[vertex * 3u], giHardware.positions[(vertex * 3u) + 1u],
                  giHardware.positions[(vertex * 3u) + 2u]);
}

struct GiHardwareTier : IGiWorldTier {
    static GiWorldHit trace(float3 origin, float3 direction, float maxDistance) {
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = direction;
        ray.TMin = 1.0e-3f;
        ray.TMax = maxDistance;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(giHardware.topLevel, RAY_FLAG_NONE, 0xFFu, ray);
        while (query.Proceed()) {
        }
        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            return gpMiss();
        }
        const float t = query.CommittedRayT();
        const uint instance = query.CommittedInstanceID();
        const uint primitive = query.CommittedPrimitiveIndex();
        const uint4 record = giHardware.instances[instance * 4u];
        const uint first = record.y + (primitive * 3u);
        const float3 v0 = ghVertex(record.x + giHardware.indices[first]);
        const float3 v1 = ghVertex(record.x + giHardware.indices[first + 1u]);
        const float3 v2 = ghVertex(record.x + giHardware.indices[first + 2u]);
        const float4 c0 = asfloat(giHardware.instances[(instance * 4u) + 1u]);
        const float4 c1 = asfloat(giHardware.instances[(instance * 4u) + 2u]);
        const float4 c2 = asfloat(giHardware.instances[(instance * 4u) + 3u]);
        const float3 local = cross(v1 - v0, v2 - v0);
        float3 normal = giNormalisedOr((c0.xyz * local.x) + (c1.xyz * local.y) + (c2.xyz * local.z),
                                       float3(0.0f, 1.0f, 0.0f));
        if (dot(normal, direction) > 0.0f) {
            normal = -normal;
        }
        GiWorldHit hit;
        hit.hit = true;
        hit.t = t;
        hit.position = origin + (direction * t);
        hit.normal = normal;
        // The proxy penalty: `c0.w` is the declared error of the instance's geometry.
        const float4 tier = asfloat(giProbes.constants[kGpTier]);
        hit.confidence = tier.z - gpClamp(c0.w * 2.0f, 0.0f, 0.4f);
        return hit;
    }
    static uint source() {
        return kGpSourceHardware;
    }
}
