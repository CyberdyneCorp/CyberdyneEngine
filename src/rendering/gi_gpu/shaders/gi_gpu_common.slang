// SPDX-License-Identifier: MIT
// CyberGI on the device: the one descriptor set, the constant words, and the host functions this
// module transcribes. Issue #35, stages 1 and 2. Include-only; the entry points are gi_trace.slang
// and gi_cards.slang.
//
// ================================================================================================
// EVERY FUNCTION BELOW IS A TRANSCRIPTION, AND NAMES WHAT IT TRANSCRIBES
// ================================================================================================
//
// `cy::rendering-gi` is the oracle. The field read is `DistanceField::distance`, the march is
// `DistanceField::sphere_trace`, the shadow test is `ShadowMap::shadowed` behind
// `ShadowMapOccluder`, the lookup is `CardSnapshot::find`, the gather is `CardGather::gather`, and
// the card update is `SurfaceCache::shade` over `shaded_direct`. Each is written in the order the
// C++ writes it — a reordering that is equal in exact arithmetic is a test failure in float — and
// `render.gi_gpu` runs both over one scene and compares.
//
// ================================================================================================
// EVERY BUFFER IS A FLAT ARRAY OF WORDS OR VEC4S
// ================================================================================================
//
// A structured buffer of a struct has a stride that is a property of each target's layout rules.
// So every record here is a whole number of `float4`s (or single words), and its layout is stated
// once, below, in the same terms `gpu_scene.cpp` writes it in.

// --- The constant words: `GpuGiConstants` in gpu_scene.cpp, one uint4 each ------------------------

static const uint kGiCounts = 0u;         // level count, window bricks, light count, gather rays
static const uint kGiGrid = 1u;           // bucket count, cell, lookup radius, max ray distance
static const uint kGiSkyZenith = 2u;      // xyz zenith, w intensity
static const uint kGiSkyHorizon = 3u;     // xyz horizon
static const uint kGiSkyGround = 4u;      // xyz ground
static const uint kGiShadowCentre = 5u;   // xyz centre, w half extent
static const uint kGiShadowDir = 6u;      // xyz direction the light travels, w depth range
static const uint kGiShadowRight = 7u;    // xyz right, w resolution (a uint)
static const uint kGiShadowUp = 8u;       // xyz up, w bias
static const uint kGiShadowEnabled = 9u;  // x enabled, y finest voxel size
static const uint kGiLevels = 16u;        // two words per level: [voxel, brick, far], [origin]

static const uint kGiEmptyBrick = 0xFFFFFFFFu;
static const uint kGiBrickEdge = 4u;
static const uint kGiBrickVoxels = 64u;
static const uint kGiGroupSize = 64u;

/// `kCardLookupAlignment` and `kShadowMapAlignment`.
static const float kGiCardAlignment = 0.25f;
static const float kGiShadowAlignment = 0.9999f;

struct GiSceneSet {
    /// `GpuGiConstants`: 32 uint4 words, the layout above.
    StructuredBuffer<uint4> constants;
    /// One word per brick of every level's window, toroidally addressed: a pool slot, or empty.
    StructuredBuffer<uint> pageTable;
    /// `DistanceField::brick_pool()`, 64 floats per slot.
    StructuredBuffer<float> bricks;
    /// Four float4 per card: [position, area], [normal, roughness], [albedo, live], [emission, 0].
    StructuredBuffer<float4> cards;
    /// Two float4 per card: [outgoing, valid], [accumulated, 0]. Last frame's radiance while a
    /// shading dispatch reads it; this frame's once the commit has run.
    RWStructuredBuffer<float4> cardState;
    /// `CardGrid::ranges()` and `::items()`.
    StructuredBuffer<uint> gridRanges;
    StructuredBuffer<uint> gridItems;
    /// Four float4 per light: [position, intensity], [direction, range], [colour, directional], 0.
    StructuredBuffer<float4> lights;
    /// `ShadowMap::depths()`.
    StructuredBuffer<float> shadowDepths;
    /// The page handles one update shades, in the order `SurfaceCache::select` chose them.
    StructuredBuffer<uint> selection;
    /// Three float4 per selected page: [direct, error], [accumulated, handle], [outgoing, 1].
    RWStructuredBuffer<float4> results;
    /// Two float4 per ray: [origin, max distance], [direction, t_min].
    StructuredBuffer<float4> rays;
    /// Three float4 per ray: [hit, t, closest approach, exhausted], [position, 0], [normal, 0].
    RWStructuredBuffer<float4> hits;
};

