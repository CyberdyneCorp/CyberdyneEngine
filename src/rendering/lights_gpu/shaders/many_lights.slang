// SPDX-License-Identifier: MIT
// The stochastic many-light path: `rendering-lighting-and-shadows`' "Stochastic many-light direct
// lighting", on the device.
//
//   candidates  per pixel, `M` lights drawn in proportion to their power from the frame's local
//               lights (the host's alias table) and resampled towards each one's unshadowed
//               irradiance here — `reservoir_update`. Then TEMPORAL reuse: last frame's final
//               reservoir at the pixel this surface was, if it was this surface, re-weighted by its
//               light's irradiance HERE and combined, its count capped — `reservoir_combine`.
//   spatial     a few neighbours' reservoirs on the same surface, combined the same way; the
//               selected light's contribution weight `W = weight_sum / (M p(y))`; ONE visibility
//               ray to a point drawn over the light's own surface, marched through the depth
//               buffer; and the estimate `E_y W V`, which is the noisy signal.
//   accumulate  `denoise::Denoiser::accumulate` over the reprojected history the candidates pass
//   variance    found; `estimate_variance`; and `filter`'s a-trous cascade, a pass a step, the last
//   filter      writing the stage's target. `default_config(StochasticDirect)` and the quality
//               ladder's position arrive as words, so the device runs the host's configuration.
//
// THE HOST IS THE REFERENCE FOR EVERY STEP: `many_light.h`'s reservoirs, `area_shading.h`'s
// irradiance and light sampling (transcribed into `cy/light.slang`), and `denoiser.cpp`.
// `render.many_lights` holds the device's mean to a sum over every light, and its denoised signal
// to `Denoiser::denoise` run over the device's own noisy signal and history.
//
// Compiled by shaders/regenerate.py (SPIR-V 1.5 and MSL) into the checked-in headers.

import cy.light;
import cy.packing;

static const uint kGroupSize = 8u;

struct CyManyLightSet
{
    /// `lights_gpu::ManyLightConstants`, word for word. Read as UINT and turned into floats where
    /// a word is one: several words are integers, and an integer's bits read as a float are a
    /// denormal a device may flush to zero on the way through a float register.
    StructuredBuffer<uint4> words;
    /// The frame's lights: `GpuLight`, which is `cy.light`'s `Light`.
    StructuredBuffer<Light> lights;
    /// The alias table over the frame's local lights: (light, alias, threshold bits, pdf bits).
    StructuredBuffer<uint4> candidates;
    DepthTexture2D depth;
    Texture2D<float4> normals;
    /// This frame's reservoir after temporal reuse: (light, weight sum, count, target).
    [format("rgba32f")] RWTexture2D<float4> temporalReservoir;
    /// Last frame's final reservoirs, and this frame's.
    [format("rgba32f")] RWTexture2D<float4> historyReservoir;
    [format("rgba32f")] RWTexture2D<float4> finalReservoir;
    /// Last frame's surface and this frame's: (view depth, normal).
    [format("rgba32f")] RWTexture2D<float4> historySurface;
    [format("rgba32f")] RWTexture2D<float4> surface;
    /// The reprojection: (source pixel index or -1, confidence).
    [format("rg32f")] RWTexture2D<float2> guidance;
    /// The noisy estimate, `E_y W V`.
    [format("rgba32f")] RWTexture2D<float4> noisy;
    /// The resolved reservoir the forward pass shades the specular half with: (light, W V).
    [format("rgba32f")] RWTexture2D<float4> resolved;
    /// The denoiser's history: (colour) and (first moment, second moment, samples).
    [format("rgba32f")] RWTexture2D<float4> historyColour;
    [format("rgba32f")] RWTexture2D<float4> historyMoments;
    [format("rgba32f")] RWTexture2D<float4> accumulated;
    [format("rgba32f")] RWTexture2D<float4> moments;
    [format("r32f")] RWTexture2D<float> variance;
    [format("rgba32f")] RWTexture2D<float4> filterSource;
    [format("rgba32f")] RWTexture2D<float4> filterOut;
    /// The stage's target: the denoised direct irradiance the forward pass samples.
    [format("rgba16f")] RWTexture2D<float4> target;
};

