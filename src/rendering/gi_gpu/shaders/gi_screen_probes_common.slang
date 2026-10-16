// SPDX-License-Identifier: MIT
// CyberGI on the device: the screen-probe final gather. Issue #35, the follow-up of stage 4.
// Include-only; the entry points are gi_screen_probes.slang (the software world tier) and
// gi_screen_probes_hardware.slang (ray queries).
//
// ================================================================================================
// EVERY FUNCTION BELOW IS A TRANSCRIPTION, AND NAMES WHAT IT TRANSCRIBES
// ================================================================================================
//
// `cy::rendering-gi`'s screen_probes.h is the oracle: `ScreenProbeGather::place`, `trace_probe`
// with its reprojection and its defensive mixture of uniform, cosine and guided directions, and
// `integrate`. `render.gi_gpu_resolve` runs both over one view and compares, probe for probe and
// pixel for pixel.
//
// THE ADAPTIVE SLOTS ARE TAKEN WITH AN ATOMIC, so which slot a cell's probe lands in depends on
// the order threads reach it; the host takes them in cell order. Nothing reads a slot except
// through its cell's entry in the table, and a probe's randomness is keyed by its cell, so the
// answer does not depend on the order.
//
// A probe is traced by ONE WORKGROUP, a lane a ray, and its estimate is summed by lane 0 in ray
// order — the host's order — as `gpGatherProbe` sums a radiance-cache probe.

#include "gi_resolve_common.slang"

/// `kScreenProbeCoefficients`, `kScreenProbeGuideEdge`, `kScreenProbeMaxRays`, `kScreenProbeNone`
/// and `kScreenStreamProbe` in screen_probes.h.
static const uint kSpCoefficients = 9u;
static const uint kSpGuideEdge = 4u;
static const uint kSpGuideCells = 16u;
static const uint kSpMaxRays = 64u;
static const uint kSpNone = 0xFFFFFFFFu;
static const uint kSpStream = 8u;
/// `kScreenStreamProbeUpdate`.
static const uint kSpUpdateStream = 9u;
/// A record: [position, depth], [normal, id bits], seven float4 of the 27 coefficients (channel
/// fastest) and the history count, [cache, cache confidence], [openness, 0, 0, 0], and seven float4
/// of the 27 coefficients the pixels integrate (`ScreenProbe::filtered`) and a zero.
static const uint kSpRecordVec4s = 18u;
static const uint kSpFilteredVec4 = 11u;
static const float kSpWeightFloor = 1.0e-4f;
/// `kHistoryMatch` in screen_probes.cpp.
static const float kSpHistoryMatch = 0.25f;
/// `kMaxGuidedShare` and `kOriginSteps`.
static const float kSpMaxGuided = 0.9f;
static const uint kSpOriginSteps = 4u;
static const float kSpGuideDensity = 16.0f / 6.28318530717958647692f;
/// `kIrradianceBands` in screen_probes.cpp.
static const float kSpIrradianceBand1 = 2.0f / 3.0f;
static const float kSpIrradianceBand2 = 0.25f;

// --- The layout: `ScreenProbeLayout` ------------------------------------------------------------

uint spWidth() { return giResolve.constants[kGrProbeLayout].x; }
uint spHeight() { return giResolve.constants[kGrProbeLayout].y; }
uint spSpacing() { return giResolve.constants[kGrProbeLayout].z; }
uint spCell() { return giResolve.constants[kGrProbeLayout].w; }
uint spTilesX() { return giResolve.constants[kGrProbeCounts].x; }
uint spTilesY() { return giResolve.constants[kGrProbeCounts].y; }
uint spCellsX() { return giResolve.constants[kGrProbeCounts].z; }
uint spCellsY() { return giResolve.constants[kGrProbeCounts].w; }
uint spUniformCount() { return giResolve.constants[kGrProbeWeights].w; }
uint spCapacity() { return giResolve.constants[kGrProbeShape].z; }
uint spRecords() { return spUniformCount() + spCapacity(); }
uint spSlot() { return giResolve.constants[kGrCounts].w; }

/// `uniform_pixel`.
uint spUniformPixel(uint tile) {
    const uint x = min(((tile % spTilesX()) * spSpacing()) + (spSpacing() / 2u), spWidth() - 1u);
    const uint y = min(((tile / spTilesX()) * spSpacing()) + (spSpacing() / 2u), spHeight() - 1u);
    return (y * spWidth()) + x;
}

/// `adaptive_pixel`.
uint spAdaptivePixel(uint cell) {
    const uint x = min(((cell % spCellsX()) * spCell()) + (spCell() / 2u), spWidth() - 1u);
    const uint y = min(((cell / spCellsX()) * spCell()) + (spCell() / 2u), spHeight() - 1u);
    return (y * spWidth()) + x;
}

uint spRecordBase(uint slot, uint record) {
    return ((slot * spRecords()) + record) * kSpRecordVec4s;
}

uint spTableIndex(uint slot, uint cell) {
    return 4u + (slot * spCellsX() * spCellsY()) + cell;
}

uint spCounter(uint slot) {
    return slot;
}

// --- The arithmetic: `screen_probe_basis`, `screen_probe_evaluate`, the weights ---------------------

