// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 4: the per-pixel resolve. Issue #35. Include-only; the entry points
// are gi_resolve.slang (the software world tier) and gi_resolve_hardware.slang (ray queries).
//
// ================================================================================================
// EVERY FUNCTION BELOW IS A TRANSCRIPTION, AND NAMES WHAT IT TRANSCRIBES
// ================================================================================================
//
// `cy::rendering-gi`'s screen_resolve.h is the oracle: `resolve_screen_pixel` with its three
// sources — `RadianceCache::sample_traced` (`grSampleTraced` below), one final-gather ray
// through the world tier (`grWorldRay`), and the far-field sky through one of
// `DistanceField::sky_visibility`'s rays — combined by `resolve.h`'s `combine()` under
// `exclusion_for()`'s mask; and the rough specular, `indirect_specular`'s rule with one lobe ray.
// `render.gi_gpu_resolve` runs both over one view and compares, pixel for pixel.
//
// The resolve's own buffers are set 2, after the scene (set 0) and the probe set (set 1) the
// world tiers and the cache query read; the hardware tier's top level is then set 3.

#include "gi_probe_common.slang"

// --- The resolve's constant words: `GpuScreenResolve` in gpu_resolve.cpp ------------------------

static const uint kGrCounts = 0u;         // width, height, pixels, current surface slot
static const uint kGrCamera = 1u;         // xyz camera (world), w ray density
static const uint kGrRays = 2u;           // max ray distance, origin lift, sky rays, exclusion mask
static const uint kGrFar = 3u;            // far begins, near-field metres, far-field metres, sky reach
static const uint kGrReflect = 4u;        // mirror max roughness, cache min roughness, max rays, cache frame
static const uint kGrIntensity = 5u;      // diffuse, specular, depth tolerance, history valid
static const uint kGrWorldToClip = 6u;    // four columns
static const uint kGrPreviousToClip = 10u;  // four columns
static const uint kGrUnpack = 14u;        // P00, P11, m22, m32 of the frame's projection
static const uint kGrViewToRelative = 15u;  // three rows: view space to camera-relative
static const uint kGrFrame = 18u;         // resolve frame, filter passes, final buffer (0 A, 1 B), 0
static const uint kGrJitter = 19u;        // the prepass's jitter undone, in pixels: gi_surfaces.slang
static const uint kGrSignals = 20u;       // three uint4 per signal: diffuse 20..22, specular 23..25
static const uint kGrInvalid = 26u;       // x region count; then two uint4 a region: min, max

static const uint kGrSourceNone = 0u;
static const uint kGrSourceCache = 3u;
static const uint kGrSourceSky = 9u;

/// `ScreenResolve`'s hash streams.
static const uint kGrStreamTrace = 5u;
static const uint kGrStreamDirection = 6u;
static const uint kGrStreamSpecular = 7u;

struct GiResolveSet {
    /// The constant words above.
    StructuredBuffer<uint4> constants;
    /// Two surface slots, this frame's and last frame's, each two float4 a pixel:
    /// [world position, view depth (zero or less is sky)], [unit normal, roughness].
    StructuredBuffer<float4> surfaces;
    /// Two float4 a pixel: [diffuse radiance, samples], [specular radiance, samples].
    RWStructuredBuffer<float4> noisy;
};

[[vk::binding(0, 2)]] ParameterBlock<GiResolveSet> giResolve;

// --- The randomness: `screen_hash`, `screen_unit`, `screen_cosine_direction` --------------------

uint grHash(uint pixel, uint frame, uint stream) {
    uint hash = pixel * 0x9E3779B9u;
    hash ^= frame * 0x85EBCA6Bu;
    hash ^= stream * 0xC2B2AE35u;
    hash ^= hash >> 15u;
    hash *= 0x2545F491u;
    hash ^= hash >> 13u;
    return hash;
}

float grUnit(uint hash) {
    return float(hash & 0x00FFFFFFu) / 16777216.0f;
}