[[vk::binding(0, 0)]] ParameterBlock<CyManyLightSet> cyManyLights;

struct CyManyLightPush
{
    uint pass;      // the a-trous pass, 0 first
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

[[vk::push_constant]] ConstantBuffer<CyManyLightPush> cyManyLightPush;

// --- The words -----------------------------------------------------------------------------------
//
//   0..3   clip to camera-relative, by column (this frame's jittered projection and rotation)
//   4..7   camera-relative to LAST frame's clip, by row: `FrameViewData::previous_relative_to_clip`
//   8      extent: width, height, 1/width, 1/height
//   9      frame index, candidates a pixel, local lights, spatial neighbours (as uint bits)
//   10     history cap, spatial radius in pixels, history valid (0/1), visibility steps
//   11     camera-relative to view space, third row (z of view space)
//   12     denoiser: sigma depth, sigma normal, sigma value, sigma roughness
//   13     denoiser: history length, passes, kernel extent, alpha floor
//   14     visibility: thickness in metres, start offset in metres, 0, 0
//   15     flags: temporal reuse, spatial reuse, visibility traced, light records (as uint bits)
//   16..19 camera-relative to this frame's clip, by row: what a visibility ray is marched through

float4 word(uint index)
{
    return asfloat(cyManyLights.words[index]);
}

uint wordBits(uint index, uint lane)
{
    return cyManyLights.words[index][lane];
}

// --- Randomness ----------------------------------------------------------------------------------
//
// COUNTER-BASED: draw `n` of a pixel's stream is a strong integer hash (`lowbias32`) of a Weyl
// sequence started from a hash of (x, y, frame, pass), so every draw is decorrelated from every
// other — across a pixel's draws, across neighbouring pixels and across frames — and a pixel's
// sequence is a pure function of where and when it is. Not the host's xorshift (`SampleStream`),
// and not an LCG with a permuted output: with either, the alias table's two draws and the
// reservoir's acceptance were correlated enough to draw a weak light about two per cent less often
// than its probability, which `render.many_lights` measured as a bias.

struct Stream
{
    uint state;
};

uint lowbias32(uint value)
{
    var x = value;
    x ^= x >> 16u;
    x *= 0x7FEB352Du;
    x ^= x >> 15u;
    x *= 0x846CA68Bu;
    x ^= x >> 16u;
    return x;
}

Stream streamFor(uint2 pixel, uint frame, uint salt)
{
    Stream stream;
    stream.state = lowbias32(pixel.x + lowbias32(pixel.y + lowbias32(frame + lowbias32(salt))));
    return stream;
}

float nextUnit(inout Stream stream)
{
    stream.state += 0x9E3779B9u;
    return float(lowbias32(stream.state) >> 8u) * (1.0 / 16777216.0);
}

// --- Surfaces ------------------------------------------------------------------------------------

struct CySurface
{
    bool valid;
    float3 position;  // camera-relative
    float3 normal;
    float viewDepth;
};

float3 relativeFromClip(float2 ndc, float depth)
{
    let clip = float4(ndc, depth, 1.0);
    let h = word(0) * clip.x + word(1) * clip.y + word(2) * clip.z + word(3) * clip.w;
    return h.xyz / h.w;
}

CySurface surfaceAt(uint2 pixel)
{
    CySurface surface;
    let depth = cyManyLights.depth.Load(int3(int2(pixel), 0)).r;
    surface.valid = depth > 0.0;
    let extent = word(8);
    let uv = (float2(pixel) + 0.5) * extent.zw;
    let ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    surface.position = surface.valid ? relativeFromClip(ndc, depth) : float3(0.0);
    let encoded = cyManyLights.normals.Load(int3(int2(pixel), 0)).xy;
    surface.normal = decodeOctahedral(encoded);
    surface.viewDepth = surface.valid ? -dot(word(11).xyz, surface.position) : 0.0;
    return surface;
}

float luminance(float3 value)
{
    return 0.2126 * value.x + 0.7152 * value.y + 0.0722 * value.z;
}

/// The target function: the unshadowed irradiance's luminance. A light index past the frame's
/// records — a history from a frame with more lights — targets nothing.
float targetOf(uint light, CySurface surface)
{
    if (light >= wordBits(15, 3))
    {
        return 0.0;
    }
    return luminance(lightIrradiance(cyManyLights.lights[light], surface.position, surface.normal));
}

// --- Reservoirs: `many_light.h`, transcribed ------------------------------------------------------
//
// Stored as (light, weight sum, sample count, target), the light as a float — exact below 2^24.
// -1 is `kNoLight`.

struct Reservoir
{
    float light;
    float weightSum;
    float count;
    float target;
};

Reservoir emptyReservoir()
{
    Reservoir reservoir;
    reservoir.light = -1.0;
    reservoir.weightSum = 0.0;
    reservoir.count = 0.0;
    reservoir.target = 0.0;
    return reservoir;
}

Reservoir toReservoir(float4 packed)
{
    Reservoir reservoir;
    reservoir.light = packed.x;
    reservoir.weightSum = packed.y;
    reservoir.count = packed.z;
    reservoir.target = packed.w;
    return reservoir;
}

float4 packReservoir(Reservoir reservoir)
{
    return float4(reservoir.light, reservoir.weightSum, reservoir.count, reservoir.target);
}

bool reservoirValid(Reservoir reservoir)
{
    return reservoir.light >= 0.0 && reservoir.target > 0.0;
}

/// `Reservoir::contribution_weight`.
float contributionWeight(Reservoir reservoir)
{
    if (!reservoirValid(reservoir) || reservoir.count <= 0.0)
    {
        return 0.0;
    }
    return reservoir.weightSum / (reservoir.count * reservoir.target);
}

/// `reservoir_update`.
void reservoirUpdate(inout Reservoir reservoir, float light, float target, float sourcePdf,
                     inout Stream stream)
{
    reservoir.count += 1.0;
    if (!(target > 0.0) || !(sourcePdf > 0.0))
    {
        return;
    }
    let weight = target / sourcePdf;
    reservoir.weightSum += weight;
    if (nextUnit(stream) * reservoir.weightSum <= weight)
    {
        reservoir.light = light;
        reservoir.target = target;
    }
}

/// `cap_reservoir`: at most `cap` samples, the weight sum scaled with the count so the contribution
/// weight — their ratio — is unchanged.
void capReservoir(inout Reservoir reservoir, float cap)
{
    if (cap > 0.0 && reservoir.count > cap)
    {
        reservoir.weightSum *= cap / reservoir.count;
        reservoir.count = cap;
    }
}

/// `reservoir_combine`: `other`'s light re-weighted by its target HERE.
void reservoirCombine(inout Reservoir reservoir, Reservoir other, float otherTargetHere,
                      float cap, inout Stream stream)
{
    if (!reservoirValid(other) || other.count <= 0.0)
    {
        return;
    }
    let weight = contributionWeight(other) * otherTargetHere * other.count;
    if (weight > 0.0)
    {
        reservoir.weightSum += weight;
        if (nextUnit(stream) * reservoir.weightSum <= weight)
        {
            reservoir.light = other.light;
            reservoir.target = otherTargetHere;
        }
    }
    reservoir.count += other.count;
    capReservoir(reservoir, cap);
}

/// One draw from the alias table: the light and the probability it was drawn with.
void drawCandidate(inout Stream stream, uint count, out uint light, out float pdf)
{
    let slot = min(uint(nextUnit(stream) * float(count)), count - 1u);
    let entry = cyManyLights.candidates[slot];
    let chosen = nextUnit(stream) < asfloat(entry.z) ? slot : entry.y;
    let picked = cyManyLights.candidates[chosen];
    light = picked.x;
    pdf = asfloat(picked.w);
}

// --- The reprojection ----------------------------------------------------------------------------
//
// Where this surface was last frame: through last frame's clip about this frame's camera, to the
// pixel it fell in, kept only if the surface stored there is this one — depth within ten per cent,
// normals within about twenty-five degrees. `temporal-rendering`'s classification, in its simplest
// form, and the same answer serves the reservoirs and the denoiser's history.

int historySource(CySurface surface, uint2 extent, out float confidence)
{
    confidence = 0.0;
    if (!surface.valid || word(10).z < 0.5)
    {
        return -1;
    }
    let point = float4(surface.position, 1.0);
    let clip = float4(dot(word(4), point), dot(word(5), point), dot(word(6), point), dot(word(7), point));
    if (clip.w <= 0.0)
    {
        return -1;
    }
    let ndc = clip.xy / clip.w;
    let uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0)
    {
        return -1;
    }
    let previous = uint2(uv * float2(extent));
    let stored = cyManyLights.historySurface.Load(int2(previous));
    if (stored.x <= 0.0 || abs(stored.x - surface.viewDepth) > 0.1 * surface.viewDepth ||
        dot(stored.yzw, surface.normal) < 0.9)
    {
        return -1;
    }
    confidence = 1.0;
    return int(previous.y * extent.x + previous.x);
}