void spBasis(float3 d, out float basis[9]) {
    basis[0] = 0.282095f;
    basis[1] = 0.488603f * d.y;
    basis[2] = 0.488603f * d.z;
    basis[3] = 0.488603f * d.x;
    basis[4] = 1.092548f * (d.x * d.y);
    basis[5] = 1.092548f * (d.y * d.z);
    basis[6] = 0.315392f * ((3.0f * (d.z * d.z)) - 1.0f);
    basis[7] = 1.092548f * (d.x * d.z);
    basis[8] = 0.546274f * ((d.x * d.x) - (d.y * d.y));
}

uint spBandOf(uint k) {
    return k == 0u ? 0u : (k < 4u ? 1u : 2u);
}

float3 spEvaluate(float3 coefficients[9], float3 direction, float3 band) {
    float basis[9];
    spBasis(direction, basis);
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (uint k = 0u; k < kSpCoefficients; ++k) {
        const uint b = spBandOf(k);
        const float scale = b == 0u ? band.x : (b == 1u ? band.y : band.z);
        sum = sum + (coefficients[k] * (basis[k] * scale));
    }
    return float3(max(sum.x, 0.0f), max(sum.y, 0.0f), max(sum.z, 0.0f));
}

/// `screen_probe_geometry_weight`.
float spGeometryWeight(float3 probePosition, float3 probeNormal, float3 position, float3 normal,
                       float depth, float tolerance) {
    const float plane = abs(dot(probeNormal, position - probePosition));
    const float reach = tolerance * depth;
    if (reach <= 0.0f) {
        return 0.0f;
    }
    const float near = max(0.0f, 1.0f - (plane / reach));
    float facing = max(0.0f, dot(probeNormal, normal));
    facing *= facing;
    facing *= facing;
    facing *= facing;
    return near * facing;
}

float spLuminance(float3 c) {
    return (0.2126f * c.x) + (0.7152f * c.y) + (0.0722f * c.z);
}

/// `bilinear_taps`.
struct SpTaps {
    uint index[4];
    float weight[4];
    uint count;
};

void spAxisTaps(uint coordinate, uint step, uint count, out uint base, out float fraction) {
    const float g = (float(coordinate) - float(step / 2u)) / float(step);
    if (g <= 0.0f) {
        base = 0u;
        fraction = 0.0f;
        return;
    }
    base = uint(g);
    fraction = g - float(base);
    if (base + 1u >= count) {
        base = count - 1u;
        fraction = 0.0f;
    }
}

SpTaps spBilinearTaps(uint x, uint y, uint step, uint countX, uint countY) {
    SpTaps taps;
    taps.count = 0u;
    for (uint t = 0u; t < 4u; ++t) {
        taps.index[t] = 0u;
        taps.weight[t] = 0.0f;
    }
    uint bx;
    uint by;
    float fx;
    float fy;
    spAxisTaps(x, step, countX, bx, fx);
    spAxisTaps(y, step, countY, by, fy);
    for (uint j = 0u; j < 2u; ++j) {
        for (uint i = 0u; i < 2u; ++i) {
            const float weight = (i == 0u ? 1.0f - fx : fx) * (j == 0u ? 1.0f - fy : fy);
            if (weight <= 0.0f) {
                continue;
            }
            taps.index[taps.count] = ((by + j) * countX) + (bx + i);
            taps.weight[taps.count] = weight;
            taps.count += 1u;
        }
    }
    return taps;
}

/// `hemisphere_direction`.
float3 spHemisphereDirection(float3 normal, float z, float turn) {
    float3 tangent;
    float3 bitangent;
    grBasis(normal, tangent, bitangent);
    const float radius = sqrt(max(0.0f, 1.0f - (z * z)));
    const float angle = 6.28318530717958647692f * turn;
    return giNormalisedOr((tangent * (radius * cos(angle))) + (bitangent * (radius * sin(angle))) +
                              (normal * z),
                          normal);
}

uint spGuideCell(float z, float turn) {
    const uint i = min(uint(z * float(kSpGuideEdge)), kSpGuideEdge - 1u);
    const uint j = min(uint(turn * float(kSpGuideEdge)), kSpGuideEdge - 1u);
    return (i * kSpGuideEdge) + j;
}

// --- A record --------------------------------------------------------------------------------------

struct SpProbe {
    float3 position;
    float depth;
    float3 normal;
    uint id;
};

SpProbe spLoadProbe(uint slot, uint record) {
    const uint base = spRecordBase(slot, record);
    const float4 a = giResolve.probes[base];
    const float4 b = giResolve.probes[base + 1u];
    SpProbe probe;
    probe.position = a.xyz;
    probe.depth = a.w;
    probe.normal = b.xyz;
    probe.id = asuint(b.w);
    return probe;
}

/// Nine RGB coefficients and a trailing word from the seven float4 at `base`.
void spLoadPacked(uint base, out float3 coefficients[9], out float last) {
    float values[28];
    for (uint v = 0u; v < 7u; ++v) {
        const float4 word = giResolve.probes[base + v];
        values[(v * 4u) + 0u] = word.x;
        values[(v * 4u) + 1u] = word.y;
        values[(v * 4u) + 2u] = word.z;
        values[(v * 4u) + 3u] = word.w;
    }
    for (uint k = 0u; k < kSpCoefficients; ++k) {
        coefficients[k] = float3(values[k * 3u], values[(k * 3u) + 1u], values[(k * 3u) + 2u]);
    }
    last = values[27];
}