struct GiDispatch {
    /// How many items this dispatch covers: selected pages, or rays.
    uint count;
    uint frame;
    uint reserved0;
    uint reserved1;
};

/// One descriptor set in SPIR-V and one argument buffer in MSL, for the reason skin.slang gives.
[[vk::binding(0, 0)]] ParameterBlock<GiSceneSet> giScene;
[[vk::push_constant]] GiDispatch giDispatch;

// --- Small things -------------------------------------------------------------------------------

float giLerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

float3 giNormalisedOr(float3 v, float3 fallback) {
    const float squared = dot(v, v);
    return squared > 1e-8f ? v * (1.0f / sqrt(squared)) : fallback;
}

/// `relative_change` in surface_cache.cpp.
float giRelativeChange(float3 before, float3 after) {
    const float magnitude = length(before) + length(after);
    if (magnitude <= 1.0e-5f) {
        return 0.0f;
    }
    return length(after - before) / magnitude;
}

// --- The field: `DistanceField::distance` ----------------------------------------------------------

uint giLevelCount() {
    return giScene.constants[kGiCounts].x;
}

uint giWindowBricks() {
    return giScene.constants[kGiCounts].y;
}

float giFinestVoxel() {
    return asfloat(giScene.constants[kGiShadowEnabled].y);
}

/// The anonymous `trilinear` in distance_field.cpp, over one pool slot.
float giTrilinear(uint slot, float fx, float fy, float fz) {
    const int edge = int(kGiBrickEdge) - 1;
    const int x0 = clamp(int(floor(fx)), 0, edge);
    const int y0 = clamp(int(floor(fy)), 0, edge);
    const int z0 = clamp(int(floor(fz)), 0, edge);
    const int x1 = clamp(x0 + 1, 0, edge);
    const int y1 = clamp(y0 + 1, 0, edge);
    const int z1 = clamp(z0 + 1, 0, edge);
    const float tx = clamp(fx - float(x0), 0.0f, 1.0f);
    const float ty = clamp(fy - float(y0), 0.0f, 1.0f);
    const float tz = clamp(fz - float(z0), 0.0f, 1.0f);
    const uint base = slot * kGiBrickVoxels;
    const uint e = kGiBrickEdge;
    const float c000 = giScene.bricks[base + ((uint(z0) * e + uint(y0)) * e) + uint(x0)];
    const float c100 = giScene.bricks[base + ((uint(z0) * e + uint(y0)) * e) + uint(x1)];
    const float c010 = giScene.bricks[base + ((uint(z0) * e + uint(y1)) * e) + uint(x0)];
    const float c110 = giScene.bricks[base + ((uint(z0) * e + uint(y1)) * e) + uint(x1)];
    const float c001 = giScene.bricks[base + ((uint(z1) * e + uint(y0)) * e) + uint(x0)];
    const float c101 = giScene.bricks[base + ((uint(z1) * e + uint(y0)) * e) + uint(x1)];
    const float c011 = giScene.bricks[base + ((uint(z1) * e + uint(y1)) * e) + uint(x0)];
    const float c111 = giScene.bricks[base + ((uint(z1) * e + uint(y1)) * e) + uint(x1)];
    const float c00 = giLerp(c000, c100, tx);
    const float c10 = giLerp(c010, c110, tx);
    const float c01 = giLerp(c001, c101, tx);
    const float c11 = giLerp(c011, c111, tx);
    return giLerp(giLerp(c00, c10, ty), giLerp(c01, c11, ty), tz);
}

int giWrap(int value, int modulus) {
    return ((value % modulus) + modulus) % modulus;
}

