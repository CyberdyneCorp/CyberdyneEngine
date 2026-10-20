// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 3: the radiance cache's gather and query, and the tracing tiers.
// Issue #35. Include-only; the entry points are gi_radiance.slang (every tier a device without ray
// queries has) and gi_hardware.slang (the ray query tier).
//
// ================================================================================================
// EVERY FUNCTION BELOW IS A TRANSCRIPTION, AND NAMES WHAT IT TRANSCRIBES
// ================================================================================================
//
// `cy::rendering-gi` is still the oracle. The gather is `RadianceCache::gather_probe` with the three
// encodings of `encode_sample` / `normalise_payload` / `decode_payload`; the query is
// `RadianceCache::sample` with `visibility_weight`; the software tier is `SoftwareTracer::trace` and
// `::radiance`; the screen tier is `ScreenTracer::trace`; the escalation is `TieredTracer::trace`.
// Each is written in the order the C++ writes it, for the reason gi_gpu_common.slang gives, and
// `render.gi_gpu_tracing` runs both over one scene and compares.
//
// THE WORLD TIER IS A TYPE PARAMETER. `IGiWorldTier` has one static `trace`, the software tier
// implements it here over the uploaded field, and the hardware tier implements it in
// gi_hardware.slang over a ray query — so the gather and the escalation are written once, and the
// entry points that would need a ray query are the only ones that declare the top level.

#include "gi_gpu_common.slang"

// --- The probe set's constant words: `GpuRadianceCache` in gpu_radiance_cache.cpp -----------------

static const uint kGpCounts = 0u;        // encoding, floats per probe, rays per probe, selected
static const uint kGpGather = 1u;        // max ray distance, visibility bias, levels, half extent
static const uint kGpSkyZenith = 2u;     // xyz zenith, w intensity
static const uint kGpSkyHorizon = 3u;    // xyz horizon
static const uint kGpSkyGround = 4u;     // xyz ground
static const uint kGpLayout = 5u;        // base spacing, query frame, window edge, query count
static const uint kGpCentres = 6u;       // one int4 per level: the window's centre cell
static const uint kGpScreen = 14u;       // width, height, max steps, thickness
static const uint kGpScreenHzb = 15u;    // hzb levels, use hzb, reach cap, escalation threshold
static const uint kGpWorldToView = 16u;  // four columns
static const uint kGpWorldToClip = 20u;  // four columns
static const uint kGpBudget = 24u;       // world rays, escalations, tier ray count, cache frame
static const uint kGpTier = 25u;         // unused, software base, hardware base, unused
static const uint kGpHzbLevels = 26u;    // one uint4 per pyramid level: offset, width, height

static const uint kGpMaxPayload = 48u;
static const uint kGpResultVec4s = 14u;  // 48 payload floats, 6 axis distances, error, 0
static const uint kGpNoProbe = 0xFFFFFFFFu;

/// `ProbeEncoding`.
static const uint kGpSphericalHarmonics = 0u;
static const uint kGpOctahedral = 1u;
static const uint kGpSphericalGaussian = 2u;

/// `RadianceSource`.
static const uint kGpSourceNone = 0u;
static const uint kGpSourceScreen = 5u;
static const uint kGpSourceSoftware = 6u;
static const uint kGpSourceHardware = 7u;
static const uint kGpSourceSky = 9u;

/// The counters a dispatch accumulates with atomics: `GpuTierCounters` in gpu_tracing.h.
static const uint kGpCountWorldRays = 0u;
static const uint kGpCountEscalations = 1u;
static const uint kGpCountRefusals = 2u;
static const uint kGpCountDepthReads = 3u;
static const uint kGpCountScreenRays = 4u;
static const uint kGpCountSoftwareRays = 5u;
static const uint kGpCountHardwareRays = 6u;
static const uint kGpCountSkyAnswers = 7u;

struct GiProbeSet {
    /// 64 uint4 words, the layout above.
    StructuredBuffer<uint4> constants;
    /// Four float4 per probe: [position, level], [last update frame bits, valid, live, error], and the
    /// six axis distances in [2].xyzw and [3].xy. The host writes a probe whose revision moved;
    /// a gather writes the last three of its own probe in place.
    RWStructuredBuffer<float4> probes;
    /// `encoding_traits().floats_per_probe` floats per probe, in place.
    RWStructuredBuffer<float> payloads;
    /// The probe handles one update gathers, in `RadianceCache::select`'s order.
    StructuredBuffer<uint> selection;
    /// Two float4 per gather ray: [the sequence's direction, 0], [its normalised form, 0].
    StructuredBuffer<float4> directions;
    /// `kGpResultVec4s` float4 per selected probe.
    RWStructuredBuffer<float4> results;
    /// One word per cell of every level's window, toroidally addressed: a probe handle, or none.
    StructuredBuffer<uint> layout;
    /// Two float4 per query: [position, 0], [normal, 0].
    StructuredBuffer<float4> queries;
    /// One float4 per query: [radiance, confidence].
    RWStructuredBuffer<float4> samples;
    /// Two float4 per tier ray: [origin, max distance], [direction, 0].
    StructuredBuffer<float4> tierRays;
    /// Two float4 per tier ray: [radiance, confidence], [hit distance, source, depth reads, 0].
    RWStructuredBuffer<float4> tierResults;
    /// Linear view depth per pixel, row-major; zero or negative is sky.
    StructuredBuffer<float> screenDepth;
    /// Last frame's lit colour per pixel.
    StructuredBuffer<float4> screenColour;
    /// The NEAREST linear depth over each texel's footprint, levels concatenated, sky as +inf.
    RWStructuredBuffer<float> screenHzb;
    /// `GpuTierCounters`, accumulated with atomics.
    RWStructuredBuffer<uint> counters;
};