// --- Pass 1: candidates and temporal reuse --------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void cyManyLightsCandidates(uint3 id : SV_DispatchThreadID)
{
    let extent = uint2(word(8).xy);
    if (id.x >= extent.x || id.y >= extent.y)
    {
        return;
    }
    let pixel = id.xy;
    let surface = surfaceAt(pixel);
    cyManyLights.surface[pixel] = surface.valid ? float4(surface.viewDepth, surface.normal)
                                                : float4(0.0);
    float confidence = 0.0;
    let source = historySource(surface, extent, confidence);
    cyManyLights.guidance[pixel] = float2(float(source), confidence);

    var reservoir = emptyReservoir();
    let lightCount = wordBits(9, 2);
    if (!surface.valid || lightCount == 0u)
    {
        cyManyLights.temporalReservoir[pixel] = packReservoir(reservoir);
        return;
    }
    var stream = streamFor(pixel, wordBits(9, 0), 1u);
    let draws = max(wordBits(9, 1), 1u);
    for (uint draw = 0u; draw < draws; ++draw)
    {
        uint light = 0u;
        float pdf = 0.0;
        drawCandidate(stream, lightCount, light, pdf);
        reservoirUpdate(reservoir, float(light), targetOf(light, surface), pdf, stream);
    }
    if ((wordBits(15, 0) != 0u) && source >= 0)
    {
        let previous = uint2(uint(source) % extent.x, uint(source) / extent.x);
        let history = toReservoir(cyManyLights.historyReservoir.Load(int2(previous)));
        if (reservoirValid(history))
        {
            let here = targetOf(uint(history.light), surface);
            reservoirCombine(reservoir, history, here, word(10).x, stream);
        }
    }
    cyManyLights.temporalReservoir[pixel] = packReservoir(reservoir);
}

