// SPDX-License-Identifier: MIT
// CyberGI on the device: the nineteen GI debug views, drawn from the device frame's own GI state.
// Issue #35, the live half. Include-only; the entry points are gi_debug_views.slang (the software
// world tier) and gi_debug_views_hardware.slang (ray queries).
//
// ================================================================================================
// WHAT EACH VIEW DRAWS, AND THE HOST FUNCTION IT TRANSCRIBES
// ================================================================================================
//
// `gi/debug_views.cpp` is the oracle, and each function below names the one it transcribes, in its
// order. The pixels are the frame's own: this frame's surfaces as the resolve unpacked them from
// the prepass (`GpuScreenResolve`'s surface slot), its published indirect diffuse and specular,
// and its reprojection's history — so a view shows the GI the frame was lit with, on the geometry
// the frame drew. Where the device frame holds a datum the host's view computes another way, the
// view draws the device's, and says so:
//
//   GiIndirectDiffuse, GiIndirectSpecular   the published targets the forward pass composites
//   GiDenoiserConfidence, GiDisocclusion    the reprojection's history (`screen_history`)
//   GiConfidence                            `RadianceCache::sample`'s confidence at the surface
//   GiCards                                 the surface-cache page `CardSnapshot::find` resolves
//   GiScreenTrace                           red: the device frame has no screen tier
//   GiTier, GiRayCount                      the reflection ray through the frame's world tier
//
// The output is one packed `Rgba8Unorm` word a frame pixel, red in the low byte and alpha zero
// where the view draws nothing: exactly `render_gi_debug_view`'s, so a test compares words.
//
// ================================================================================================
// THE SETS
// ================================================================================================
//
// 0 the scene and 1 the probe set, as every GI dispatch binds them; 2 this file's own; 3 the
// hardware tier's top level (gi_debug_views_hardware.slang only). Four, the fewest a Vulkan device
// may bound: the resolve's constants and surfaces are bound HERE rather than through the resolve's
// own set, which would make a fifth.

#include "gi_probe_common.slang"

// --- The resolve's words this file reads (`gi_resolve_common.slang`'s numbering) ----------------

static const uint kGdrCounts = 0u;       // grid width, grid height, grid pixels, current slot
static const uint kGdrRays = 2u;         // max ray distance, ...
static const uint kGdrReflect = 4u;      // mirror max roughness, cache min roughness, max rays
static const uint kGdrWorldToClip = 6u;  // four columns: UNJITTERED world to clip

// --- This file's words: `GpuGiDebugView` in gpu_debug_view.cpp ----------------------------------

static const uint kGdView = 0u;      // view, frame width, frame height, grid divisor
static const uint kGdFrame = 1u;     // exposure (0 automatic), frame, max age, probe slots
static const uint kGdCamera = 2u;    // xyz camera, w disc metres
static const uint kGdField = 3u;     // slice height, emitter lights, hardware tier, reflection probes
static const uint kGdCache = 4u;     // xyz the radiance cache's camera, w probe levels
static const uint kGdLayout = 5u;    // base spacing, half extent probes, 0, 0

/// `GiDebugView`'s order.
static const uint kGdProbes = 0u;
static const uint kGdProbeIrradiance = 1u;
static const uint kGdProbeAge = 2u;
static const uint kGdProbeError = 3u;
static const uint kGdProbeVisibility = 4u;
static const uint kGdRadianceClipmap = 5u;
static const uint kGdSurfaceCache = 6u;
static const uint kGdDistanceField = 7u;
static const uint kGdCards = 8u;
static const uint kGdReflectionProbes = 9u;
static const uint kGdScreenTrace = 10u;
static const uint kGdConfidence = 11u;
static const uint kGdTier = 12u;
static const uint kGdRayCount = 13u;
static const uint kGdDenoiserConfidence = 14u;
static const uint kGdDisocclusion = 15u;
static const uint kGdIndirectDiffuse = 16u;
static const uint kGdIndirectSpecular = 17u;
static const uint kGdEmissive = 18u;

/// `kEmitterShadowLiftMetres`.
static const float kGdEmitterShadowLift = 0.3f;