[[vk::binding(0, 1)]] ParameterBlock<GiProbeSet> giProbes;

// --- Small things -------------------------------------------------------------------------------

float gpClamp(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

float3 gpMax0(float3 v) {
    return float3(max(0.0f, v.x), max(0.0f, v.y), max(0.0f, v.z));
}

static const float3 kGpAxes[6] = {
    float3(1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f),
    float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f, 0.0f, -1.0f),
};

/// `SkyTerm::radiance` over the probe set's own sky — the radiance cache's `ProbeUpdateContext::sky`,
/// which need not be the card gather's.
float3 gpSky(float3 direction) {
    const float4 zenith = asfloat(giProbes.constants[kGpSkyZenith]);
    const float3 horizon = asfloat(giProbes.constants[kGpSkyHorizon].xyz);
    const float3 ground = asfloat(giProbes.constants[kGpSkyGround].xyz);
    const float3 unit = giNormalisedOr(direction, float3(0.0f, 1.0f, 0.0f));
    if (unit.y < 0.0f) {
        return ground * zenith.w;
    }
    const float t = gpClamp(pow(unit.y, 0.45f), 0.0f, 1.0f);
    return (horizon + ((zenith.xyz - horizon) * t)) * zenith.w;
}

// --- The encodings: `encode_sample`, `normalise_payload`, `decode_payload` -----------------------

static const uint kGpOctEdge = 4u;