/// `basis_around` in screen_resolve.cpp.
void grBasis(float3 normal, out float3 tangent, out float3 bitangent) {
    const float sign = normal.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + normal.z);
    const float b = normal.x * normal.y * a;
    tangent = float3(1.0f + (sign * normal.x * normal.x * a), sign * b, -sign * normal.x);
    bitangent = float3(b, sign + (normal.y * normal.y * a), -normal.y);
}

float3 grCosineDirection(float3 normal, float u1, float u2) {
    float3 tangent;
    float3 bitangent;
    grBasis(normal, tangent, bitangent);
    const float radius = sqrt(u1);
    const float angle = 6.28318530717958647692f * u2;
    const float z = sqrt(max(0.0f, 1.0f - u1));
    return giNormalisedOr((tangent * (radius * cos(angle))) + (bitangent * (radius * sin(angle))) +
                              (normal * z),
                          normal);
}

/// `screen_traces`.
bool grTraces(uint pixel, uint frame, float density) {
    if (density >= 1.0f) {
        return true;
    }
    return grUnit(grHash(pixel, frame, kGrStreamTrace)) < density;
}

// --- The resolve's arithmetic: `combine`, `far_field_weight`, the reflection rules ---------------

struct GrSample {
    float3 radiance;
    float confidence;
    uint source;
};

GrSample grSample(float3 radiance, float confidence, uint source) {
    GrSample sample;
    sample.radiance = radiance;
    sample.confidence = confidence;
    sample.source = source;
    return sample;
}

/// `combine()`'s accumulation, one sample at a time and in the host's order.
struct GrCombine {
    float3 total;
    float totalWeight;
    float confidence;
};

GrCombine grCombineBegin() {
    GrCombine state;
    state.total = float3(0.0f, 0.0f, 0.0f);
    state.totalWeight = 0.0f;
    state.confidence = 0.0f;
    return state;
}

void grCombineAdd(inout GrCombine state, GrSample sample, uint excluded) {
    if (sample.source == kGrSourceNone) {
        return;
    }
    if ((excluded & (1u << sample.source)) != 0u) {
        return;
    }
    const float weight = gpClamp(sample.confidence, 0.0f, 1.0f);
    if (weight <= 0.0f) {
        return;
    }
    state.total = state.total + (sample.radiance * weight);
    state.totalWeight += weight;
    state.confidence = max(state.confidence, weight);
}

float3 grCombineEnd(GrCombine state) {
    return state.totalWeight > 0.0f ? state.total / state.totalWeight : float3(0.0f, 0.0f, 0.0f);
}

/// `far_field_weight`.
float grFarFieldWeight(float distance) {
    const float4 far = asfloat(giResolve.constants[kGrFar]);
    const float begin = far.x;
    if (begin <= 0.0f) {
        return 1.0f;
    }
    const float ratio = far.y > 0.0f ? far.z / far.y : 8.0f;
    const float width = begin * gpClamp(ratio / 100.0f, 0.15f, 1.0f);
    if (distance <= begin) {
        return 0.0f;
    }
    const float t = gpClamp((distance - begin) / width, 0.0f, 1.0f);
    return t * t * (3.0f - (2.0f * t));
}

/// `DistanceField::sky_visibility_ray`.
float grSkyVisibilityRay(float3 position, float3 normal, uint index, uint count) {
    if (count == 0u || giLevelCount() == 0u) {
        return 1.0f;
    }
    const float reach = asfloat(giResolve.constants[kGrFar].w);
    const float voxel = giFinestVoxel();
    const float3 origin = position + (normal * (voxel * 2.0f));
    const float3 direction = giHemisphereDirection(normal, index, count);
    const GiTraceHit hit = giSphereTrace(origin, direction, reach, 0.0f);
    if (hit.hit) {
        return 0.0f;
    }
    return gpClamp(hit.closest / (voxel * 4.0f), 0.0f, 1.0f);
}

/// `reflection_ray_count`.
uint grReflectionRays(float roughness) {
    const uint4 words = giResolve.constants[kGrReflect];
    const float mirror = asfloat(words.x);
    const float cacheMin = asfloat(words.y);
    const uint maxRays = max(1u, words.z);
    if (roughness <= mirror) {
        return maxRays;
    }
    if (roughness < cacheMin) {
        const float span = max(cacheMin - mirror, 1.0e-4f);
        const float t = gpClamp((roughness - mirror) / span, 0.0f, 1.0f);
        const float rays = float(maxRays) + ((1.0f - float(maxRays)) * t);
        // `std::lround`, half away from zero, for a count that is never negative. `round` would be
        // the target's choice of tie rule.
        return max(1u, uint(floor(rays + 0.5f)));
    }
    return 0u;
}