struct GiDebugSet {
    /// The resolve's constant words for this frame.
    StructuredBuffer<uint4> resolveConstants;
    /// The resolve's two surface slots: [world position, view depth], [unit normal, roughness].
    StructuredBuffer<float4> surfaces;
    /// The reprojection's answer, two words a grid cell: the history cell (an int, -1 for none)
    /// and its confidence.
    StructuredBuffer<uint> sources;
    /// The published targets the forward pass composites, one texel a frame pixel.
    Texture2D<float4> diffuse;
    Texture2D<float4> specular;
    /// The words above.
    StructuredBuffer<uint4> words;
    /// The promoted emitters (`EmissiveLights::lights()`), four float4 each, as the scene's lights.
    StructuredBuffer<float4> emitters;
    /// One word a probe slot: a leak suspect this frame (`GiProbeVisibility`).
    RWStructuredBuffer<uint> suspects;
    /// [0]: the exposure this frame's views are drawn at.
    RWStructuredBuffer<float> exposure;
    /// One packed `Rgba8Unorm` word a frame pixel.
    RWStructuredBuffer<uint> output;
};

[[vk::binding(0, 2)]] ParameterBlock<GiDebugSet> giDebug;

// --- Packing: `to_byte`, `pack_colour`, `pack_radiance`, `hue_of`, `ramp` --------------------------

static const float3 kGdGreen = float3(0.15f, 0.85f, 0.25f);
static const float3 kGdRed = float3(0.95f, 0.15f, 0.1f);
static const float3 kGdMagenta = float3(1.0f, 0.0f, 1.0f);
static const float3 kGdDark = float3(0.08f, 0.08f, 0.08f);

uint gdByte(float value) {
    // `std::lround` of a value in [0, 255]: half away from zero, which floor(x + 0.5) is there.
    return uint(floor((gpClamp(value, 0.0f, 1.0f) * 255.0f) + 0.5f));
}

uint gdPackColour(float3 colour) {
    return gdByte(colour.x) | (gdByte(colour.y) << 8u) | (gdByte(colour.z) << 16u) | (255u << 24u);
}

uint gdPackRadiance(float3 radiance, float exposure) {
    const float3 lit = radiance * exposure;
    const float3 positive = float3(max(lit.x, 0.0f), max(lit.y, 0.0f), max(lit.z, 0.0f));
    return gdPackColour(float3(positive.x / (1.0f + positive.x), positive.y / (1.0f + positive.y),
                               positive.z / (1.0f + positive.z)));
}

float3 gdHue(uint index) {
    const float walk = float(index) * 0.618034f;
    const float hue = (walk - floor(walk)) * 6.0f;
    const float fraction = hue - floor(hue);
    const float value = 0.9f;
    const float low = value * (1.0f - 0.65f);
    const float falling = value * (1.0f - (0.65f * fraction));
    const float rising = value * (1.0f - (0.65f * (1.0f - fraction)));
    switch (uint(hue) % 6u) {
        case 0u:
            return float3(value, rising, low);
        case 1u:
            return float3(falling, value, low);
        case 2u:
            return float3(low, value, rising);
        case 3u:
            return float3(low, falling, value);
        case 4u:
            return float3(rising, low, value);
        default:
            return float3(value, low, falling);
    }
}

float3 gdLerp3(float3 a, float3 b, float t) {
    return a + ((b - a) * t);
}

float3 gdRamp(float t) {
    return gdLerp3(kGdGreen, kGdRed, gpClamp(t, 0.0f, 1.0f));
}

/// `tier_colour`.
float3 gdTierColour(uint source) {
    if (source == kGpSourceScreen) {
        return float3(0.15f, 0.4f, 1.0f);
    }
    if (source == kGpSourceSoftware) {
        return kGdGreen;
    }
    if (source == kGpSourceHardware) {
        return float3(1.0f, 0.55f, 0.05f);
    }
    if (source == kGpSourceSky) {
        return float3(1.0f, 1.0f, 1.0f);
    }
    return float3(0.0f, 0.0f, 0.0f);
}

float gdLuminance(float3 value) {
    // `emission_luminance`.
    return (0.2126f * value.x) + (0.7152f * value.y) + (0.0722f * value.z);
}