/// `octahedral_direction` in radiance_cache.cpp.
float3 gpOctahedralDirection(uint texel) {
    const uint x = texel % kGpOctEdge;
    const uint y = texel / kGpOctEdge;
    const float u = (float(x) / float(kGpOctEdge - 1u) * 2.0f) - 1.0f;
    const float v = (float(y) / float(kGpOctEdge - 1u) * 2.0f) - 1.0f;
    float3 direction = float3(u, v, 1.0f - abs(u) - abs(v));
    if (direction.z < 0.0f) {
        const float fx = (1.0f - abs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
        const float fy = (1.0f - abs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
        direction.x = fx;
        direction.y = fy;
    }
    return giNormalisedOr(direction, float3(0.0f, 0.0f, 1.0f));
}

/// `octahedral_uv` in radiance_cache.cpp.
void gpOctahedralUv(float3 direction, out float u, out float v) {
    const float sum = abs(direction.x) + abs(direction.y) + abs(direction.z);
    const float3 p = sum > 0.0f ? direction / sum : float3(0.0f, 0.0f, 1.0f);
    float px = p.x;
    float py = p.y;
    if (p.z < 0.0f) {
        const float fx = (1.0f - abs(p.y)) * (p.x >= 0.0f ? 1.0f : -1.0f);
        const float fy = (1.0f - abs(p.x)) * (p.y >= 0.0f ? 1.0f : -1.0f);
        px = fx;
        py = fy;
    }
    u = gpClamp((px * 0.5f) + 0.5f, 0.0f, 1.0f) * float(kGpOctEdge - 1u);
    v = gpClamp((py * 0.5f) + 0.5f, 0.0f, 1.0f) * float(kGpOctEdge - 1u);
}

void gpEncode(uint encoding, inout float payload[kGpMaxPayload], float3 direction, float3 radiance) {
    if (encoding == kGpSphericalHarmonics) {
        const float kY0 = 0.282095f;
        const float kY1 = 0.488603f;
        const float basis[4] = { kY0, kY1 * direction.y, kY1 * direction.z, kY1 * direction.x };
        for (uint c = 0u; c < 4u; ++c) {
            const float scale = basis[c] * 1.0f;
            payload[(c * 3u) + 0u] += radiance.x * scale;
            payload[(c * 3u) + 1u] += radiance.y * scale;
            payload[(c * 3u) + 2u] += radiance.z * scale;
        }
        return;
    }
    if (encoding == kGpOctahedral) {
        for (uint texel = 0u; texel < kGpOctEdge * kGpOctEdge; ++texel) {
            const float cosine = max(0.0f, dot(gpOctahedralDirection(texel), direction));
            if (cosine <= 0.0f) {
                continue;
            }
            const float scale = cosine * 1.0f;
            payload[(texel * 3u) + 0u] += radiance.x * scale;
            payload[(texel * 3u) + 1u] += radiance.y * scale;
            payload[(texel * 3u) + 2u] += radiance.z * scale;
        }
        return;
    }
    for (uint lobe = 0u; lobe < 6u; ++lobe) {
        const float cosine = max(0.0f, dot(kGpAxes[lobe], direction));
        if (cosine <= 0.0f) {
            continue;
        }
        const float scale = cosine * 1.0f;
        payload[(lobe * 3u) + 0u] += radiance.x * scale;
        payload[(lobe * 3u) + 1u] += radiance.y * scale;
        payload[(lobe * 3u) + 2u] += radiance.z * scale;
    }
}

void gpNormalise(uint encoding, uint floats, inout float payload[kGpMaxPayload], float totalWeight) {
    if (totalWeight <= 0.0f) {
        return;
    }
    const float pi = 3.14159265358979f;
    const float scale = encoding == kGpSphericalHarmonics ? (4.0f * pi) / totalWeight
                                                         : pi / totalWeight;
    for (uint index = 0u; index < floats; ++index) {
        payload[index] *= scale;
    }
}

/// `decode_payload`, over a payload in registers.
float3 gpDecode(uint encoding, float payload[kGpMaxPayload], float3 direction) {
    if (encoding == kGpSphericalHarmonics) {
        const float kY0 = 0.282095f;
        const float kY1 = 0.488603f;
        const float kA0 = 1.0f;
        const float kA1 = 2.0f / 3.0f;
        const float basis[4] = { kY0 * kA0, kY1 * kA1 * direction.y, kY1 * kA1 * direction.z,
                                 kY1 * kA1 * direction.x };
        float3 result = float3(0.0f, 0.0f, 0.0f);
        for (uint c = 0u; c < 4u; ++c) {
            result.x += payload[(c * 3u) + 0u] * basis[c];
            result.y += payload[(c * 3u) + 1u] * basis[c];
            result.z += payload[(c * 3u) + 2u] * basis[c];
        }
        return gpMax0(result);
    }
    if (encoding == kGpOctahedral) {
        float u = 0.0f;
        float v = 0.0f;
        gpOctahedralUv(direction, u, v);
        const uint x0 = uint(u);
        const uint y0 = uint(v);
        const uint x1 = min(x0 + 1u, kGpOctEdge - 1u);
        const uint y1 = min(y0 + 1u, kGpOctEdge - 1u);
        const float tx = u - float(x0);
        const float ty = v - float(y0);
        const uint t00 = (y0 * kGpOctEdge) + x0;
        const uint t10 = (y0 * kGpOctEdge) + x1;
        const uint t01 = (y1 * kGpOctEdge) + x0;
        const uint t11 = (y1 * kGpOctEdge) + x1;
        const float3 f00 = float3(payload[t00 * 3u], payload[(t00 * 3u) + 1u], payload[(t00 * 3u) + 2u]);
        const float3 f10 = float3(payload[t10 * 3u], payload[(t10 * 3u) + 1u], payload[(t10 * 3u) + 2u]);
        const float3 f01 = float3(payload[t01 * 3u], payload[(t01 * 3u) + 1u], payload[(t01 * 3u) + 2u]);
        const float3 f11 = float3(payload[t11 * 3u], payload[(t11 * 3u) + 1u], payload[(t11 * 3u) + 2u]);
        const float3 top = f00 + ((f10 - f00) * tx);
        const float3 bottom = f01 + ((f11 - f01) * tx);
        return top + ((bottom - top) * ty);
    }
    float3 result = float3(0.0f, 0.0f, 0.0f);
    float total = 0.0f;
    for (uint lobe = 0u; lobe < 6u; ++lobe) {
        const float cosine = max(0.0f, dot(kGpAxes[lobe], direction));
        if (cosine <= 0.0f) {
            continue;
        }
        result = result + (float3(payload[lobe * 3u], payload[(lobe * 3u) + 1u],
                                  payload[(lobe * 3u) + 2u]) * cosine);
        total += cosine;
    }
    return total > 0.0f ? result / total : float3(0.0f, 0.0f, 0.0f);
}

/// A stored probe's payload into registers.
void gpLoadPayload(uint probe, uint floats, out float payload[kGpMaxPayload]) {
    for (uint index = 0u; index < kGpMaxPayload; ++index) {
        payload[index] = index < floats ? giProbes.payloads[(probe * floats) + index] : 0.0f;
    }
}

// --- The world tiers: `SceneTracer::trace` behind `IGiWorldTier` ----------------------------------

struct GiWorldHit {
    bool hit;
    float t;
    float3 position;
    float3 normal;
    /// `SceneHit::confidence`: the tier's own certainty, before the surface cache's staleness.
    float confidence;
};

GiWorldHit gpMiss() {
    GiWorldHit miss;
    miss.hit = false;
    miss.t = 0.0f;
    miss.position = float3(0.0f, 0.0f, 0.0f);
    miss.normal = float3(0.0f, 1.0f, 0.0f);
    miss.confidence = 1.0f;
    return miss;
}

interface IGiWorldTier {
    /// Trace `direction` (already normalised by the caller, as both host tiers normalise it).
    static GiWorldHit trace(float3 origin, float3 direction, float maxDistance);
    /// The `RadianceSource` a hit from this tier reports.
    static uint source();
}

/// `SoftwareTracer::trace`: sphere trace the uploaded field from `t_min` zero.
struct GiSoftwareTier : IGiWorldTier {
    static GiWorldHit trace(float3 origin, float3 direction, float maxDistance) {
        const GiTraceHit traced = giSphereTrace(origin, direction, maxDistance, 0.0f);
        if (!traced.hit) {
            return gpMiss();
        }
        GiWorldHit hit;
        hit.hit = true;
        hit.t = traced.t;
        hit.position = traced.position;
        hit.normal = traced.normal;
        // The grazing scale is the field's (`GpuGiScene::upload_field`), the base the tier's.
        const float grazing = asfloat(giScene.constants[kGiShadowEnabled].z);
        const float4 tier = asfloat(giProbes.constants[kGpTier]);
        hit.confidence = tier.y * gpClamp(traced.closest / max(grazing, 1.0e-4f), 0.25f, 1.0f);
        return hit;
    }
    static uint source() {
        return kGpSourceSoftware;
    }
}

// --- The gather: `RadianceCache::gather_probe` ---------------------------------------------------

/// The most rays one probe gathers: one per lane of the workgroup that gathers it.
static const uint kGpMaxProbeRays = 64u;

/// Each ray's answer, traced by its own lane and folded in by lane 0 in RAY ORDER — the order the
/// host folds them in, so the payload's sums are the host's sums.
groupshared float3 gpRayRadiance[kGpMaxProbeRays];
groupshared float gpRayDistance[kGpMaxProbeRays];

/// One selected probe's gather through tier `T`, by one workgroup: lane `ray` traces ray `ray`, and
/// lane 0 encodes them. The result record is what `GpuRadianceCache::retire` hands to
/// `RadianceCache::apply`; the payload is also written in place, because nothing else in a gather
/// dispatch reads another probe's payload.
///
/// ONE WORKGROUP PER PROBE, NOT ONE LANE: a probe's rays are independent and the host's are only
/// SUMMED in order, so tracing them side by side and summing them in order is the host's answer at a
/// thirty-second of the latency. One lane per probe was 1.5 ms for 64 probes of 32 rays on the
/// RTX 5060 — every ray's march waiting on the last.
void gpGatherProbe<T : IGiWorldTier>(uint index, uint lane) {
    const uint4 counts = giProbes.constants[kGpCounts];
    if (index >= counts.w) {
        return;  // uniform over the workgroup: the index is the group's
    }
    const uint encoding = counts.x;
    const uint floats = counts.y;
    const uint rays = min(max(1u, counts.z), kGpMaxProbeRays);
    const float maxDistance = asfloat(giProbes.constants[kGpGather].x);
    const uint probe = giProbes.selection[index];
    const float3 position = giProbes.probes[probe * 4u].xyz;

    if (lane < rays) {
        const float3 direction = giProbes.directions[lane * 2u].xyz;
        const float3 unit = giProbes.directions[(lane * 2u) + 1u].xyz;
        float3 radiance = gpSky(direction);
        float hitDistance = maxDistance;
        const GiWorldHit hit = T.trace(position, unit, maxDistance);
        if (hit.hit) {
            hitDistance = hit.t;
            const uint card = giFindCard(hit.position, hit.normal);
            // Nothing in the surface cache covers the hit: black, as the host says, and honest.
            radiance = card != 0xFFFFFFFFu ? giScene.cardState[card * 2u].xyz
                                          : float3(0.0f, 0.0f, 0.0f);
        }
        gpRayRadiance[lane] = radiance;
        gpRayDistance[lane] = hitDistance;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0u) {
        return;
    }

    float data[kGpMaxPayload];
    gpLoadPayload(probe, floats, data);
    float3 previous = float3(0.0f, 0.0f, 0.0f);
    for (uint axis = 0u; axis < 6u; ++axis) {
        previous = previous + gpDecode(encoding, data, kGpAxes[axis]);
    }
    for (uint i = 0u; i < kGpMaxPayload; ++i) {
        data[i] = 0.0f;
    }
    float axisDistance[6];
    float axisWeight[6];
    for (uint a = 0u; a < 6u; ++a) {
        axisDistance[a] = 0.0f;
        axisWeight[a] = 0.0f;
    }

    float totalWeight = 0.0f;
    for (uint ray = 0u; ray < rays; ++ray) {
        const float3 direction = giProbes.directions[ray * 2u].xyz;
        gpEncode(encoding, data, direction, gpRayRadiance[ray]);
        totalWeight += 1.0f;
        for (uint a = 0u; a < 6u; ++a) {
            const float cosine = max(0.0f, dot(kGpAxes[a], direction));
            axisDistance[a] += gpRayDistance[ray] * cosine;
            axisWeight[a] += cosine;
        }
    }
    gpNormalise(encoding, floats, data, totalWeight);

    float3 current = float3(0.0f, 0.0f, 0.0f);
    for (uint axis = 0u; axis < 6u; ++axis) {
        current = current + gpDecode(encoding, data, kGpAxes[axis]);
    }
    const float magnitude = length(previous) + length(current);
    const float error = magnitude > 1.0e-5f ? length(current - previous) / magnitude : 0.0f;

    const uint record = index * kGpResultVec4s;
    for (uint v = 0u; v < 12u; ++v) {
        giProbes.results[record + v] =
            float4(data[v * 4u], data[(v * 4u) + 1u], data[(v * 4u) + 2u], data[(v * 4u) + 3u]);
    }
    float distances[6];
    for (uint a = 0u; a < 6u; ++a) {
        distances[a] = axisWeight[a] > 0.0f ? axisDistance[a] / axisWeight[a] : maxDistance;
    }
    giProbes.results[record + 12u] = float4(distances[0], distances[1], distances[2], distances[3]);
    giProbes.results[record + 13u] = float4(distances[4], distances[5], error, asfloat(probe));
    for (uint f = 0u; f < floats; ++f) {
        giProbes.payloads[(probe * floats) + f] = data[f];
    }
    // The error rides in the record's last word, as `GpuRadianceCache::upload_payload` writes it
    // for a host gather: what `GiProbeError` draws.
    giProbes.probes[(probe * 4u) + 1u] = float4(asfloat(giDispatch.frame), 1.0f, 1.0f, error);
    giProbes.probes[(probe * 4u) + 2u] =
        float4(distances[0], distances[1], distances[2], distances[3]);
    giProbes.probes[(probe * 4u) + 3u] = float4(distances[4], distances[5], 0.0f, 0.0f);
}

// --- The query: `RadianceCache::sample` ------------------------------------------------------------

int gpFloorDiv(float value, float divisor) {
    return int(floor(value / divisor));
}

/// `RadianceCache::visibility_weight`, over the axis distances the probe record carries.
float gpVisibilityWeight(uint probe, float3 position, float3 query) {
    const float3 offset = query - position;
    const float span = length(offset);
    if (span <= 1.0e-4f) {
        return 1.0f;
    }
    const float3 direction = offset / span;
    const float4 near = giProbes.probes[(probe * 4u) + 2u];
    const float4 far = giProbes.probes[(probe * 4u) + 3u];
    const float distances[6] = { near.x, near.y, near.z, near.w, far.x, far.y };
    float expected = 0.0f;
    float weight = 0.0f;
    for (uint axis = 0u; axis < 6u; ++axis) {
        const float cosine = max(0.0f, dot(kGpAxes[axis], direction));
        expected += distances[axis] * cosine;
        weight += cosine;
    }
    if (weight <= 0.0f) {
        return 1.0f;
    }
    expected /= weight;
    // The query is further from the probe than the probe's own view of the world in that
    // direction: something is between them. This is the wall.
    const float slack = asfloat(giProbes.constants[kGpGather].y);
    if (span > expected + slack) {
        return 0.0f;
    }
    float visible = gpClamp((expected + slack - span) / max(slack, 1.0e-4f), 0.0f, 1.0f);
    // Every axis the query lies well along must reach it on its own (the host's comment says why).
    for (uint axis = 0u; axis < 6u; ++axis) {
        const float cosine = dot(kGpAxes[axis], direction);
        if (cosine < 0.5f) {
            continue;
        }
        const float along = span * cosine;
        visible = min(visible, gpClamp((distances[axis] + slack - along) / max(slack, 1.0e-4f),
                                       0.0f, 1.0f));
    }
    return visible;
}

/// The window cell of a level that holds `cell`, toroidally addressed, or none outside the window.
uint gpLayoutProbe(uint level, int3 cell) {
    const int3 centre = asint(giProbes.constants[kGpCentres + level].xyz);
    const int half = int(asuint(giProbes.constants[kGpGather].w));
    if (any(cell < centre - int3(half, half, half)) || any(cell > centre + int3(half, half, half))) {
        return kGpNoProbe;
    }
    const int edge = int(asuint(giProbes.constants[kGpLayout].z));
    const uint cells = uint(edge * edge * edge);
    const uint index = ((uint(giWrap(cell.z, edge)) * uint(edge) + uint(giWrap(cell.y, edge))) *
                        uint(edge)) +
                       uint(giWrap(cell.x, edge));
    return giProbes.layout[(level * cells) + index];
}

/// `RadianceCache::sample`. The host visits the probes its spatial index returns for a box of the
/// coarsest reach; every probe that box can return and the per-probe test then keeps lies within
/// one and a half spacings of the query, so here each level's cells within that radius are visited
/// instead. Which probes contribute is identical; the order they are summed in is not, which is
/// the only difference and a last-bit one.
float4 gpSample(float3 position, float3 normal, uint frame) {
    const uint4 counts = giProbes.constants[kGpCounts];
    const uint encoding = counts.x;
    const uint floats = counts.y;
    const uint levels = asuint(giProbes.constants[kGpGather].z);
    const float base = asfloat(giProbes.constants[kGpLayout].x);
    float3 total = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;
    float oldest = 0.0f;
    for (uint level = 0u; level < levels; ++level) {
        const float spacing = base * float(1u << level);
        const float radius = spacing * 1.5f;
        const int3 lo = int3(gpFloorDiv(position.x - radius, spacing),
                             gpFloorDiv(position.y - radius, spacing),
                             gpFloorDiv(position.z - radius, spacing));
        const int3 hi = int3(gpFloorDiv(position.x + radius, spacing),
                             gpFloorDiv(position.y + radius, spacing),
                             gpFloorDiv(position.z + radius, spacing));
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int y = lo.y; y <= hi.y; ++y) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    const uint probe = gpLayoutProbe(level, int3(x, y, z));
                    if (probe == kGpNoProbe) {
                        continue;
                    }
                    const float4 record = giProbes.probes[(probe * 4u) + 1u];
                    if (record.y == 0.0f || record.z == 0.0f) {
                        continue;  // not valid, or not live
                    }
                    const float3 at = giProbes.probes[probe * 4u].xyz;
                    const float3 offset = position - at;
                    const float span = length(offset);
                    if (span > radius) {
                        continue;
                    }
                    float weight = 1.0f - gpClamp(span / radius, 0.0f, 1.0f);
                    if (span > 1.0e-4f) {
                        const float facing = dot(normal, -offset / span);
                        weight *= gpClamp((facing + 1.0f) * 0.5f, 0.0f, 1.0f);
                    }
                    weight *= gpVisibilityWeight(probe, at, position);
                    if (weight <= 0.0f) {
                        continue;
                    }
                    float data[kGpMaxPayload];
                    gpLoadPayload(probe, floats, data);
                    total = total + (gpDecode(encoding, data, normal) * weight);
                    totalWeight += weight;
                    const uint last = asuint(record.x);
                    const float age = frame > last ? float(frame - last) : 0.0f;
                    oldest = max(oldest, age);
                }
            }
        }
    }
    if (totalWeight <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    // "WHEN a region has been invalidated and not yet re-solved THEN samples from it SHALL carry
    // reduced confidence."
    const float staleness = gpClamp(oldest / 240.0f, 0.0f, 1.0f);
    return float4(total / totalWeight, gpClamp(1.0f - (staleness * 0.75f), 0.0f, 1.0f));
}