/// `DistanceField::sample_level`. The host finds a brick in a hash and treats a missing key as far;
/// here the window test is that miss — the host retires every brick outside the window at each
/// scroll, so a key outside it is exactly a key the hash does not hold.
float giSampleLevel(uint level, float3 p, out float farDistance) {
    const float4 shape = asfloat(giScene.constants[kGiLevels + (level * 2u)]);
    const int4 origin = asint(giScene.constants[kGiLevels + (level * 2u) + 1u]);
    const float voxel = shape.x;
    const float brick = shape.y;
    farDistance = shape.z;
    const int bx = int(floor(p.x / brick));
    const int by = int(floor(p.y / brick));
    const int bz = int(floor(p.z / brick));
    const int dim = int(giWindowBricks());
    if (bx < origin.x || by < origin.y || bz < origin.z || bx >= origin.x + dim ||
        by >= origin.y + dim || bz >= origin.z + dim) {
        return farDistance;
    }
    const uint cell = ((uint(giWrap(bz, dim)) * uint(dim) + uint(giWrap(by, dim))) * uint(dim)) +
                      uint(giWrap(bx, dim));
    const uint slot = giScene.pageTable[(level * uint(dim * dim * dim)) + cell];
    if (slot == kGiEmptyBrick) {
        return farDistance;
    }
    const float3 corner = float3(float(bx) * brick, float(by) * brick, float(bz) * brick);
    const float3 local = (p - corner) / voxel;
    return giTrilinear(slot, local.x, local.y, local.z);
}

/// `DistanceField::distance`.
float giDistance(float3 p) {
    float answer = 1.0e6f;
    const uint levels = giLevelCount();
    for (uint level = 0u; level < levels; ++level) {
        float farDistance = 0.0f;
        const float sampled = giSampleLevel(level, p, farDistance);
        answer = min(answer, sampled);
        if (sampled < farDistance) {
            return sampled;
        }
    }
    return answer;
}

struct GiTraceHit {
    bool hit;
    bool exhausted;
    float t;
    float closest;
    float3 position;
    float3 normal;
};

/// `DistanceField::sphere_trace`, including the delayed closest-approach window.
GiTraceHit giSphereTrace(float3 origin, float3 direction, float maxDistance, float tMin) {
    GiTraceHit result;
    result.hit = false;
    result.exhausted = false;
    result.t = 0.0f;
    result.closest = 0.0f;
    result.position = float3(0.0f, 0.0f, 0.0f);
    result.normal = float3(0.0f, 1.0f, 0.0f);
    if (giLevelCount() == 0u) {
        return result;
    }
    const float voxel = giFinestVoxel();
    const float surface = voxel * 0.5f;
    float t = max(tMin, surface);
    float recent[3] = { 1.0e6f, 1.0e6f, 1.0e6f };
    uint recentSlot = 0u;
    float closest = 1.0e6f;
    for (uint step = 0u; step < 192u; ++step) {
        if (t > maxDistance) {
            closest = min(closest, min(recent[0], min(recent[1], recent[2])));
            result.closest = closest;
            return result;
        }
        const float3 position = origin + (direction * t);
        const float d = giDistance(position);
        if (d < surface) {
            result.hit = true;
            result.t = t;
            result.position = position;
            const float h = voxel;
            const float3 gradient = float3(
                giDistance(position + float3(h, 0.0f, 0.0f)) -
                    giDistance(position - float3(h, 0.0f, 0.0f)),
                giDistance(position + float3(0.0f, h, 0.0f)) -
                    giDistance(position - float3(0.0f, h, 0.0f)),
                giDistance(position + float3(0.0f, 0.0f, h)) -
                    giDistance(position - float3(0.0f, 0.0f, h)));
            result.normal = giNormalisedOr(gradient, -direction);
            result.closest = closest;
            return result;
        }
        closest = min(closest, recent[recentSlot]);
        recent[recentSlot] = d;
        recentSlot = (recentSlot + 1u) % 3u;
        t += max(d, surface);
    }
    result.exhausted = true;
    closest = min(closest, min(recent[0], min(recent[1], recent[2])));
    result.closest = closest;
    return result;
}

/// `hemisphere_direction` in distance_field.cpp.
float3 giHemisphereDirection(float3 normal, uint index, uint count) {
    const float golden = 2.39996323f;
    const float offset = (float(index) + 0.5f) / float(count);
    const float radius = sqrt(offset);
    const float angle = golden * float(index);
    const float x = radius * cos(angle);
    const float y = radius * sin(angle);
    const float z = sqrt(max(0.0f, 1.0f - (x * x) - (y * y)));
    float3 tangent = abs(normal.y) < 0.99f ? cross(float3(0.0f, 1.0f, 0.0f), normal)
                                            : cross(float3(1.0f, 0.0f, 0.0f), normal);
    tangent = giNormalisedOr(tangent, float3(1.0f, 0.0f, 0.0f));
    const float3 bitangent = cross(normal, tangent);
    return giNormalisedOr((tangent * x) + (bitangent * y) + (normal * z), normal);
}