// --- The frame's pixel ---------------------------------------------------------------------------

struct GdPixel {
    float3 position;
    float depth;
    float3 normal;
    float roughness;
    uint cell;
};

/// The surface the resolve unpacked for the grid cell `x, y` falls in.
GdPixel gdPixel(uint x, uint y) {
    const uint4 counts = giDebug.resolveConstants[kGdrCounts];
    const uint divisor = max(giDebug.words[kGdView].w, 1u);
    const uint cx = min(x / divisor, counts.x - 1u);
    const uint cy = min(y / divisor, counts.y - 1u);
    GdPixel pixel;
    pixel.cell = (cy * counts.x) + cx;
    const uint record = ((counts.w * counts.z) + pixel.cell) * 2u;
    const float4 a = giDebug.surfaces[record];
    const float4 b = giDebug.surfaces[record + 1u];
    pixel.position = a.xyz;
    pixel.depth = a.w;
    pixel.normal = b.xyz;
    pixel.roughness = b.w;
    return pixel;
}

float3 gdCamera() {
    return asfloat(giDebug.words[kGdCamera].xyz);
}

float gdExposure() {
    return giDebug.exposure[0];
}

// --- The probe discs: `project`, `make_disc`, `splat_disc`, `draw_discs` -------------------------

struct GdDisc {
    float x;
    float y;
    float radius;
    float distance;
    bool drawn;
};

float4 gdClip(float3 point) {
    const float4 c0 = asfloat(giDebug.resolveConstants[kGdrWorldToClip]);
    const float4 c1 = asfloat(giDebug.resolveConstants[kGdrWorldToClip + 1u]);
    const float4 c2 = asfloat(giDebug.resolveConstants[kGdrWorldToClip + 2u]);
    const float4 c3 = asfloat(giDebug.resolveConstants[kGdrWorldToClip + 3u]);
    return (c0 * point.x) + (c1 * point.y) + (c2 * point.z) + c3;
}

/// `project`: x, y in pixels and w; w zero where the point is at or behind the eye.
float3 gdProject(float3 point) {
    const float4 clip = gdClip(point);
    if (clip.w <= 1.0e-6f) {
        return float3(0.0f, 0.0f, clip.w);
    }
    const uint4 view = giDebug.words[kGdView];
    return float3((((clip.x / clip.w) * 0.5f) + 0.5f) * float(view.y),
                  (0.5f - ((clip.y / clip.w) * 0.5f)) * float(view.z), clip.w);
}

/// `make_disc`, for the probe at `position`.
GdDisc gdMakeDisc(float3 position) {
    GdDisc disc;
    disc.drawn = false;
    disc.x = 0.0f;
    disc.y = 0.0f;
    disc.radius = 0.0f;
    disc.distance = 0.0f;
    const float metres = asfloat(giDebug.words[kGdCamera].w);
    const float3 centre = gdProject(position);
    if (centre.z <= 4.0f * metres) {
        return disc;
    }
    const float3 camera = gdCamera();
    const float3 view = giNormalisedOr(position - camera, float3(0.0f, 0.0f, -1.0f));
    float3 side = cross(view, float3(0.0f, 1.0f, 0.0f));
    if (dot(side, side) < 1.0e-6f) {
        side = cross(view, float3(1.0f, 0.0f, 0.0f));
    }
    const float3 edge = gdProject(position + (side * (1.0f / sqrt(dot(side, side))) * metres));
    const uint4 frame = giDebug.words[kGdView];
    disc.x = centre.x;
    disc.y = centre.y;
    const float dx = edge.x - centre.x;
    const float dy = edge.y - centre.y;
    disc.radius = gpClamp(sqrt((dx * dx) + (dy * dy)), 1.0f, float(frame.z) * 0.125f);
    if (disc.x + disc.radius < 0.0f || disc.y + disc.radius < 0.0f ||
        disc.x - disc.radius > float(frame.y) || disc.y - disc.radius > float(frame.z)) {
        return disc;
    }
    disc.distance = length(position - camera);
    disc.drawn = true;
    return disc;
}