/// `reflection_lobe_direction`.
float3 grLobeDirection(float3 reflection, float3 normal, float roughness, uint index, uint count,
                       uint rotation) {
    const float3 axis = giNormalisedOr(reflection, normal);
    if (count <= 1u || roughness <= 0.0f) {
        return axis;
    }
    const float golden = 2.39996323f;
    const float offset = (float(index) + 0.5f) / float(count);
    const float spread = gpClamp(roughness, 0.0f, 1.0f) * 0.7f;
    const float radius = sqrt(offset) * spread;
    const float angle = (golden * float(index)) + (float(rotation) * 0.61803399f);
    float3 tangent = abs(axis.y) < 0.99f ? cross(float3(0.0f, 1.0f, 0.0f), axis)
                                         : cross(float3(1.0f, 0.0f, 0.0f), axis);
    tangent = giNormalisedOr(tangent, float3(1.0f, 0.0f, 0.0f));
    const float3 bitangent = cross(axis, tangent);
    const float3 direction =
        axis + (tangent * (radius * cos(angle))) + (bitangent * (radius * sin(angle)));
    const float3 unit = giNormalisedOr(direction, axis);
    return dot(unit, normal) < 0.0f ? giNormalisedOr(unit - (normal * (2.0f * dot(unit, normal))), axis)
                                    : unit;
}

/// `world_ray` in screen_resolve.cpp: one ray through world tier `T`, and none from a hit whose
/// surface faces away from the ray — a ray that started inside a thin wall and met its far face.
///
/// The ray leaves the surface lifted along its normal by `lift` for the hardware tier and by at
/// least one finest voxel for the software tier, whose hit threshold is half of one.
GrSample grWorldRay<T : IGiWorldTier>(float3 position, float3 normal, float3 unit, float lift,
                                      float reach) {
    const uint cacheFrame = giProbes.constants[kGpBudget].w;
    const float height = T.source() == kGpSourceHardware ? lift : max(lift, giFinestVoxel());
    const GiWorldHit hit = T.trace(position + (normal * height), unit, reach);
    if (hit.hit && dot(hit.normal, unit) > 0.0f) {
        return grSample(float3(0.0f, 0.0f, 0.0f), 0.0f, kGrSourceNone);
    }
    const GiTierSample tier = gpWorldRadiance(hit, unit, T.source(), cacheFrame);
    return grSample(tier.radiance, tier.confidence, tier.source);
}

// --- The cache through the field: `RadianceCache::sample_traced` ------------------------------------

/// `probe_occluded` in radiance_cache.cpp: whether the field stops a trace from `from` before the
/// probe at `to`, short of the probe's own voxel — answered by the two ends' empty spheres alone
/// where they cover the segment.
bool grProbeOccluded(float3 from, float3 to) {
    const float3 offset = to - from;
    const float span = length(offset);
    const float voxel = giFinestVoxel();
    if (span <= voxel * 2.0f) {
        return false;
    }
    if (giDistance(from) + giDistance(to) >= span) {
        return false;
    }
    return giSphereTrace(from, offset / span, span - voxel, 0.0f).hit;
}

/// `facing_weight` in radiance_cache.cpp.
float grFacing(float3 toward, float3 normal) {
    return gpClamp((dot(normal, toward) + 1.0f) * 0.5f, 0.0f, 1.0f);
}