// --- Pass 2: spatial reuse, visibility and the estimate ------------------------------------------

/// One visibility ray from the surface to `to`, marched through the depth buffer: occluded when the
/// ray passes behind a surface by less than the thickness. Off the screen, the ray is taken as
/// visible — the screen holds no occluder there.
float traceVisibility(CySurface surface, float3 to, uint2 extent)
{
    let steps = max(wordBits(10, 3), 1u);
    let thickness = word(14).x;
    let start = surface.position + surface.normal * word(14).y;
    let along = to - start;
    for (uint step = 1u; step <= steps; ++step)
    {
        let t = (float(step) - 0.5) / float(steps);
        let point = start + along * (t * 0.98);
        // The depth is compared in view space, the screen position found through this frame's
        // clip.
        let viewZ = -dot(word(11).xyz, point);
        if (viewZ <= 0.0)
        {
            return 1.0;
        }
        let clip = float4(dot(word(16), float4(point, 1.0)), dot(word(17), float4(point, 1.0)),
                          dot(word(18), float4(point, 1.0)), dot(word(19), float4(point, 1.0)));
        if (clip.w <= 0.0)
        {
            return 1.0;
        }
        let ndc = clip.xy / clip.w;
        let uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0)
        {
            return 1.0;
        }
        let texel = uint2(uv * float2(extent));
        let stored = cyManyLights.surface.Load(int2(texel)).x;
        if (stored > 0.0 && viewZ > stored + 1e-3 && viewZ < stored + thickness)
        {
            return 0.0;
        }
    }
    return 1.0;
}