/// Whether `disc` covers pixel `x, y` in front of the surface there (`splat_disc`'s test), and how
/// far from its centre.
bool gdCovers(GdDisc disc, uint x, uint y, GdPixel pixel, out float fromCentre) {
    const float dx = (float(x) + 0.5f) - disc.x;
    const float dy = (float(y) + 0.5f) - disc.y;
    fromCentre = sqrt((dx * dx) + (dy * dy));
    const bool hidden = pixel.depth > 0.0f && length(pixel.position - gdCamera()) < disc.distance;
    return fromCentre <= disc.radius && !hidden;
}

/// `draw_discs`' painter's order, for one pixel: the discs are splatted far to near, ties by x
/// then y ascending, so the one the pixel shows is the last of that order that covers it.
bool gdLaterInOrder(GdDisc candidate, GdDisc best) {
    if (candidate.distance != best.distance) {
        return candidate.distance < best.distance;
    }
    if (candidate.x != best.x) {
        return candidate.x > best.x;
    }
    return candidate.y >= best.y;
}

/// A probe record: [position, level], [last update frame bits, valid, live, error].
float4 gdProbeA(uint probe) {
    return giProbes.probes[probe * 4u];
}

float4 gdProbeB(uint probe) {
    return giProbes.probes[(probe * 4u) + 1u];
}

/// `probe_mean`: the payload decoded along the six axes, averaged.
float3 gdProbeMean(uint probe) {
    const uint4 counts = giProbes.constants[kGpCounts];
    float data[kGpMaxPayload];
    gpLoadPayload(probe, counts.y, data);
    float3 total = float3(0.0f, 0.0f, 0.0f);
    for (uint axis = 0u; axis < 6u; ++axis) {
        total = total + gpDecode(counts.x, data, kGpAxes[axis]);
    }
    return total / 6.0f;
}

/// `probe_colour`.
float3 gdProbeColour(uint view, uint probe) {
    const float4 record = gdProbeB(probe);
    const bool valid = record.y != 0.0f;
    if (view == kGdProbeIrradiance) {
        if (!valid) {
            return kGdMagenta;
        }
        const float3 mean = gdProbeMean(probe) * gdExposure();
        return float3(mean.x / (1.0f + mean.x), mean.y / (1.0f + mean.y), mean.z / (1.0f + mean.z));
    }
    if (view == kGdProbeAge) {
        const uint4 frame = giDebug.words[kGdFrame];
        const uint last = asuint(record.x);
        const uint age = frame.y > last ? frame.y - last : 0u;
        return valid ? gdRamp(float(age) / float(max(frame.z, 1u))) : kGdMagenta;
    }
    if (view == kGdProbeError) {
        return valid ? gdRamp(record.w / 0.25f) : kGdMagenta;
    }
    return valid ? kGdGreen : kGdRed;
}

/// The disc pixel `x, y` shows among the live probes (`suspectsOnly`: those marked leak
/// suspects), in the colour `view` gives it; false where none covers it.
bool gdDiscTexel(uint view, uint x, uint y, GdPixel pixel, bool suspectsOnly, out uint texel) {
    texel = 0u;
    const uint slots = giDebug.words[kGdFrame].w;
    GdDisc best;
    best.drawn = false;
    best.x = 0.0f;
    best.y = 0.0f;
    best.radius = 0.0f;
    best.distance = 0.0f;
    uint bestProbe = 0u;
    float bestFromCentre = 0.0f;
    for (uint probe = 0u; probe < slots; ++probe) {
        if (gdProbeB(probe).z == 0.0f) {
            continue;  // not live
        }
        if (suspectsOnly && giDebug.suspects[probe] == 0u) {
            continue;
        }
        const GdDisc disc = gdMakeDisc(gdProbeA(probe).xyz);
        float fromCentre = 0.0f;
        if (!disc.drawn || !gdCovers(disc, x, y, pixel, fromCentre)) {
            continue;
        }
        if (!best.drawn || gdLaterInOrder(disc, best)) {
            best = disc;
            bestProbe = probe;
            bestFromCentre = fromCentre;
        }
    }
    if (!best.drawn) {
        return false;
    }
    const float3 colour = suspectsOnly ? kGdRed : gdProbeColour(view, bestProbe);
    texel = gdPackColour(colour * (bestFromCentre > best.radius * 0.75f ? 0.6f : 1.0f));
    return true;
}