// --- The screen tier: `ScreenTracer::trace`, accelerated by a nearest-depth pyramid ---------------
//
// THE PYRAMID IS THE DUAL OF THE ENGINE'S HZB, AND NEITHER CAN DO THE OTHER'S JOB. `cy::rendering-
// hzb` keeps the FURTHEST depth of each footprint, which is what proves an object hidden; a march
// needs to prove the opposite — that a stretch of the ray is IN FRONT of everything it crosses, so
// every step along it would `continue` — and that takes the NEAREST depth. A furthest-depth pyramid
// can only prove a stretch behind everything, which ends a march at its first step there anyway.
//
// THE SKIP CHANGES NO ANSWER, AND THAT IS WHY IT IS ALLOWED. The oracle steps at fixed parameters
// `t = step * k`; the accelerated march visits the same parameters and skips a run of them only
// when the run provably `continue`s at every one (`gpScreenRunEmpty`): both ends on screen with a pixel's margin, both
// in front of the eye, and the furthest ray depth of the run nearer than the nearest buffer depth
// over the pixels the run can touch. Ray depth is affine in `t` and the projection of a segment in
// front of the eye is a segment, so the run's pixels are inside its ends' bounding box (widened by
// a pixel for rounding) and its depths below the larger end's. Every step the skip does not prove
// is stepped exactly as the oracle steps it. `render.gi_gpu_tracing` holds the accelerated march to
// the unaccelerated one bit for bit.