/// `TracedSums::finish`.
float4 grFinish(float3 total, float weight, float oldest) {
    if (weight <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float staleness = gpClamp(oldest / 240.0f, 0.0f, 1.0f);
    return float4(total / weight, gpClamp(1.0f - (staleness * 0.75f), 0.0f, 1.0f));
}

/// `RadianceCache::sample_traced`: both signals' cache answers over the probes within one
/// spacing, every one that would contribute traced to from the surface lifted along its normal.
/// The probes are visited cell by cell rather than in the host's spatial-index order, which is the
/// one difference and a last-bit one, as `gpSample`'s is.
void grSampleTraced(float3 position, float3 normal, float3 reflection, uint frame,
                    out float4 diffuse, out float4 specular) {
    const uint4 counts = giProbes.constants[kGpCounts];
    const uint encoding = counts.x;
    const uint floats = counts.y;
    const uint levels = asuint(giProbes.constants[kGpGather].z);
    const float base = asfloat(giProbes.constants[kGpLayout].x);
    const float3 lifted = position + (normal * asfloat(giProbes.constants[kGpGather].y));
    float3 diffuseTotal = float3(0.0f, 0.0f, 0.0f);
    float diffuseWeight = 0.0f;
    float diffuseOldest = 0.0f;
    float3 specularTotal = float3(0.0f, 0.0f, 0.0f);
    float specularWeight = 0.0f;
    float specularOldest = 0.0f;
    for (uint level = 0u; level < levels; ++level) {
        const float spacing = base * float(1u << level);
        const int3 lo = int3(gpFloorDiv(position.x - spacing, spacing),
                             gpFloorDiv(position.y - spacing, spacing),
                             gpFloorDiv(position.z - spacing, spacing));
        const int3 hi = int3(gpFloorDiv(position.x + spacing, spacing),
                             gpFloorDiv(position.y + spacing, spacing),
                             gpFloorDiv(position.z + spacing, spacing));
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int y = lo.y; y <= hi.y; ++y) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    const uint probe = gpLayoutProbe(level, int3(x, y, z));
                    if (probe == kGpNoProbe) {
                        continue;
                    }
                    const float4 record = giProbes.probes[(probe * 4u) + 1u];
                    if (record.y == 0.0f || record.z == 0.0f) {
                        continue;
                    }
                    const float3 at = giProbes.probes[probe * 4u].xyz;
                    const float3 offset = position - at;
                    const float span = length(offset);
                    if (span > spacing) {
                        continue;
                    }
                    const float falloff = (1.0f - gpClamp(span / spacing, 0.0f, 1.0f)) *
                                          gpVisibilityWeight(probe, at, position);
                    const bool apart = span > 1.0e-4f;
                    const float3 toward = apart ? -offset / span : normal;
                    const float diffuseShare = apart ? falloff * grFacing(toward, normal) : falloff;
                    const float specularShare =
                        apart ? falloff * grFacing(toward, reflection) : falloff;
                    if ((diffuseShare <= 0.0f && specularShare <= 0.0f) ||
                        grProbeOccluded(lifted, at)) {
                        continue;
                    }
                    float data[kGpMaxPayload];
                    gpLoadPayload(probe, floats, data);
                    const uint last = asuint(record.x);
                    const float age = frame > last ? float(frame - last) : 0.0f;
                    if (diffuseShare > 0.0f) {
                        diffuseTotal = diffuseTotal + (gpDecode(encoding, data, normal) * diffuseShare);
                        diffuseWeight += diffuseShare;
                        diffuseOldest = max(diffuseOldest, age);
                    }
                    if (specularShare > 0.0f) {
                        specularTotal =
                            specularTotal + (gpDecode(encoding, data, reflection) * specularShare);
                        specularWeight += specularShare;
                        specularOldest = max(specularOldest, age);
                    }
                }
            }
        }
    }
    diffuse = grFinish(diffuseTotal, diffuseWeight, diffuseOldest);
    specular = grFinish(specularTotal, specularWeight, specularOldest);
}

// --- One pixel: `resolve_screen_pixel` -----------------------------------------------------------