// --- The surface views ---------------------------------------------------------------------------

/// `IlluminationSystem::probe_blocked`: the field's march from the probe stops two voxels short.
bool gdProbeBlocked(float3 probe, float3 position) {
    const float clearance = 2.0f * giFinestVoxel();
    const float3 offset = position - probe;
    const float span = length(offset);
    if (span <= clearance) {
        return false;
    }
    return giSphereTrace(probe, offset / span, span - clearance, 0.0f).hit;
}

/// `visibility_texel` over `RadianceCache::contributions`: every probe whose weight is positive,
/// as `gpSample` visits them, and the leaked share of their weight. The suspects are marked for
/// the disc pass.
uint gdVisibilityTexel(GdPixel pixel) {
    const uint levels = asuint(giProbes.constants[kGpGather].z);
    const float base = asfloat(giProbes.constants[kGpLayout].x);
    float total = 0.0f;
    float leaked = 0.0f;
    uint contributing = 0u;
    for (uint level = 0u; level < levels; ++level) {
        const float spacing = base * float(1u << level);
        const float radius = spacing * 1.5f;
        const int3 lo = int3(gpFloorDiv(pixel.position.x - radius, spacing),
                             gpFloorDiv(pixel.position.y - radius, spacing),
                             gpFloorDiv(pixel.position.z - radius, spacing));
        const int3 hi = int3(gpFloorDiv(pixel.position.x + radius, spacing),
                             gpFloorDiv(pixel.position.y + radius, spacing),
                             gpFloorDiv(pixel.position.z + radius, spacing));
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int y = lo.y; y <= hi.y; ++y) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    const uint probe = gpLayoutProbe(level, int3(x, y, z));
                    if (probe == kGpNoProbe) {
                        continue;
                    }
                    const float4 record = gdProbeB(probe);
                    if (record.y == 0.0f || record.z == 0.0f) {
                        continue;
                    }
                    const float3 at = gdProbeA(probe).xyz;
                    const float3 offset = pixel.position - at;
                    const float span = length(offset);
                    if (span > radius) {
                        continue;
                    }
                    float weight = 1.0f - gpClamp(span / radius, 0.0f, 1.0f);
                    if (span > 1.0e-4f) {
                        const float facing = dot(pixel.normal, -offset / span);
                        weight *= gpClamp((facing + 1.0f) * 0.5f, 0.0f, 1.0f);
                    }
                    weight *= gpVisibilityWeight(probe, at, pixel.position);
                    if (weight <= 0.0f) {
                        continue;
                    }
                    contributing += 1u;
                    total += weight;
                    if (gdProbeBlocked(at, pixel.position)) {
                        leaked += weight;
                        giDebug.suspects[probe] = 1u;
                    }
                }
            }
        }
    }
    if (contributing == 0u) {
        return gdPackColour(kGdDark);
    }
    const float share = total > 0.0f ? leaked / total : 0.0f;
    return gdPackColour(gdLerp3(kGdGreen, kGdRed, share) * 0.8f);
}

/// `clipmap_colour`.
float3 gdClipmapColour(float3 position) {
    const float4 cache = asfloat(giDebug.words[kGdCache]);
    const uint levels = giDebug.words[kGdCache].w;
    const float base = asfloat(giDebug.words[kGdLayout].x);
    const uint halfExtent = giDebug.words[kGdLayout].y;
    const uint slots = giDebug.words[kGdFrame].w;
    for (uint level = 0u; level < levels; ++level) {
        const float spacing = base * float(1u << level);
        const float half = spacing * float(halfExtent);
        const float3 offset = position - cache.xyz;
        if (abs(offset.x) > half || abs(offset.y) > half || abs(offset.z) > half) {
            continue;
        }
        bool resident = false;
        for (uint probe = 0u; probe < slots && !resident; ++probe) {
            const float4 record = gdProbeB(probe);
            const float4 placed = gdProbeA(probe);
            resident = record.z != 0.0f && record.y != 0.0f && uint(placed.w) == level &&
                       length(placed.xyz - position) <= spacing * 1.5f;
        }
        return gdHue(level + 1u) * (resident ? 1.0f : 0.3f);
    }
    return kGdDark;
}