float3 gpTransformPoint(uint first, float3 p) {
    const float4 c0 = asfloat(giProbes.constants[first]);
    const float4 c1 = asfloat(giProbes.constants[first + 1u]);
    const float4 c2 = asfloat(giProbes.constants[first + 2u]);
    const float4 c3 = asfloat(giProbes.constants[first + 3u]);
    return (c0 * p.x + c1 * p.y + c2 * p.z + c3).xyz;
}

float3 gpProjectPoint(uint first, float3 p) {
    const float4 c0 = asfloat(giProbes.constants[first]);
    const float4 c1 = asfloat(giProbes.constants[first + 1u]);
    const float4 c2 = asfloat(giProbes.constants[first + 2u]);
    const float4 c3 = asfloat(giProbes.constants[first + 3u]);
    const float4 clip = c0 * p.x + c1 * p.y + c2 * p.z + c3 * 1.0f;
    if (clip.w == 0.0f) {
        return clip.xyz;
    }
    return clip.xyz * (1.0f / clip.w);
}

/// `edge_confidence` in tracing.cpp.
float gpEdgeConfidence(float x, float y, uint width, uint height) {
    const float border = 0.1f;
    const float u = x / float(width);
    const float v = y / float(height);
    const float nearest = min(min(u, 1.0f - u), min(v, 1.0f - v));
    return gpClamp(nearest / border, 0.0f, 1.0f);
}