void spStorePacked(uint base, float3 coefficients[9], float last) {
    float values[28];
    for (uint k = 0u; k < kSpCoefficients; ++k) {
        values[k * 3u] = coefficients[k].x;
        values[(k * 3u) + 1u] = coefficients[k].y;
        values[(k * 3u) + 2u] = coefficients[k].z;
    }
    values[27] = last;
    for (uint v = 0u; v < 7u; ++v) {
        giResolve.probes[base + v] = float4(values[(v * 4u) + 0u], values[(v * 4u) + 1u],
                                            values[(v * 4u) + 2u], values[(v * 4u) + 3u]);
    }
}

void spLoadCoefficients(uint slot, uint record, out float3 coefficients[9], out float history) {
    spLoadPacked(spRecordBase(slot, record) + 2u, coefficients, history);
}

void spStoreCoefficients(uint slot, uint record, float3 coefficients[9], float history) {
    spStorePacked(spRecordBase(slot, record) + 2u, coefficients, history);
}

/// `ScreenProbe::filtered`: what the pixels integrate.
void spLoadFiltered(uint slot, uint record, out float3 coefficients[9]) {
    float unused;
    spLoadPacked(spRecordBase(slot, record) + kSpFilteredVec4, coefficients, unused);
}

void spStoreFiltered(uint slot, uint record, float3 coefficients[9]) {
    spStorePacked(spRecordBase(slot, record) + kSpFilteredVec4, coefficients, 0.0f);
}