/// `field_slice_colour`; false where the slice does not lie between the eye and the surface.
bool gdFieldSlice(GdPixel pixel, out float3 colour) {
    colour = kGdDark;
    const float3 camera = gdCamera();
    const float3 toward = pixel.position - camera;
    const float surface = length(toward);
    if (surface <= 1.0e-6f) {
        return false;
    }
    const float3 direction = toward / surface;
    if (abs(direction.y) < 1.0e-6f) {
        return false;
    }
    const float height = asfloat(giDebug.words[kGdField].x);
    const float along = (height - camera.y) / direction.y;
    if (along <= 0.0f || along >= surface) {
        return false;
    }
    const float distance = giDistance(camera + (direction * along));
    const float3 base = distance >= 0.0f ? float3(0.2f, 0.45f, 1.0f) : float3(1.0f, 0.5f, 0.1f);
    const float band = abs(distance) / 0.25f;
    const float fraction = band - floor(band);
    const float shade =
        fraction < 0.12f ? 0.35f : 1.0f - (0.5f * gpClamp(band / 8.0f, 0.0f, 1.0f));
    colour = base * shade;
    return true;
}

/// `direct_radiance` for promoted emitter `light` of this file's list.
float3 gdEmitterRadiance(uint light, float3 position, float3 normal) {
    const float4 a = giDebug.emitters[(light * 4u) + 0u];
    const float4 b = giDebug.emitters[(light * 4u) + 1u];
    const float4 c = giDebug.emitters[(light * 4u) + 2u];
    const float4 d = giDebug.emitters[(light * 4u) + 3u];
    if (c.w != 0.0f) {
        const float3 toLight = -giNormalisedOr(b.xyz, float3(0.0f, 1.0f, 0.0f));
        return c.xyz * (a.w * max(0.0f, dot(normal, toLight)));
    }
    const float3 offset = a.xyz - position;
    const float distanceSquared = max(dot(offset, offset), 1.0e-6f);
    if (b.w > 0.0f && distanceSquared > b.w * b.w) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    if (d.w > 0.0f) {
        return c.xyz * (a.w * giEmitterFormFactor(a.xyz, d.xyz, d.w, position, normal));
    }
    const float3 toLight = offset / sqrt(distanceSquared);
    return c.xyz * (a.w * max(0.0f, dot(normal, toLight)) / distanceSquared);
}

/// `shadow_target`.
float3 gdShadowTarget(uint light, float3 position) {
    const float4 a = giDebug.emitters[(light * 4u) + 0u];
    const float4 b = giDebug.emitters[(light * 4u) + 1u];
    const float4 c = giDebug.emitters[(light * 4u) + 2u];
    const float4 d = giDebug.emitters[(light * 4u) + 3u];
    if (c.w != 0.0f) {
        return position - (giNormalisedOr(b.xyz, float3(0.0f, 1.0f, 0.0f)) * 1000.0f);
    }
    return d.w > 0.0f ? a.xyz + (d.xyz * kGdEmitterShadowLift) : a.xyz;
}

/// `emissive_radiance`: the surface's own emission — the card the lookup resolves, which is what
/// the device knows of a surface's material — and every promoted emitter's light that reaches it.
float3 gdEmissive(GdPixel pixel) {
    float3 total = float3(0.0f, 0.0f, 0.0f);
    const uint card = giFindCard(pixel.position, pixel.normal);
    if (card != 0xFFFFFFFFu) {
        total = giScene.cards[(card * 4u) + 3u].xyz;
    }
    const float3 origin = pixel.position + (pixel.normal * 1.0e-2f);
    const uint count = giDebug.words[kGdField].y;
    for (uint light = 0u; light < count; ++light) {
        const float3 arriving = gdEmitterRadiance(light, pixel.position, pixel.normal);
        if (dot(arriving, arriving) <= 0.0f ||
            giFieldOccluded(origin, gdShadowTarget(light, pixel.position))) {
            continue;
        }
        total = total + arriving;
    }
    return total;
}