struct GiScreenStep {
    /// The point is behind the eye or off the screen: the march ends with no confidence.
    bool outside;
    float x;
    float y;
    float depth;
};

GiScreenStep gpScreenStep(float3 point, uint width, uint height) {
    GiScreenStep step;
    const float3 inView = gpTransformPoint(kGpWorldToView, point);
    step.depth = -inView.z;
    const float3 clip = gpProjectPoint(kGpWorldToClip, point);
    step.x = ((clip.x * 0.5f) + 0.5f) * float(width);
    step.y = (0.5f - (clip.y * 0.5f)) * float(height);
    step.outside = step.depth <= 0.0f || step.x < 0.0f || step.y < 0.0f ||
                   step.x >= float(width) || step.y >= float(height);
    return step;
}

/// The nearest buffer depth over the pixel box [x0, x1] x [y0, y1], from the coarsest pyramid level
/// whose texels cover it in at most two by two. +inf where every pixel is sky.
float gpNearestDepth(int x0, int y0, int x1, int y1, inout uint reads) {
    const uint levels = giProbes.constants[kGpScreenHzb].x;
    const int span = max(x1 - x0, y1 - y0);
    uint level = 0u;
    while (level + 1u < levels && (1 << int(level)) <= span) {
        ++level;
    }
    const uint4 shape = giProbes.constants[kGpHzbLevels + level];
    const int lx0 = x0 >> int(level);
    const int ly0 = y0 >> int(level);
    const int lx1 = min(x1 >> int(level), int(shape.y) - 1);
    const int ly1 = min(y1 >> int(level), int(shape.z) - 1);
    float nearest = 1.0e30f;
    for (int y = ly0; y <= ly1; ++y) {
        for (int x = lx0; x <= lx1; ++x) {
            nearest = min(nearest, giProbes.screenHzb[shape.x + (uint(y) * shape.y) + uint(x)]);
            reads += 1u;
        }
    }
    return nearest;
}