// --- Occlusion: `ShadowMapOccluder` over `ShadowMap` and `SoftwareTracer::occluded` -------------

bool giShadowMapShadowed(float3 p) {
    const float4 centre = asfloat(giScene.constants[kGiShadowCentre]);
    const float4 dir = asfloat(giScene.constants[kGiShadowDir]);
    const float4 right = asfloat(giScene.constants[kGiShadowRight]);
    const float4 up = asfloat(giScene.constants[kGiShadowUp]);
    const uint resolution = giScene.constants[kGiShadowRight].w;
    const float extent = centre.w;
    const float3 relative = p - centre.xyz;
    const float u = dot(relative, right.xyz);
    const float v = dot(relative, up.xyz);
    const float depth = dot(relative, dir.xyz) + (dir.w * 0.5f);
    if (abs(u) >= extent || abs(v) >= extent || depth < 0.0f || depth > dir.w) {
        return false;
    }
    const int last = int(resolution) - 1;
    const int column = clamp(int(floor((((u / extent) * 0.5f) + 0.5f) * float(resolution))), 0, last);
    const int row = clamp(int(floor((((v / extent) * 0.5f) + 0.5f) * float(resolution))), 0, last);
    const float stored = giScene.shadowDepths[(uint(row) * resolution) + uint(column)];
    return stored < depth - up.w;
}

/// `SoftwareTracer::occluded`.
bool giFieldOccluded(float3 from, float3 to) {
    const float3 offset = to - from;
    const float span = length(offset);
    if (span <= 1.0e-4f) {
        return false;
    }
    const float bias = giFinestVoxel() * 2.0f;
    if (span <= bias) {
        return false;
    }
    return giSphereTrace(from, offset / span, span * 0.999f, bias).hit;
}

/// `ShadowMapOccluder::occluded`, with the field as its fallback.
bool giOccluded(float3 from, float3 to) {
    const float3 offset = to - from;
    const float span = length(offset);
    if (span <= 1.0e-4f) {
        return false;
    }
    const float3 dir = asfloat(giScene.constants[kGiShadowDir].xyz);
    if (giScene.constants[kGiShadowEnabled].x != 0u && dot(offset / span, -dir) > kGiShadowAlignment) {
        return giShadowMapShadowed(from);
    }
    return giFieldOccluded(from, to);
}

// --- Direct light: `direct_radiance` and `shaded_direct` ----------------------------------------

float3 giDirectRadiance(uint light, float3 position, float3 normal) {
    const float4 a = giScene.lights[(light * 4u) + 0u];
    const float4 b = giScene.lights[(light * 4u) + 1u];
    const float4 c = giScene.lights[(light * 4u) + 2u];
    if (c.w != 0.0f) {
        const float3 toLight = -giNormalisedOr(b.xyz, float3(0.0f, 1.0f, 0.0f));
        const float cosine = max(0.0f, dot(normal, toLight));
        return c.xyz * (a.w * cosine);
    }
    const float3 offset = a.xyz - position;
    const float distanceSquared = max(dot(offset, offset), 1.0e-6f);
    if (b.w > 0.0f && distanceSquared > b.w * b.w) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 toLight = offset / sqrt(distanceSquared);
    const float cosine = max(0.0f, dot(normal, toLight));
    return c.xyz * (a.w * cosine / distanceSquared);
}

float3 giShadedDirect(float3 position, float3 normal) {
    float3 total = float3(0.0f, 0.0f, 0.0f);
    const uint count = giScene.constants[kGiCounts].z;
    for (uint light = 0u; light < count; ++light) {
        const float3 contribution = giDirectRadiance(light, position, normal);
        if (contribution.x <= 0.0f && contribution.y <= 0.0f && contribution.z <= 0.0f) {
            continue;
        }
        const float4 a = giScene.lights[(light * 4u) + 0u];
        const float4 b = giScene.lights[(light * 4u) + 1u];
        const float4 c = giScene.lights[(light * 4u) + 2u];
        const float3 target =
            c.w != 0.0f ? position - (giNormalisedOr(b.xyz, float3(0.0f, 1.0f, 0.0f)) * 1000.0f)
                        : a.xyz;
        if (giOccluded(position + (normal * 1.0e-2f), target)) {
            continue;
        }
        total = total + contribution;
    }
    return total;
}

// --- The sky: `SkyTerm::radiance` ----------------------------------------------------------------