/// `grReflectionRays`: `reflection_ray_count` over the resolve's thresholds.
uint gdReflectionRays(float roughness) {
    const uint4 words = giDebug.resolveConstants[kGdrReflect];
    const float mirror = asfloat(words.x);
    const float cacheMin = asfloat(words.y);
    const uint maxRays = max(1u, words.z);
    if (roughness <= mirror) {
        return maxRays;
    }
    if (roughness >= cacheMin) {
        return 0u;
    }
    const float span = max(cacheMin - mirror, 1.0e-4f);
    const float t = gpClamp((roughness - mirror) / span, 0.0f, 1.0f);
    const float rays = float(maxRays) + ((1.0f - float(maxRays)) * t);
    return max(1u, uint(floor(rays + 0.5f)));
}

/// One pixel of a per-pixel view through world tier `T`; false where the view draws nothing.
bool gdSurfaceTexel<T : IGiWorldTier>(uint view, uint x, uint y, GdPixel pixel, out uint texel) {
    texel = 0u;
    const float exposure = gdExposure();
    const float3 camera = gdCamera();
    const float3 viewDirection = giNormalisedOr(pixel.position - camera, float3(0.0f, 0.0f, -1.0f));
    const float3 reflection =
        viewDirection - (pixel.normal * (2.0f * dot(viewDirection, pixel.normal)));
    const float3 origin = pixel.position + (pixel.normal * 1.0e-3f);
    switch (view) {
        case kGdProbeVisibility:
            texel = gdVisibilityTexel(pixel);
            return true;
        case kGdRadianceClipmap:
            texel = gdPackColour(gdClipmapColour(pixel.position));
            return true;
        case kGdSurfaceCache: {
            const uint card = giFindCard(pixel.position, pixel.normal);
            texel = card != 0xFFFFFFFFu ? gdPackRadiance(giScene.cardState[card * 2u].xyz, exposure)
                                        : gdPackColour(kGdMagenta);
            return true;
        }
        case kGdDistanceField: {
            float3 colour;
            const bool drawn = gdFieldSlice(pixel, colour);
            texel = drawn ? gdPackColour(colour) : 0u;
            return drawn;
        }
        case kGdCards: {
            const uint card = giFindCard(pixel.position, pixel.normal);
            texel = card == 0xFFFFFFFFu ? gdPackColour(kGdDark) : gdPackColour(gdHue(card));
            return true;
        }
        case kGdReflectionProbes:
            // The device draws this view only where the set is empty (`GpuGiDebugView::draws`).
            texel = gdPackColour(kGdDark);
            return true;
        case kGdScreenTrace:
            // `screen_trace_texel` with no screen view: the device frame's rays have no screen tier.
            texel = gdPackColour(kGdRed);
            return true;
        case kGdConfidence: {
            const uint frame = giDebug.words[kGdFrame].y;
            const float4 cached = gpSample(pixel.position, pixel.normal, frame);
            texel = gdPackColour(gdRamp(1.0f - cached.w));
            return true;
        }
        case kGdTier: {
            const float reach = asfloat(giDebug.resolveConstants[kGdrRays].x);
            const GiTierSample answer = gpWorldTrace<T>(origin, reflection, reach);
            texel = gdPackColour(gdTierColour(answer.confidence > 0.0f ? answer.source
                                                                       : kGpSourceNone));
            return true;
        }
        case kGdRayCount: {
            const float rays = float(gdReflectionRays(pixel.roughness)) / 16.0f;
            texel = gdPackColour(T.source() == kGpSourceHardware ? float3(rays, 0.0f, 0.0f)
                                                                 : float3(0.0f, rays, 0.0f));
            return true;
        }
        case kGdDenoiserConfidence: {
            const int source = asint(giDebug.sources[pixel.cell * 2u]);
            const float trust = asfloat(giDebug.sources[(pixel.cell * 2u) + 1u]);
            texel = gdPackColour(gdRamp(1.0f - (source >= 0 ? trust : 0.0f)));
            return true;
        }
        case kGdDisocclusion: {
            const int source = asint(giDebug.sources[pixel.cell * 2u]);
            texel = gdPackColour(source >= 0 ? kGdDark : kGdMagenta);
            return true;
        }
        case kGdIndirectDiffuse:
            texel = gdPackRadiance(giDebug.diffuse.Load(int3(int(x), int(y), 0)).xyz, exposure);
            return true;
        case kGdIndirectSpecular:
            texel = gdPackRadiance(giDebug.specular.Load(int3(int(x), int(y), 0)).xyz, exposure);
            return true;
        case kGdEmissive:
            texel = gdPackRadiance(gdEmissive(pixel), exposure);
            return true;
        default:
            return false;
    }
}