/// Whether the `run` steps after `index` provably `continue`: both ends on screen with a pixel's
/// margin and in front of the eye, and the run's furthest ray depth nearer than the nearest buffer
/// depth over every pixel it can touch.
bool gpScreenRunEmpty(float3 origin, float3 direction, float step, uint index, uint run,
                      GiScreenStep here, uint width, uint height, inout uint reads) {
    const float t = step * float(index + run);
    const GiScreenStep there = gpScreenStep(origin + (direction * t), width, height);
    const float margin = 1.0f;
    const bool inside =
        !there.outside && there.x >= margin && there.y >= margin &&
        there.x < float(width) - margin && there.y < float(height) - margin &&
        here.x >= margin && here.y >= margin && here.x < float(width) - margin &&
        here.y < float(height) - margin;
    if (!inside) {
        return false;
    }
    const int x0 = int(floor(min(here.x, there.x))) - 1;
    const int y0 = int(floor(min(here.y, there.y))) - 1;
    const int x1 = int(floor(max(here.x, there.x))) + 1;
    const int y1 = int(floor(max(here.y, there.y))) + 1;
    const float nearest = gpNearestDepth(max(x0, 0), max(y0, 0), min(x1, int(width) - 1),
                                         min(y1, int(height) - 1), reads);
    // A relative margin, so a last-bit difference between the affine depth and a stepped one can
    // never turn a proven `continue` into a step that would have hit.
    const float furthest = max(here.depth, there.depth);
    return furthest < nearest - (abs(nearest) * 1.0e-4f) - 1.0e-4f;
}

struct GiTierSample {
    float3 radiance;
    float confidence;
    float hitDistance;
    uint source;
};

GiTierSample gpTierSample(uint source) {
    GiTierSample sample;
    sample.radiance = float3(0.0f, 0.0f, 0.0f);
    sample.confidence = 0.0f;
    sample.hitDistance = -1.0f;
    sample.source = source;
    return sample;
}

/// `ScreenTracer::trace`. `accelerated` takes the pyramid's provable skips; the answer is the same.
GiTierSample gpScreenTrace(float3 origin, float3 direction, float maxDistance, bool accelerated,
                           inout uint reads) {
    GiTierSample sample = gpTierSample(kGpSourceScreen);
    const uint4 screen = giProbes.constants[kGpScreen];
    const uint width = screen.x;
    const uint height = screen.y;
    if (width == 0u || height == 0u) {
        return sample;
    }
    const float thickness = asfloat(screen.w);
    const uint steps = max(1u, screen.z);
    const float step = maxDistance / float(steps);
    // The run the next skip tries: doubled after a run is proven empty, halved after one is not,
    // so a ray crossing open space takes long strides and one grazing a surface stops trying.
    uint run = 8u;
    uint index = 1u;
    while (index <= steps) {
        const float t = step * float(index);
        const float3 point = origin + (direction * t);
        const GiScreenStep here = gpScreenStep(point, width, height);
        if (here.outside) {
            // Behind the eye, or "WHEN a traced ray exits the viewport THEN confidence SHALL fall
            // toward zero near the edge and a higher tier or cached source SHALL take over".
            sample.confidence = 0.0f;
            return sample;
        }
        const uint pixel = (uint(here.y) * width) + uint(here.x);
        const float bufferDepth = giProbes.screenDepth[pixel];
        reads += 1u;
        // How far in front of the buffer this step is. A skip is tried only where there is room
        // for one: a step about to meet the surface would only pay for a test that fails.
        const float gap = bufferDepth > 0.0f ? bufferDepth - here.depth : 1.0e30f;
        if (bufferDepth > 0.0f) {
            const float behind = here.depth - bufferDepth;
            if (behind > 0.0f) {
                if (behind > thickness) {
                    // Far behind whatever the buffer shows: not a hit and not a miss.
                    sample.confidence = 0.0f;
                    return sample;
                }
                sample.radiance = giProbes.screenColour[pixel].xyz;
                sample.hitDistance = t;
                const float agreement = 1.0f - (behind / max(thickness, 1.0e-4f));
                sample.confidence = gpClamp(
                    gpEdgeConfidence(here.x, here.y, width, height) * agreement * 0.92f, 0.0f, 1.0f);
                return sample;
            }
        }
        uint skip = 0u;
        if (accelerated && index + 1u < steps && gap > thickness * 2.0f) {
            const uint limit = min(run, steps - index);
            if (gpScreenRunEmpty(origin, direction, step, index, limit, here, width, height,
                                 reads)) {
                skip = limit;
                run = min(run * 2u, 32u);
            } else {
                run = max(run / 2u, 2u);
            }
        }
        index += 1u + skip;
    }
    // On screen the whole way and nothing met: a screen tracer cannot prove a miss.
    sample.confidence = 0.2f;
    sample.hitDistance = -1.0f;
    return sample;
}

// --- The world tiers' radiance: `SoftwareTracer::radiance` and `HardwareTracer::radiance` ---------