// SPATIAL REUSE, WEIGHTED BY THE BALANCE HEURISTIC — `combine_spatial` in many_light.h.
//
// Each input — this pixel's reservoir and up to `spatial_neighbours` of its neighbours' on the same
// surface — offers its light weighted by `M_i p_i(y) / sum_j M_j p_j(y)`, the share of the light's
// target over every input's surface that input stands for. That makes the combination unbiased
// where the plain one is not: counting a neighbour's samples for a light that cannot reach the
// neighbour darkens every edge of every light's footprint (about ten per cent of a many-lit
// floor), and the 1/Z correction that leaves out such neighbours brightens it again (five per cent)
// because it also leaves out the neighbours whose reservoirs came up empty. The cost is a target
// evaluation per input pair: twenty-five for four neighbours.

static const uint kMaxSpatialInputs = 9u;

Reservoir spatialReuse(uint2 pixel, CySurface surface, Reservoir own, uint2 extent,
                       inout Stream stream)
{
    Reservoir inputs[kMaxSpatialInputs];
    CySurface surfaces[kMaxSpatialInputs];
    int2 places[kMaxSpatialInputs];
    inputs[0] = own;
    surfaces[0] = surface;
    places[0] = int2(pixel);
    uint count = 1u;
    let neighbours = min(wordBits(9, 3), kMaxSpatialInputs - 1u);
    let radius = word(10).y;
    for (uint index = 0u; index < neighbours; ++index)
    {
        let angle = nextUnit(stream) * 6.28318530717959;
        let distance = (0.5 + nextUnit(stream) * 0.5) * radius;
        let other = int2(pixel) + int2(round(float2(cos(angle), sin(angle)) * distance));
        if (other.x < 0 || other.y < 0 || other.x >= int(extent.x) || other.y >= int(extent.y))
        {
            continue;
        }
        bool repeated = false;
        for (uint seen = 0u; seen < count; ++seen)
        {
            repeated = repeated || all(places[seen] == other);
        }
        let theirs = cyManyLights.surface.Load(other);
        if (repeated || theirs.x <= 0.0 || abs(theirs.x - surface.viewDepth) > 0.1 * surface.viewDepth ||
            dot(theirs.yzw, surface.normal) < 0.9)
        {
            continue;
        }
        // AN EMPTY RESERVOIR IS STILL AN INPUT: its candidates found nothing, and they count in
        // every light's denominator all the same.
        inputs[count] = toReservoir(cyManyLights.temporalReservoir.Load(other));
        surfaces[count] = surfaceAt(uint2(other));
        places[count] = other;
        count += 1u;
    }
    Reservoir combined = emptyReservoir();
    float total = 0.0;
    for (uint index = 0u; index < count; ++index)
    {
        total += inputs[index].count;
    }
    for (uint index = 0u; index < count; ++index)
    {
        let candidate = inputs[index];
        if (!reservoirValid(candidate))
        {
            continue;
        }
        let light = uint(candidate.light);
        let here = targetOf(light, surface);
        if (!(here > 0.0))
        {
            continue;
        }
        float denominator = 0.0;
        for (uint other = 0u; other < count; ++other)
        {
            let target = other == index ? candidate.target : targetOf(light, surfaces[other]);
            denominator += inputs[other].count * target;
        }
        let share = denominator > 0.0 ? candidate.count * candidate.target / denominator : 0.0;
        let weight = share * here * contributionWeight(candidate);
        combined.weightSum += weight;
        if (weight > 0.0 && nextUnit(stream) * combined.weightSum <= weight)
        {
            combined.light = candidate.light;
            combined.target = here;
        }
    }
    // Stored so `contributionWeight` — weight sum over count and target — is the combination's
    // `weight_sum / target`, and next frame's history speaks for every sample that went in.
    combined.count = total;
    combined.weightSum *= total;
    return combined;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyManyLightsSpatial(uint3 id : SV_DispatchThreadID)
{
    let extent = uint2(word(8).xy);
    if (id.x >= extent.x || id.y >= extent.y)
    {
        return;
    }
    let pixel = id.xy;
    let surface = surfaceAt(pixel);
    var reservoir = toReservoir(cyManyLights.temporalReservoir.Load(int2(pixel)));
    if (!surface.valid)
    {
        cyManyLights.finalReservoir[pixel] = packReservoir(emptyReservoir());
        cyManyLights.noisy[pixel] = float4(0.0);
        cyManyLights.resolved[pixel] = float4(-1.0, 0.0, 0.0, 0.0);
        return;
    }
    var stream = streamFor(pixel, wordBits(9, 0), 2u);
    if (wordBits(15, 1) != 0u)
    {
        reservoir = spatialReuse(pixel, surface, reservoir, extent, stream);
    }
    cyManyLights.finalReservoir[pixel] = packReservoir(reservoir);
    if (!reservoirValid(reservoir))
    {
        cyManyLights.noisy[pixel] = float4(0.0);
        cyManyLights.resolved[pixel] = float4(-1.0, 0.0, 0.0, 0.0);
        return;
    }
    let light = cyManyLights.lights[uint(reservoir.light)];
    let weight = contributionWeight(reservoir);
    var visibility = 1.0;
    if (wordBits(15, 2) != 0u)
    {
        let point = sampleLightPoint(light, surface.position, nextUnit(stream), nextUnit(stream));
        visibility = traceVisibility(surface, point, extent);
    }
    let irradiance = lightIrradiance(light, surface.position, surface.normal);
    cyManyLights.noisy[pixel] = float4(irradiance * (weight * visibility), 1.0);
    cyManyLights.resolved[pixel] = float4(reservoir.light, weight * visibility, 0.0, 0.0);
}

// --- Pass 3: accumulation — `Denoiser::accumulate` -----------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void cyManyLightsAccumulate(uint3 id : SV_DispatchThreadID)
{
    let extent = uint2(word(8).xy);
    if (id.x >= extent.x || id.y >= extent.y)
    {
        return;
    }
    let pixel = id.xy;
    let value = cyManyLights.noisy.Load(int2(pixel)).rgb;
    let scalar = luminance(value);
    let fresh = 1.0;
    let history = cyManyLights.guidance.Load(int2(pixel));
    let historyLength = word(13).x;
    let alphaFloor = word(13).w;
    let source = int(history.x);
    let confidence = saturate(history.y);
    if (source < 0 || confidence <= 0.0)
    {
        cyManyLights.accumulated[pixel] = float4(value, 0.0);
        cyManyLights.moments[pixel] = float4(scalar, scalar * scalar, fresh, 0.0);
        return;
    }
    let from = uint2(uint(source) % extent.x, uint(source) / extent.x);
    let colour = cyManyLights.historyColour.Load(int2(from)).rgb;
    let moments = cyManyLights.historyMoments.Load(int2(from));
    let carried = moments.z * confidence;
    let count = carried + fresh;
    let alpha = max(count > 0.0 ? fresh / count : 1.0, alphaFloor);
    cyManyLights.accumulated[pixel] = float4(lerp(colour, value, alpha), 0.0);
    cyManyLights.moments[pixel] = float4(lerp(moments.x, scalar, alpha),
                                         lerp(moments.y, scalar * scalar, alpha),
                                         min(count, historyLength), 0.0);
}

// --- Pass 4: variance — `Denoiser::estimate_variance` --------------------------------------------

float stableVariance(float first, float second)
{
    let variance = max(0.0, second - first * first);
    let roundoff = 1e-6 * max(1.0, abs(second));
    return variance <= roundoff ? 0.0 : variance;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyManyLightsVariance(uint3 id : SV_DispatchThreadID)
{
    let extent = uint2(word(8).xy);
    if (id.x >= extent.x || id.y >= extent.y)
    {
        return;
    }
    let pixel = id.xy;
    let moments = cyManyLights.moments.Load(int2(pixel));
    var variance = stableVariance(moments.x, moments.y);
    if (moments.z < 4.0)
    {
        float sum = 0.0;
        float sumSquared = 0.0;
        float count = 0.0;
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                let other = int2(pixel) + int2(dx, dy);
                if (other.x < 0 || other.y < 0 || other.x >= int(extent.x) ||
                    other.y >= int(extent.y))
                {
                    continue;
                }
                let first = cyManyLights.moments.Load(other).x;
                sum += first;
                sumSquared += first * first;
                count += 1.0;
            }
        }
        let mean = sum / count;
        variance = max(variance, stableVariance(mean, sumSquared / count));
    }
    cyManyLights.variance[pixel] = variance;
}