/// A probe as `place` writes it: everything the trace fills is its default.
void spWriteProbe(uint slot, uint record, float3 position, float depth, float3 normal, uint id) {
    const uint base = spRecordBase(slot, record);
    giResolve.probes[base] = float4(position, depth);
    giResolve.probes[base + 1u] = float4(normal, asfloat(id));
    for (uint v = 0u; v < 7u; ++v) {
        giResolve.probes[base + 2u + v] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    giResolve.probes[base + 9u] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    giResolve.probes[base + 10u] = float4(1.0f, 0.0f, 0.0f, 0.0f);
    for (uint v = 0u; v < 7u; ++v) {
        giResolve.probes[base + kSpFilteredVec4 + v] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}

// --- `ScreenProbeGather::place` ---------------------------------------------------------------------

/// One thread a uniform tile and a half-tile cell: the tile's probe, and the cell's adaptive probe
/// where the uniform ones about its centre — read from the surfaces they stand on, which is what
/// `place` wrote them from — cannot be interpolated.
void spPlace(uint thread) {
    const uint slot = spSlot();
    const uint uniform = spUniformCount();
    const uint pixels = giResolve.constants[kGrCounts].z;
    if (thread < uniform) {
        const uint pixel = spUniformPixel(thread);
        const uint record = ((slot * pixels) + pixel) * 2u;
        const float4 at = giResolve.surfaces[record];
        if (at.w > 0.0f) {
            const float3 normal =
                giNormalisedOr(giResolve.surfaces[record + 1u].xyz, float3(0.0f, 1.0f, 0.0f));
            spWriteProbe(slot, thread, at.xyz, at.w, normal, thread);
        } else {
            spWriteProbe(slot, thread, float3(0.0f, 0.0f, 0.0f), 0.0f, float3(0.0f, 1.0f, 0.0f),
                         thread);
        }
    }
    const uint cells = spCellsX() * spCellsY();
    if (thread >= cells) {
        return;
    }
    giResolve.slots[spTableIndex(slot, thread)] = kSpNone;
    const uint pixel = spAdaptivePixel(thread);
    const uint record = ((slot * pixels) + pixel) * 2u;
    const float4 at = giResolve.surfaces[record];
    if (at.w <= 0.0f) {
        return;
    }
    const float3 normal =
        giNormalisedOr(giResolve.surfaces[record + 1u].xyz, float3(0.0f, 1.0f, 0.0f));
    const float4 weights = asfloat(giResolve.constants[kGrProbeWeights]);
    const SpTaps taps = spBilinearTaps(pixel % spWidth(), pixel / spWidth(), spSpacing(),
                                       spTilesX(), spTilesY());
    float covered = 0.0f;
    for (uint tap = 0u; tap < taps.count; ++tap) {
        const uint probePixel = spUniformPixel(taps.index[tap]);
        const uint probeRecord = ((slot * pixels) + probePixel) * 2u;
        const float4 probeAt = giResolve.surfaces[probeRecord];
        if (probeAt.w <= 0.0f) {
            continue;
        }
        const float3 probeNormal =
            giNormalisedOr(giResolve.surfaces[probeRecord + 1u].xyz, float3(0.0f, 1.0f, 0.0f));
        covered += taps.weight[tap] *
                   spGeometryWeight(probeAt.xyz, probeNormal, at.xyz, normal, at.w, weights.y);
    }
    if (covered >= weights.x) {
        return;
    }
    uint taken = 0u;
    InterlockedAdd(giResolve.slots[spCounter(slot)], 1u, taken);
    if (taken >= spCapacity()) {
        return;
    }
    giResolve.slots[spTableIndex(slot, thread)] = taken;
    spWriteProbe(slot, uniform + taken, at.xyz, at.w, normal, uniform + thread);
}

// --- `ScreenProbeGather::trace_probe` ----------------------------------------------------------------

/// `screen_landing`: the previous frame's cell `position` lands on, the camera's motion alone.
bool spLanding(float3 position, out uint x, out uint y) {
    x = 0u;
    y = 0u;
    float4 c0 = asfloat(giResolve.constants[kGrWorldToClip]);
    float4 c1 = asfloat(giResolve.constants[kGrWorldToClip + 1u]);
    float4 c2 = asfloat(giResolve.constants[kGrWorldToClip + 2u]);
    float4 c3 = asfloat(giResolve.constants[kGrWorldToClip + 3u]);
    float4 clip = c0 * position.x + c1 * position.y + c2 * position.z + c3 * 1.0f;
    if (clip.w <= 1e-8f) {
        return false;
    }
    float inverse = 1.0f / clip.w;
    const float2 current =
        float2(((clip.x * inverse) * 0.5f) + 0.5f, ((clip.y * inverse) * 0.5f) + 0.5f);
    c0 = asfloat(giResolve.constants[kGrPreviousToClip]);
    c1 = asfloat(giResolve.constants[kGrPreviousToClip + 1u]);
    c2 = asfloat(giResolve.constants[kGrPreviousToClip + 2u]);
    c3 = asfloat(giResolve.constants[kGrPreviousToClip + 3u]);
    clip = c0 * position.x + c1 * position.y + c2 * position.z + c3 * 1.0f;
    if (clip.w <= 1e-8f) {
        return false;
    }
    inverse = 1.0f / clip.w;
    const float2 previous =
        float2(((clip.x * inverse) * 0.5f) + 0.5f, ((clip.y * inverse) * 0.5f) + 0.5f);
    const float2 motion = float2(previous.x - current.x, previous.y - current.y);
    const float2 uv = float2(current.x + motion.x, current.y + motion.y);
    const float width = float(spWidth());
    const float height = float(spHeight());
    const float fx = floor(uv.x * width);
    const float fy = floor((1.0f - uv.y) * height);
    if (fx < 0.0f || fy < 0.0f || fx >= width || fy >= height) {
        return false;
    }
    x = uint(fx);
    y = uint(fy);
    return true;
}

/// `invalidated_at`, over the resolve's words.
bool spInvalidated(float3 point) {
    const uint count = min(giResolve.constants[kGrInvalid].x, 4u);
    for (uint region = 0u; region < count; ++region) {
        const float3 lo = asfloat(giResolve.constants[kGrInvalid + 1u + (region * 2u)].xyz);
        const float3 hi = asfloat(giResolve.constants[kGrInvalid + 2u + (region * 2u)].xyz);
        if (point.x >= lo.x && point.y >= lo.y && point.z >= lo.z && point.x <= hi.x &&
            point.y <= hi.y && point.z <= hi.z) {
            return true;
        }
    }
    return false;
}

/// `ScreenProbeGather::reproject`: last frame's coefficients about where the probe lands.
bool spReproject(SpProbe probe, float tolerance, out float3 coefficients[9], out float history,
                 out uint matched) {
    for (uint k = 0u; k < kSpCoefficients; ++k) {
        coefficients[k] = float3(0.0f, 0.0f, 0.0f);
    }
    history = 0.0f;
    matched = kSpNone;
    if (spInvalidated(probe.position)) {
        return false;
    }
    uint x;
    uint y;
    if (!spLanding(probe.position, x, y)) {
        return false;
    }
    // The one previous probe that stands for this one best: see `reproject`.
    const uint previous = 1u - spSlot();
    uint best = kSpNone;
    float bestWeight = 0.0f;
    for (uint pass = 0u; pass < 2u; ++pass) {
        const SpTaps taps = pass == 0u
                                ? spBilinearTaps(x, y, spSpacing(), spTilesX(), spTilesY())
                                : spBilinearTaps(x, y, spCell(), spCellsX(), spCellsY());
        for (uint tap = 0u; tap < taps.count; ++tap) {
            uint record = taps.index[tap];
            if (pass == 1u) {
                const uint slot = giResolve.slots[spTableIndex(previous, record)];
                if (slot == kSpNone) {
                    continue;
                }
                record = spUniformCount() + slot;
            }
            const SpProbe candidate = spLoadProbe(previous, record);
            if (candidate.depth <= 0.0f) {
                continue;
            }
            const float weight =
                taps.weight[tap] * spGeometryWeight(candidate.position, candidate.normal,
                                                    probe.position, probe.normal, probe.depth,
                                                    tolerance);
            if (weight > bestWeight) {
                bestWeight = weight;
                best = record;
            }
        }
    }
    if (best == kSpNone || bestWeight < kSpHistoryMatch) {
        return false;
    }
    spLoadCoefficients(previous, best, coefficients, history);
    matched = best;
    return true;
}

/// `screen_probe_traces`.
bool spProbeTraces(uint id, uint frame, float density) {
    if (density >= 1.0f) {
        return true;
    }
    return grUnit(grHash(id, frame, kSpUpdateStream)) < density;
}

/// `probe_origin` in screen_probes.cpp: the surface lifted along its normal as a final-gather ray
/// is, then moved out along the field's gradient to the same clearance from any other surface — so
/// a probe beside a thin wall does not step inside it, nor one on a room's edge leave through it.
float3 spProbeOrigin<T : IGiWorldTier>(SpProbe probe, float lift) {
    if (giLevelCount() == 0u) {
        return probe.position + (probe.normal * lift);
    }
    const float voxel = giFinestVoxel();
    const float clearance = T.source() == kGpSourceHardware ? lift : max(lift, voxel);
    float3 origin = probe.position + (probe.normal * clearance);
    if (voxel <= 0.0f) {
        return origin;
    }
    for (uint step = 0u; step < kSpOriginSteps; ++step) {
        const float distance = giDistance(origin);
        if (distance >= clearance) {
            break;
        }
        const float3 gradient = float3(
            giDistance(origin + float3(voxel, 0.0f, 0.0f)) - giDistance(origin - float3(voxel, 0.0f, 0.0f)),
            giDistance(origin + float3(0.0f, voxel, 0.0f)) - giDistance(origin - float3(0.0f, voxel, 0.0f)),
            giDistance(origin + float3(0.0f, 0.0f, voxel)) - giDistance(origin - float3(0.0f, 0.0f, voxel)));
        origin = origin + (giNormalisedOr(gradient, probe.normal) * (clearance - distance));
    }
    return origin;
}

/// `probe_ray`: one ray from `origin` through tier `T`, and none from a back face.
GrSample spProbeRay<T : IGiWorldTier>(float3 origin, float3 unit, float reach) {
    const uint cacheFrame = giProbes.constants[kGpBudget].w;
    const GiWorldHit hit = T.trace(origin, unit, reach);
    if (hit.hit && dot(hit.normal, unit) > 0.0f) {
        return grSample(float3(0.0f, 0.0f, 0.0f), 0.0f, kGrSourceNone);
    }
    const GiTierSample tier = gpWorldRadiance(hit, unit, T.source(), cacheFrame);
    return grSample(tier.radiance, tier.confidence, tier.source);
}

groupshared float3 spOrigin;
groupshared float3 spHistory[9];
groupshared float spHistoryCount;
groupshared uint spReprojected;
/// The previous record a probe keeps instead of tracing, below a ray density of one, or none.
groupshared uint spCarried;
groupshared float4 spStandIn;
groupshared float spWeights[16];
groupshared float3 spRayValue[64];
groupshared float3 spRayDirection[64];
groupshared float spRayWeight[64];
groupshared uint spRayValid[64];

/// One probe record by one workgroup: lane 0 finds the stand-in and the history, sixteen lanes the
/// guide, a lane a ray, and lane 0 sums the rays in order and blends.
void spTrace<T : IGiWorldTier>(uint record, uint lane) {
    const uint slot = spSlot();
    // The NEXT frame's adaptive counter: this frame's place is done with the other slot's.
    if (record == 0u && lane == 0u) {
        giResolve.slots[spCounter(1u - slot)] = 0u;
    }
    const uint uniform = spUniformCount();
    if (record >= uniform &&
        record - uniform >= min(giResolve.slots[spCounter(slot)], spCapacity())) {
        return;  // uniform over the workgroup: the record is the group's
    }
    const SpProbe probe = spLoadProbe(slot, record);
    if (probe.depth <= 0.0f) {
        return;
    }
    const uint4 shape = giResolve.constants[kGrProbeShape];
    const float4 weights = asfloat(giResolve.constants[kGrProbeWeights]);
    const uint frame = giResolve.constants[kGrFrame].x;
    const uint cacheFrame = giResolve.constants[kGrReflect].w;

    if (lane == 0u) {
        float3 previous[9];
        float history = 0.0f;
        uint matched = kSpNone;
        const bool allowed = (shape.w & 2u) != 0u && giResolve.constants[kGrIntensity].w != 0u;
        const bool found = allowed && spReproject(probe, weights.y, previous, history, matched);
        for (uint k = 0u; k < kSpCoefficients; ++k) {
            spHistory[k] = previous[k];
        }
        spHistoryCount = history;
        spReprojected = found ? 1u : 0u;
        const float density = asfloat(giResolve.constants[kGrCamera].w);
        spCarried = found && !spProbeTraces(probe.id, frame, density) ? matched : kSpNone;
        if (spCarried == kSpNone) {
            float4 diffuse;
            float4 specular;
            grSampleTraced(probe.position, probe.normal, probe.normal, cacheFrame, diffuse,
                           specular);
            spStandIn = diffuse;
            spOrigin = spProbeOrigin<T>(probe, asfloat(giResolve.constants[kGrRays].y));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (spCarried != kSpNone) {
        // `trace_probe`'s probe that does not trace this frame: the one it stands for, kept —
        // its radiance and count, its cache answer and its openness. Uniform over the group.
        if (lane == 0u) {
            float3 kept[9];
            for (uint k = 0u; k < kSpCoefficients; ++k) {
                kept[k] = spHistory[k];
            }
            spStoreCoefficients(slot, record, kept, spHistoryCount);
            spStoreFiltered(slot, record, kept);
            const uint from = spRecordBase(1u - slot, spCarried);
            const uint base = spRecordBase(slot, record);
            giResolve.probes[base + 9u] = giResolve.probes[from + 9u];
            giResolve.probes[base + 10u] = giResolve.probes[from + 10u];
        }
        return;
    }

    if (lane < kSpGuideCells) {
        const float z = (float(lane / kSpGuideEdge) + 0.5f) / float(kSpGuideEdge);
        const float turn = (float(lane % kSpGuideEdge) + 0.5f) / float(kSpGuideEdge);
        const float3 direction = spHemisphereDirection(probe.normal, z, turn);
        float3 radiance = float3(0.0f, 0.0f, 0.0f);
        if (spReprojected != 0u) {
            float3 previous[9];
            for (uint k = 0u; k < kSpCoefficients; ++k) {
                previous[k] = spHistory[k];
            }
            radiance = spEvaluate(previous, direction, float3(1.0f, 1.0f, 1.0f));
        } else {
            radiance = gpSample(probe.position, direction, cacheFrame).xyz;
        }
        spWeights[lane] = max(spLuminance(radiance), 0.0f) * z;
    }
    GroupMemoryBarrierWithGroupSync();

    float guideTotal = 0.0f;
    for (uint cell = 0u; cell < kSpGuideCells; ++cell) {
        guideTotal += spWeights[cell];
    }
    const float guided = guideTotal > 0.0f ? gpClamp(weights.z, 0.0f, kSpMaxGuided) : 0.0f;
    const float uniformShare = (1.0f - guided) * 0.5f;
    const float cosineShare = (1.0f - guided) * 0.5f;
    const uint rays = min(max(shape.x, 1u), kSpMaxRays);

    if (lane < rays) {
        const uint hash = grHash((probe.id * kSpMaxRays) + lane, frame, kSpStream);
        const float u0 = grUnit(hash);
        const float u1 = grUnit(hash * 0x27220A95u);
        const float u2 = grUnit(hash * 0x9E3779B9u);
        float z = u1;
        float turn = u2;
        if (u0 >= uniformShare && u0 < uniformShare + cosineShare) {
            z = sqrt(u1);
        } else if (u0 >= uniformShare + cosineShare) {
            const float target = ((u0 - uniformShare - cosineShare) / guided) * guideTotal;
            uint chosen = kSpGuideCells - 1u;
            float running = 0.0f;
            for (uint cell = 0u; cell < kSpGuideCells; ++cell) {
                running += spWeights[cell];
                if (spWeights[cell] > 0.0f && target < running) {
                    chosen = cell;
                    break;
                }
            }
            z = (float(chosen / kSpGuideEdge) + u1) / float(kSpGuideEdge);
            turn = (float(chosen % kSpGuideEdge) + u2) / float(kSpGuideEdge);
        }
        float density = (uniformShare / 6.28318530717958647692f) +
                        (cosineShare * z / 3.14159265358979323846f);
        if (guided > 0.0f) {
            density += guided * (spWeights[spGuideCell(z, turn)] / guideTotal) * kSpGuideDensity;
        }
        uint valid = 0u;
        float3 value = float3(0.0f, 0.0f, 0.0f);
        const float3 direction = spHemisphereDirection(probe.normal, z, turn);
        if (density > 0.0f) {
            const float4 rayWords = asfloat(giResolve.constants[kGrRays]);
            const uint excluded = giResolve.constants[kGrRays].w;
            const float3 unit = giNormalisedOr(direction, float3(0.0f, 0.0f, -1.0f));
            const float4 standIn = spStandIn;
            const GrSample sample =
                grGatherSample(spProbeRay<T>(spOrigin, unit, rayWords.x),
                               grSample(standIn.xyz, standIn.w, kGrSourceCache), excluded);
            if (sample.source != kGrSourceNone) {
                valid = 1u;
                value = sample.radiance;
            }
        }
        spRayValue[lane] = value;
        spRayDirection[lane] = direction;
        spRayWeight[lane] = density > 0.0f ? 1.0f / (density * float(rays)) : 0.0f;
        spRayValid[lane] = valid;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0u) {
        return;
    }

    float3 sum[9];
    for (uint k = 0u; k < kSpCoefficients; ++k) {
        sum[k] = float3(0.0f, 0.0f, 0.0f);
    }
    for (uint ray = 0u; ray < rays; ++ray) {
        if (spRayValid[ray] == 0u) {
            continue;
        }
        float basis[9];
        spBasis(spRayDirection[ray], basis);
        const float weight = spRayWeight[ray];
        for (uint k = 0u; k < kSpCoefficients; ++k) {
            sum[k] = sum[k] + (spRayValue[ray] * (basis[k] * weight));
        }
    }

    const float frames = float(max(shape.y, 1u));
    float3 blended[9];
    float history = 1.0f;
    if (spReprojected != 0u) {
        const float alpha = max(1.0f / (spHistoryCount + 1.0f), 1.0f / frames);
        for (uint k = 0u; k < kSpCoefficients; ++k) {
            blended[k] = spHistory[k] + ((sum[k] - spHistory[k]) * alpha);
        }
        history = min(spHistoryCount + 1.0f, frames);
    } else {
        for (uint k = 0u; k < kSpCoefficients; ++k) {
            blended[k] = sum[k];
        }
    }
    spStoreCoefficients(slot, record, blended, history);
    spStoreFiltered(slot, record, blended);  // `unfiltered`: the filter, if it runs, replaces it

    float openness = 1.0f;
    const float3 camera = asfloat(giResolve.constants[kGrCamera].xyz);
    const float far = grFarFieldWeight(length(probe.position - camera));
    const uint skyRays = giResolve.constants[kGrRays].z;
    if (far > 0.0f && skyRays > 0u) {
        openness = grSkyVisibilityRay(probe.position, probe.normal, (frame + probe.id) % skyRays,
                                      skyRays);
    }
    const uint base = spRecordBase(slot, record);
    giResolve.probes[base + 9u] = spStandIn;
    giResolve.probes[base + 10u] = float4(openness, 0.0f, 0.0f, 0.0f);
}

// --- `ScreenProbeGather::filter` ---------------------------------------------------------------------

/// `filter_tent`.
float spFilterTent(int offset, uint reach) {
    const float distance = float(offset < 0 ? -offset : offset);
    return 1.0f - (distance / float(reach + 1u));
}

/// `filter_probe` for one record: its own coefficients and the uniform neighbours' within the
/// reach, by the tent and the geometry weight, into its filtered coefficients. Every probe reads
/// only coefficients and writes only its own filtered ones, so the probes run at once.
void spFilter(uint record) {
    const uint slot = spSlot();
    const uint uniform = spUniformCount();
    const uint reach = giResolve.constants[kGrProbeSamples].y;
    if (reach == 0u || record >= spRecords()) {
        return;
    }
    if (record >= uniform && record - uniform >= min(giResolve.slots[spCounter(slot)], spCapacity())) {
        return;
    }
    const SpProbe probe = spLoadProbe(slot, record);
    if (probe.depth <= 0.0f) {
        return;
    }
    float3 sum[9];
    float history;
    spLoadCoefficients(slot, record, sum, history);
    float total = 1.0f;
    // `probe_tile`.
    const uint pixel =
        probe.id < uniform ? spUniformPixel(probe.id) : spAdaptivePixel(probe.id - uniform);
    const int tileX = int(min((pixel % spWidth()) / spSpacing(), spTilesX() - 1u));
    const int tileY = int(min((pixel / spWidth()) / spSpacing(), spTilesY() - 1u));
    const float tolerance = asfloat(giResolve.constants[kGrProbeWeights].y);
    const float worldReach = asfloat(giResolve.constants[kGrProbeSamples].z);
    const int span = int(reach);
    for (int dy = -span; dy <= span; ++dy) {
        const int y = tileY + dy;
        for (int dx = -span; dx <= span; ++dx) {
            const int x = tileX + dx;
            if (x < 0 || y < 0 || x >= int(spTilesX()) || y >= int(spTilesY())) {
                continue;
            }
            const uint tile = (uint(y) * spTilesX()) + uint(x);
            const SpProbe neighbour = spLoadProbe(slot, tile);
            if (tile == probe.id || neighbour.depth <= 0.0f) {
                continue;
            }
            const float near =
                1.0f - (length(neighbour.position - probe.position) / (worldReach * probe.depth));
            const float weight = spFilterTent(dx, reach) * spFilterTent(dy, reach) *
                                 max(near, 0.0f) *
                                 spGeometryWeight(neighbour.position, neighbour.normal,
                                                  probe.position, probe.normal, probe.depth,
                                                  tolerance);
            if (weight <= 0.0f) {
                continue;
            }
            float3 coefficients[9];
            float count;
            spLoadCoefficients(slot, tile, coefficients, count);
            for (uint k = 0u; k < kSpCoefficients; ++k) {
                sum[k] = sum[k] + (coefficients[k] * weight);
            }
            total += weight;
        }
    }
    for (uint k = 0u; k < kSpCoefficients; ++k) {
        sum[k] = sum[k] / total;
    }
    spStoreFiltered(slot, record, sum);
}

// --- `ScreenProbeGather::integrate` ------------------------------------------------------------------

/// One grid cell's two signals from the probes about it.
void spIntegrate<T : IGiWorldTier>(uint pixel) {
    const uint4 counts = giResolve.constants[kGrCounts];
    if (pixel >= counts.z) {
        return;
    }
    const uint record = ((counts.w * counts.z) + pixel) * 2u;
    const float4 at = giResolve.surfaces[record];
    const float4 facing = giResolve.surfaces[record + 1u];
    if (at.w <= 0.0f) {
        giResolve.noisy[pixel * 2u] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        giResolve.noisy[(pixel * 2u) + 1u] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const uint slot = counts.w;
    const uint frame = giResolve.constants[kGrFrame].x;
    const float3 position = at.xyz;
    const float3 normal = giNormalisedOr(facing.xyz, float3(0.0f, 1.0f, 0.0f));
    const float roughness = facing.w;
    const float3 offset = position - asfloat(giResolve.constants[kGrCamera].xyz);
    const float3 incident = giNormalisedOr(offset, float3(0.0f, 0.0f, -1.0f));
    const float3 reflection =
        giNormalisedOr(incident - (normal * (2.0f * dot(incident, normal))), normal);
    const float rough = gpClamp(roughness, 0.0f, 1.0f);
    const float3 specularBand = float3(1.0f, 1.0f + ((kSpIrradianceBand1 - 1.0f) * rough),
                                       1.0f + ((kSpIrradianceBand2 - 1.0f) * rough));
    const float3 irradianceBand = float3(1.0f, kSpIrradianceBand1, kSpIrradianceBand2);
    const float4 weights = asfloat(giResolve.constants[kGrProbeWeights]);
    const uint x = pixel % spWidth();
    const uint y = pixel / spWidth();

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    float3 glossy = float3(0.0f, 0.0f, 0.0f);
    float3 cache = float3(0.0f, 0.0f, 0.0f);
    float cacheConfidence = 0.0f;
    float openness = 0.0f;
    float total = 0.0f;
    uint nearest = kSpNone;
    float nearestPlane = 1.0e30f;
    for (uint pass = 0u; pass < 2u; ++pass) {
        const SpTaps taps = pass == 0u
                                ? spBilinearTaps(x, y, spSpacing(), spTilesX(), spTilesY())
                                : spBilinearTaps(x, y, spCell(), spCellsX(), spCellsY());
        for (uint tap = 0u; tap < taps.count; ++tap) {
            uint index = taps.index[tap];
            if (pass == 1u) {
                const uint taken = giResolve.slots[spTableIndex(slot, index)];
                if (taken == kSpNone) {
                    continue;
                }
                index = spUniformCount() + taken;
            }
            const SpProbe probe = spLoadProbe(slot, index);
            if (probe.depth <= 0.0f) {
                continue;
            }
            const float plane = abs(dot(probe.normal, position - probe.position));
            if (plane < nearestPlane) {
                nearestPlane = plane;
                nearest = index;
            }
            const float weight =
                taps.weight[tap] * spGeometryWeight(probe.position, probe.normal, position,
                                                    normal, at.w, weights.y);
            if (weight <= 0.0f) {
                continue;
            }
            float3 coefficients[9];
            spLoadFiltered(slot, index, coefficients);
            const uint base = spRecordBase(slot, index);
            const float4 cached = giResolve.probes[base + 9u];
            irradiance = irradiance + (spEvaluate(coefficients, normal, irradianceBand) * weight);
            glossy = glossy + (spEvaluate(coefficients, reflection, specularBand) * weight);
            cache = cache + (cached.xyz * weight);
            cacheConfidence += cached.w * weight;
            openness += giResolve.probes[base + 10u].x * weight;
            total += weight;
        }
    }
    if (total < kSpWeightFloor) {
        if (nearest == kSpNone) {
            const float none = asfloat(giResolve.constants[kGrProbeSamples].x);
            giResolve.noisy[pixel * 2u] = float4(0.0f, 0.0f, 0.0f, none);
            giResolve.noisy[(pixel * 2u) + 1u] = float4(0.0f, 0.0f, 0.0f, none);
            return;
        }
        float3 coefficients[9];
        spLoadFiltered(slot, nearest, coefficients);
        const uint base = spRecordBase(slot, nearest);
        irradiance = spEvaluate(coefficients, normal, irradianceBand);
        glossy = spEvaluate(coefficients, reflection, specularBand);
        cache = giResolve.probes[base + 9u].xyz;
        cacheConfidence = giResolve.probes[base + 9u].w;
        openness = giResolve.probes[base + 10u].x;
    } else {
        irradiance = irradiance / total;
        glossy = glossy / total;
        cache = cache / total;
        cacheConfidence /= total;
        openness /= total;
    }

    const uint4 shape = giResolve.constants[kGrProbeShape];
    const uint excluded = giResolve.constants[kGrRays].w;
    GrCombine state = grCombineBegin();
    if ((shape.w & 1u) != 0u) {
        grCombineAdd(state, grSample(cache, cacheConfidence, kGrSourceCache), excluded);
    }
    grCombineAdd(state, grSample(irradiance, 1.0f, T.source()), excluded);
    GrSample sky = grSample(float3(0.0f, 0.0f, 0.0f), 0.0f, kGrSourceSky);
    const float far = grFarFieldWeight(length(offset));
    if (far > 0.0f) {
        sky.radiance = gpSky(normal) * openness;
        sky.confidence = gpClamp(far * openness, 0.0f, 1.0f);
    }
    grCombineAdd(state, sky, excluded);
    const float3 diffuse = grCombineEnd(state) * asfloat(giResolve.constants[kGrIntensity].x);

    const GrSample probeSpecular = grSample(glossy, gpClamp(roughness, 0.15f, 1.0f), T.source());
    GrCombine reflected = grCombineBegin();
    const uint count = grReflectionRays(roughness);
    const float density = asfloat(giResolve.constants[kGrCamera].w);
    if (count > 0u && grTraces(pixel, frame, density)) {
        const float4 rays = asfloat(giResolve.constants[kGrRays]);
        const uint hash = grHash(pixel, frame, kGrStreamSpecular);
        const float3 direction =
            grLobeDirection(reflection, normal, roughness, hash % count, count, frame);
        const float3 unit = giNormalisedOr(direction, float3(0.0f, 0.0f, -1.0f));
        grCombineAdd(reflected,
                     grGatherSample(grWorldRay<T>(position, normal, unit, rays.y, rays.x),
                                    probeSpecular, 0u),
                     0u);
    }
    grCombineAdd(reflected, probeSpecular, 0u);
    const float3 specular = grCombineEnd(reflected) * asfloat(giResolve.constants[kGrIntensity].y);
    const float samples = asfloat(giResolve.constants[kGrProbeSamples].x);
    giResolve.noisy[pixel * 2u] = float4(diffuse, samples);
    giResolve.noisy[(pixel * 2u) + 1u] = float4(specular, samples);
}