/// What a world hit resolves to: a surface-cache lookup, never a material.
GiTierSample gpWorldRadiance(GiWorldHit hit, float3 direction, uint source, uint cacheFrame) {
    GiTierSample sample = gpTierSample(source);
    if (!hit.hit) {
        sample.source = kGpSourceSky;
        sample.radiance = gpSky(direction);
        sample.confidence = 1.0f;
        sample.hitDistance = -1.0f;
        return sample;
    }
    sample.hitDistance = hit.t;
    const uint card = giFindCard(hit.position, hit.normal);
    if (card != 0xFFFFFFFFu) {
        sample.radiance = giScene.cardState[card * 2u].xyz;
        const uint last = asuint(giScene.cardState[(card * 2u) + 1u].w);
        const float age = cacheFrame > last ? float(cacheFrame - last) : 0.0f;
        // "Stale cache is distrusted."
        const float staleness = gpClamp(age / 240.0f, 0.0f, 1.0f);
        sample.confidence = hit.confidence * (1.0f - (staleness * 0.6f));
        return sample;
    }
    sample.confidence = hit.confidence * 0.2f;
    return sample;
}

/// One ray through world tier `T` alone: what `TieredTracer` issues when it escalates.
GiTierSample gpWorldTrace<T : IGiWorldTier>(float3 origin, float3 unit, float reach) {
    const uint cacheFrame = giProbes.constants[kGpBudget].w;
    const GiWorldHit hit = T.trace(origin, unit, reach);
    return gpWorldRadiance(hit, unit, T.source(), cacheFrame);
}

// --- Counting without contention ------------------------------------------------------------------
//
// A batch is millions of rays and its counters are eight words. One atomic per ray on one word is
// a queue two million long — it was most of a 1080p batch's time — so every count is summed over
// the wave first and added once per wave, and a budget ticket is one atomic per wave plus the
// lane's place among the wave's requests.

void gpCount(uint counter, uint amount) {
    const uint total = WaveActiveSum(amount);
    if (WaveIsFirstLane() && total != 0u) {
        InterlockedAdd(giProbes.counters[counter], total);
    }
}

/// The calling lane's place in the order requests for `counter` were granted: the wave's base, from
/// one atomic, plus the lane's rank among the wave's active lanes.
uint gpTicket(uint counter) {
    // Sums rather than ballot counts: Metal has the prefix sum and not the prefix ballot count.
    const uint count = WaveActiveSum(1u);
    const uint rank = WavePrefixSum(1u);
    uint base = 0u;
    if (WaveIsFirstLane()) {
        InterlockedAdd(giProbes.counters[counter], count, base);
    }
    return WaveReadLaneFirst(base) + rank;
}

/// `TieredTracer::trace`: the screen first, the world tier only when the screen's confidence is
/// below the threshold AND the budget allows, and the two BLENDED by the screen's confidence.
/// The budget is two atomic counters, so which rays a spent budget refuses depends on the order
/// threads reach them; how many it refuses does not.
GiTierSample gpTieredTrace<T : IGiWorldTier>(float3 origin, float3 direction, float maxDistance,
                                             bool screenPresent, inout uint reads) {
    const float3 unit = giNormalisedOr(direction, float3(0.0f, 0.0f, -1.0f));
    const float4 hzb = asfloat(giProbes.constants[kGpScreenHzb]);
    const float reach = min(maxDistance, hzb.z);
    const float threshold = hzb.w;
    const bool accelerated = giProbes.constants[kGpScreenHzb].y != 0u;

    GiTierSample screen = gpTierSample(kGpSourceNone);
    if (screenPresent) {
        gpCount(kGpCountScreenRays, 1u);
        screen = gpScreenTrace(origin, unit, reach, accelerated, reads);
        if (screen.confidence >= threshold) {
            return screen;
        }
    }

    // The host spends a world ray and, with a screen, an escalation together, and refuses when
    // either is spent — so the two budgets are one limit, their minimum, over one counter of
    // attempts. Issued is attempts minus refusals.
    const uint4 budget = giProbes.constants[kGpBudget];
    const uint limit = screenPresent ? min(budget.x, budget.y) : budget.x;
    const uint attempt = gpTicket(kGpCountWorldRays);
    if (attempt >= limit) {
        gpCount(kGpCountRefusals, 1u);
        return screen;
    }
    if (screenPresent) {
        gpCount(kGpCountEscalations, 1u);
    }
    gpCount(T.source() == kGpSourceHardware ? kGpCountHardwareRays : kGpCountSoftwareRays, 1u);
    const GiTierSample world = gpWorldTrace<T>(origin, unit, reach);
    gpCount(kGpCountSkyAnswers, world.source == kGpSourceSky ? 1u : 0u);
    if (!screenPresent || screen.confidence <= 0.0f) {
        return world;
    }
    GiTierSample blended;
    blended.radiance = world.radiance + ((screen.radiance - world.radiance) * screen.confidence);
    blended.confidence = max(screen.confidence, world.confidence);
    blended.source = screen.confidence >= world.confidence ? screen.source : world.source;
    blended.hitDistance =
        screen.confidence >= world.confidence ? screen.hitDistance : world.hitDistance;
    return blended;
}

void gpWriteTierResult(uint ray, GiTierSample sample, uint reads) {
    giProbes.tierResults[ray * 2u] = float4(sample.radiance, sample.confidence);
    giProbes.tierResults[(ray * 2u) + 1u] =
        float4(sample.hitDistance, asfloat(sample.source), asfloat(reads), 0.0f);
}