/// Every view, one frame pixel a thread.
void gdDrawPixel<T : IGiWorldTier>(uint3 thread) {
    const uint4 view = giDebug.words[kGdView];
    if (thread.x >= view.y || thread.y >= view.z) {
        return;
    }
    const uint index = (thread.y * view.y) + thread.x;
    const GdPixel pixel = gdPixel(thread.x, thread.y);
    uint texel = 0u;
    if (view.x <= kGdProbeError) {
        // A disc view draws over the sky as well; a surface view draws nothing there.
        const bool covered = gdDiscTexel(view.x, thread.x, thread.y, pixel, false, texel);
        texel = covered ? texel : 0u;
    } else if (pixel.depth > 0.0f) {
        const bool drawn = gdSurfaceTexel<T>(view.x, thread.x, thread.y, pixel, texel);
        texel = drawn ? texel : 0u;
    }
    giDebug.output[index] = texel;
}

// --- The exposure, and the leak suspects' discs --------------------------------------------------

static const uint kGdExposureLanes = 256u;
groupshared float gdLuminanceSum[kGdExposureLanes];
groupshared uint gdSurfaceCount[kGdExposureLanes];

/// `automatic_exposure` over the frame's own indirect diffuse: the mean over every eighth surface
/// pixel drawn at a third of white. One workgroup, summed in lane order then folded in a fixed
/// tree, so the exposure is the same from run to run. It also clears the leak suspects.
void gdExposurePass(uint lane) {
    const uint4 view = giDebug.words[kGdView];
    const uint slots = giDebug.words[kGdFrame].w;
    for (uint probe = lane; probe < slots; probe += kGdExposureLanes) {
        giDebug.suspects[probe] = 0u;
    }
    const uint pixels = view.y * view.z;
    float sum = 0.0f;
    uint count = 0u;
    for (uint index = lane * 8u; index < pixels; index += kGdExposureLanes * 8u) {
        const uint x = index % view.y;
        const uint y = index / view.y;
        const GdPixel pixel = gdPixel(x, y);
        if (pixel.depth <= 0.0f) {
            continue;
        }
        sum += gdLuminance(giDebug.diffuse.Load(int3(int(x), int(y), 0)).xyz);
        count += 1u;
    }
    gdLuminanceSum[lane] = sum;
    gdSurfaceCount[lane] = count;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = kGdExposureLanes / 2u; stride > 0u; stride /= 2u) {
        if (lane < stride) {
            gdLuminanceSum[lane] += gdLuminanceSum[lane + stride];
            gdSurfaceCount[lane] += gdSurfaceCount[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u) {
        const float authored = asfloat(giDebug.words[kGdFrame].x);
        const float mean = gdSurfaceCount[0] == 0u ? 0.0f
                                                   : gdLuminanceSum[0] / float(gdSurfaceCount[0]);
        giDebug.exposure[0] = authored > 0.0f ? authored : (mean > 1.0e-12f ? 0.5f / mean : 1.0f);
    }
}

/// `draw_visibility_view`'s second half: the suspects as red discs, over the surface pass.
void gdSuspectPass(uint3 thread) {
    const uint4 view = giDebug.words[kGdView];
    if (view.x != kGdProbeVisibility || thread.x >= view.y || thread.y >= view.z) {
        return;
    }
    const GdPixel pixel = gdPixel(thread.x, thread.y);
    uint texel = 0u;
    if (gdDiscTexel(view.x, thread.x, thread.y, pixel, true, texel)) {
        giDebug.output[(thread.y * view.y) + thread.x] = texel;
    }
}