// --- Passes 5 on: the a-trous cascade — `Denoiser::filter_pixel` ----------------------------------

float splineWeight(int offset)
{
    let magnitude = abs(offset);
    return magnitude == 0 ? 6.0 / 16.0 : (magnitude == 1 ? 4.0 / 16.0 : (magnitude == 2 ? 1.0 / 16.0 : 0.0));
}

/// One pixel of one a-trous pass.
float3 filterPixel(uint2 pixel, uint2 extent)
{
    let centre = cyManyLights.filterSource.Load(int2(pixel));
    var result = centre.rgb;
    let surface = cyManyLights.surface.Load(int2(pixel));
    let depth = surface.x;
    let sigmas = word(12);
    let shape = word(13);
    if (depth > 0.0)
    {
        let centreValue = luminance(centre.rgb);
        let deviation = sqrt(cyManyLights.variance.Load(int2(pixel)));
        let samples = cyManyLights.moments.Load(int2(pixel)).z;
        let converged = samples >= 12.0 && deviation <= 0.02 * (abs(centreValue) + 1e-6);
        if (!converged)
        {
            let step = max(1u, 1u << cyManyLightPush.pass);
            let reach = int(shape.z);
            float3 sum = float3(0.0);
            float weightSum = 0.0;
            for (int dy = -reach; dy <= reach; ++dy)
            {
                for (int dx = -reach; dx <= reach; ++dx)
                {
                    let tap = int2(pixel) + int2(dx, dy) * int(step);
                    if (tap.x < 0 || tap.y < 0 || tap.x >= int(extent.x) || tap.y >= int(extent.y))
                    {
                        continue;
                    }
                    let theirs = cyManyLights.surface.Load(tap);
                    if (theirs.x <= 0.0)
                    {
                        continue;
                    }
                    var weight = splineWeight(dx) * splineWeight(dy);
                    weight *= exp(-abs(theirs.x - depth) / (sigmas.x * abs(depth) + 1e-6));
                    weight *= pow(max(0.0, dot(surface.yzw, theirs.yzw)), sigmas.y);
                    let value = cyManyLights.filterSource.Load(tap).rgb;
                    weight *= exp(-abs(luminance(value) - centreValue) / (sigmas.z * deviation + 1e-6));
                    weight = max(weight, 0.0);
                    if (weight <= 0.0)
                    {
                        continue;
                    }
                    sum += value * weight;
                    weightSum += weight;
                }
            }
            if (weightSum > 0.0)
            {
                result = sum / weightSum;
            }
        }
    }
    return result;
}

/// Every pass but the last: into the other half of the ping-pong.
[shader("compute")]
[numthreads(8, 8, 1)]
void cyManyLightsFilter(uint3 id : SV_DispatchThreadID)
{
    let extent = uint2(word(8).xy);
    if (id.x >= extent.x || id.y >= extent.y)
    {
        return;
    }
    cyManyLights.filterOut[id.xy] = float4(filterPixel(id.xy, extent), 1.0);
}

/// The last pass: into the stage's target, which the forward pass samples.
[shader("compute")]
[numthreads(8, 8, 1)]
void cyManyLightsFilterLast(uint3 id : SV_DispatchThreadID)
{
    let extent = uint2(word(8).xy);
    if (id.x >= extent.x || id.y >= extent.y)
    {
        return;
    }
    cyManyLights.target[id.xy] = float4(filterPixel(id.xy, extent), 1.0);
}