/// `resolve_diffuse` in screen_resolve.cpp. `traced` says whether the pixel spent its ray.
float3 grResolveDiffuse<T : IGiWorldTier>(uint pixel, float3 position, float3 normal,
                                          float distance, float4 cached, uint frame) {
    const float4 rays = asfloat(giResolve.constants[kGrRays]);
    const uint excluded = giResolve.constants[kGrRays].w;
    const float density = asfloat(giResolve.constants[kGrCamera].w);
    GrCombine state = grCombineBegin();

    grCombineAdd(state, grSample(cached.xyz, cached.w, kGrSourceCache), excluded);

    if (grTraces(pixel, frame, density)) {
        const uint hash = grHash(pixel, frame, kGrStreamDirection);
        const float3 direction =
            grCosineDirection(normal, grUnit(hash), grUnit(hash * 0x27220A95u));
        const float3 unit = giNormalisedOr(direction, float3(0.0f, 0.0f, -1.0f));
        grCombineAdd(state, grWorldRay<T>(position, normal, unit, rays.y, rays.x), excluded);
    }

    // `far_sky`: zero confidence in the near field, where it is not even traced.
    GrSample sky = grSample(float3(0.0f, 0.0f, 0.0f), 0.0f, kGrSourceSky);
    const float far = grFarFieldWeight(distance);
    if (far > 0.0f) {
        float openness = 1.0f;
        const uint count = asuint(rays.z);
        if (count > 0u) {
            openness = grSkyVisibilityRay(position, normal, (frame + pixel) % count, count);
        }
        sky.radiance = gpSky(normal) * openness;
        sky.confidence = gpClamp(far * openness, 0.0f, 1.0f);
    }
    grCombineAdd(state, sky, excluded);
    return grCombineEnd(state) * asfloat(giResolve.constants[kGrIntensity].x);
}

/// `resolve_specular` in screen_resolve.cpp.
float3 grResolveSpecular<T : IGiWorldTier>(uint pixel, float3 position, float3 normal,
                                           float roughness, float3 reflection, float4 cached,
                                           uint frame) {
    const float4 rays = asfloat(giResolve.constants[kGrRays]);
    const float density = asfloat(giResolve.constants[kGrCamera].w);
    GrCombine state = grCombineBegin();
    const uint count = grReflectionRays(roughness);
    if (count > 0u && grTraces(pixel, frame, density)) {
        const uint hash = grHash(pixel, frame, kGrStreamSpecular);
        const float3 direction =
            grLobeDirection(reflection, normal, roughness, hash % count, count, frame);
        const float3 unit = giNormalisedOr(direction, float3(0.0f, 0.0f, -1.0f));
        grCombineAdd(state, grWorldRay<T>(position, normal, unit, rays.y, rays.x), 0u);
    }
    if (cached.w > 0.0f) {
        grCombineAdd(state,
                     grSample(cached.xyz, cached.w * gpClamp(roughness, 0.15f, 1.0f),
                              kGrSourceCache),
                     0u);
    }
    return grCombineEnd(state) * asfloat(giResolve.constants[kGrIntensity].y);
}

/// `ScreenResolve::produce` for one pixel of this frame's surface slot.
void grResolvePixel<T : IGiWorldTier>(uint pixel) {
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
    const uint frame = giResolve.constants[kGrFrame].x;
    const uint cacheFrame = giResolve.constants[kGrReflect].w;
    const float3 position = at.xyz;
    const float3 normal = giNormalisedOr(facing.xyz, float3(0.0f, 1.0f, 0.0f));
    const float3 offset = position - asfloat(giResolve.constants[kGrCamera].xyz);
    // `reflect_view` and `cache_samples`: one traced cache query answers both signals.
    const float3 incident = giNormalisedOr(offset, float3(0.0f, 0.0f, -1.0f));
    const float3 reflection =
        giNormalisedOr(incident - (normal * (2.0f * dot(incident, normal))), normal);
    float4 cachedDiffuse;
    float4 cachedSpecular;
    grSampleTraced(position, normal, reflection, cacheFrame, cachedDiffuse, cachedSpecular);
    const float3 diffuse =
        grResolveDiffuse<T>(pixel, position, normal, length(offset), cachedDiffuse, frame);
    const float3 specular = grResolveSpecular<T>(pixel, position, normal, facing.w, reflection,
                                                 cachedSpecular, frame);
    giResolve.noisy[pixel * 2u] = float4(diffuse, 1.0f);
    giResolve.noisy[(pixel * 2u) + 1u] = float4(specular, 1.0f);
}