float3 giSky(float3 direction) {
    const float4 zenith = asfloat(giScene.constants[kGiSkyZenith]);
    const float3 horizon = asfloat(giScene.constants[kGiSkyHorizon].xyz);
    const float3 ground = asfloat(giScene.constants[kGiSkyGround].xyz);
    const float3 unit = giNormalisedOr(direction, float3(0.0f, 1.0f, 0.0f));
    if (unit.y < 0.0f) {
        return ground * zenith.w;
    }
    const float t = clamp(pow(unit.y, 0.45f), 0.0f, 1.0f);
    return (horizon + ((zenith.xyz - horizon) * t)) * zenith.w;
}

// --- The lookup: `CardSnapshot::find` ------------------------------------------------------------

int giCell(float value, float cell) {
    return int(floor(value / cell));
}

uint giBucket(int x, int y, int z, uint buckets) {
    const uint hash = (uint(x) * 73856093u) ^ (uint(y) * 19349663u) ^ (uint(z) * 83492791u);
    return hash & (buckets - 1u);
}

/// The card a hit at `position` facing `normal` resolves through, or 0xFFFFFFFF.
uint giFindCard(float3 position, float3 normal) {
    const uint4 grid = giScene.constants[kGiGrid];
    const uint buckets = grid.x;
    if (buckets == 0u) {
        return 0xFFFFFFFFu;
    }
    const float cell = asfloat(grid.y);
    const float radius = asfloat(grid.z);
    const float radiusSquared = radius * radius;
    const int3 lo = int3(giCell(position.x - radius, cell), giCell(position.y - radius, cell),
                         giCell(position.z - radius, cell));
    const int3 hi = int3(giCell(position.x + radius, cell), giCell(position.y + radius, cell),
                         giCell(position.z + radius, cell));
    uint best = 0xFFFFFFFFu;
    float bestScore = 1.0e9f;
    float bestAlignment = 0.0f;
    for (int z = lo.z; z <= hi.z; ++z) {
        for (int y = lo.y; y <= hi.y; ++y) {
            for (int x = lo.x; x <= hi.x; ++x) {
                const uint bucket = giBucket(x, y, z, buckets);
                const uint first = giScene.gridRanges[bucket * 2u];
                const uint count = giScene.gridRanges[(bucket * 2u) + 1u];
                for (uint item = first; item < first + count; ++item) {
                    const uint handle = giScene.gridItems[item];
                    if (giScene.cardState[handle * 2u].w == 0.0f) {
                        continue;
                    }
                    const float3 candidateNormal = giScene.cards[(handle * 4u) + 1u].xyz;
                    const float alignment = dot(candidateNormal, normal);
                    if (alignment <= kGiCardAlignment) {
                        continue;
                    }
                    const float3 offset = giScene.cards[handle * 4u].xyz - position;
                    const float squared = dot(offset, offset);
                    if (squared > radiusSquared) {
                        continue;
                    }
                    // `card_lookup_prefers`: the score, then the alignment, then the handle.
                    const float score = squared / alignment;
                    const bool better = score != bestScore ? score < bestScore
                                        : alignment != bestAlignment ? alignment > bestAlignment
                                                                     : handle < best;
                    if (better) {
                        bestScore = score;
                        bestAlignment = alignment;
                        best = handle;
                    }
                }
            }
        }
    }
    return best;
}

// --- The gather: `CardGather::gather` ------------------------------------------------------------

float3 giGather(float3 position, float3 normal) {
    const uint rays = giScene.constants[kGiCounts].w;
    if (rays == 0u || giLevelCount() == 0u) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float maxDistance = asfloat(giScene.constants[kGiGrid].w);
    const float voxel = giFinestVoxel();
    const float bias = voxel * 2.0f;
    const float3 origin = position + (normal * voxel);
    float3 total = float3(0.0f, 0.0f, 0.0f);
    for (uint index = 0u; index < rays; ++index) {
        const float3 direction = giHemisphereDirection(normal, index, rays);
        const GiTraceHit hit = giSphereTrace(origin, direction, maxDistance, bias);
        if (!hit.hit) {
            total = total + giSky(direction);
            continue;
        }
        const uint card = giFindCard(hit.position, hit.normal);
        if (card != 0xFFFFFFFFu) {
            total = total + giScene.cardState[card * 2u].xyz;
        }
    }
    return total / float(rays);
}
