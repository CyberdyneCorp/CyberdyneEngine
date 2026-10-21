// SPDX-License-Identifier: MIT
// The temporal upscaler, stage one: reconstruct every OUTPUT pixel from this frame's jittered
// RENDER samples and its reprojected history. `rendering-post-processing` — "Temporal upscaling".
//
// One thread an output pixel:
//
//   1. the 3x3 render texels around the sample nearest the pixel's centre give a spatial
//      reconstruction (a kernel in render pixels), this frame's evidence for the pixel (a narrower
//      kernel in output pixels), the neighbourhood's moments, the nearest surface's depth and
//      motion, whether the centre sample is a thin feature, and — with a pre-translucency copy —
//      how reactive the pixel is;
//   2. the history is fetched bicubically where the motion says the surface was;
//   3. its trust is withdrawn where the depth the history kept is nearer than anything within two
//      render pixels would have been last frame (disocclusion), where
//      the neighbourhood's brightness changed against the history at the same sample positions
//      (a change of shading), and where the pixel is reactive; a lock protecting a thin feature
//      is carried over, faded, or broken; and the history is clipped toward the neighbourhood's
//      box as far as this frame observed the pixel, unless it is locked;
//   4. the new texel is the history's colour as `weight` samples plus this frame's evidence, in
//      linear light, and its weight the sum, capped — or, with no history, the spatial
//      reconstruction.
//
// OUTPUT: rgb the reconstructed linear colour, a the evidence it holds in samples; and the state,
// the lock and the depth of the surface the texel holds.
//
// REGENERATING: `python3 src/rendering/upscale/shaders/regenerate.py --slangc <slangc>`; the script's
// docstring carries the invocations.

struct CyUpscaleReconstructSet
{
    /// This frame's linear HDR colour at the render extent.
    Texture2D<float4> color;
    /// A depth texture on Metal (`depth2d`), which is what the prepass target IS there.
    DepthTexture2D depth;
    /// The prepass motion vectors; read only with `kCyUpscaleVelocity`.
    Texture2D<float2> velocity;
    /// The colour before translucency; read only with `kCyUpscaleReactive`.
    Texture2D<float4> opaque;
    /// Last frame's history and state — its lock and the depth of the surface it holds — at the
    /// output extent; read only with `kCyUpscaleHistoryValid`.
    Texture2D<float4> history;
    Texture2D<float2> previousState;
    [format("rgba16f")] RWTexture2D<float4> reconstructed;
    [format("rg32f")] RWTexture2D<float2> state;
};

[[vk::binding(0, 0)]] ParameterBlock<CyUpscaleReconstructSet> cyUpscaleReconstructSet;

// AFTER the set, so Slang gives the set Metal buffer 0 and the push constants buffer 1 — the Metal
// RHI's convention: set N at buffer N, push constants at the set count.
#include "upscale_common.slang"

/// `kEvidenceFalloff` in upscale.cpp: the evidence kernel, exp(-4 d^2) in output pixels.
static const float kCyUpscaleEvidenceFalloff = 4.0;
/// `kShadingMargin`, `kShadingRelative`, `kShadingKeep`, `kShadingRange` in upscale.cpp.
static const float kCyUpscaleShadingMargin = 0.02;
static const float kCyUpscaleShadingRelative = 0.10;
static const float kCyUpscaleShadingKeep = 0.25;
static const float kCyUpscaleShadingRange = 0.35;
/// `kLockContrast`, `kLockReach`, `kLockFrames` in upscale.cpp.
static const float kCyUpscaleLockContrast = 0.05;
static const float kCyUpscaleLockReach = 0.05;
static const float kCyUpscaleLockFrames = 64.0;
/// `kSettleFrames` in upscale.cpp.
static const float kCyUpscaleSettleFrames = 4.0;
/// The taps across the centre in each of the four directions: horizontal, vertical, the diagonals.
static const int2 kCyUpscaleRidgePairs[4] = { int2(3, 5), int2(1, 7), int2(0, 8), int2(2, 6) };

void cyUpscaleCatmullRomWeights(float f, out float4 w)
{
    w.x = f * (-0.5 + f * (1.0 - 0.5 * f));
    w.y = 1.0 + f * f * (-2.5 + 1.5 * f);
    w.z = f * (0.5 + f * (2.0 - 1.5 * f));
    w.w = f * f * (-0.5 + 0.5 * f);
}

/// The history at `uv`, bicubic over the sixteen texels around it with clamped edges.
float4 cyUpscaleSampleHistory(float2 uv)
{
    let size = cyUpscaleOutputExtent();
    let p = uv * float2(size) - 0.5;
    let f = floor(p);
    float4 wx;
    float4 wy;
    cyUpscaleCatmullRomWeights(p.x - f.x, wx);
    cyUpscaleCatmullRomWeights(p.y - f.y, wy);
    let origin = int2(f) - int2(1, 1);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (int b = 0; b < 4; ++b)
    {
        for (int a = 0; a < 4; ++a)
        {
            let texel = cyUpscaleClampTexel(origin + int2(a, b), size);
            sum += cyUpscaleReconstructSet.history.Load(int3(texel, 0)) * (wx[a] * wy[b]);
        }
    }
    return sum;
}

/// `previous_state_at` in upscale.cpp: the nearest texel's lock and depth, and the nearest depth
/// any of the 3x3 texels around it kept.
float3 cyUpscalePreviousStateAt(float2 uv)
{
    let size = cyUpscaleOutputExtent();
    let centre = int2(floor(uv * float2(size)));
    let own = cyUpscaleReconstructSet.previousState.Load(int3(cyUpscaleClampTexel(centre, size), 0));
    float near = own.y;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            let texel = cyUpscaleClampTexel(centre + int2(dx, dy), size);
            near = max(near, cyUpscaleReconstructSet.previousState.Load(int3(texel, 0)).y);
        }
    }
    return float3(own.x, own.y, near);
}

float cyUpscaleReactive(float3 finalColour, float3 opaque, float exposure)
{
    let difference = abs(cyUpscaleTonemap(finalColour, exposure) - cyUpscaleTonemap(opaque, exposure));
    return saturate(cyUpscaleLargest(difference) * 4.0);
}

/// `Neighbourhood` in upscale.cpp.
struct CyUpscaleNeighbourhood
{
    float3 spatial;
    float3 evidence;
    float evidenceWeight;
    float3 mean;
    float3 deviation;
    float closestDepth;
    int2 closest;
    float nearDepth;
    float reactive;
    float2 sampleUv[9];
    float luma[9];
    float3 ycocg[9];
    float centreWeight;
};

CyUpscaleNeighbourhood cyUpscaleGather(float2 p, float2 ratio, float exposure)
{
    let render = cyUpscaleRenderExtent();
    let base = cyUpscaleNearestTexel(p);
    let hasReactive = (cyUpscaleFlags() & kCyUpscaleReactive) != 0;
    CyUpscaleNeighbourhood n;
    n.spatial = float3(0.0, 0.0, 0.0);
    n.evidence = float3(0.0, 0.0, 0.0);
    n.evidenceWeight = 0.0;
    n.closestDepth = -1.0;
    n.closest = int2(0, 0);
    n.reactive = 0.0;
    n.centreWeight = 0.0;
    float spatialWeight = 0.0;
    float3 firstMoment = float3(0.0, 0.0, 0.0);
    float3 secondMoment = float3(0.0, 0.0, 0.0);
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            let texel = cyUpscaleClampTexel(base + int2(dx, dy), render);
            let colour = cyUpscaleReconstructSet.color.Load(int3(texel, 0)).rgb;
            let sample = float2(texel) + 0.5 + cyUpscale.sample.xy;
            n.sampleUv[tap] = sample / float2(render);
            let renderDistance = p - sample;
            let outputDistance = renderDistance * ratio;
            let spatialTap = exp(-kCyUpscaleKernelFalloff * dot(renderDistance, renderDistance));
            let evidenceTap = exp(-kCyUpscaleEvidenceFalloff * dot(outputDistance, outputDistance));
            n.spatial += colour * spatialTap;
            spatialWeight += spatialTap;
            n.evidence += colour * evidenceTap;
            n.evidenceWeight += evidenceTap;
            let ycocg = cyUpscaleToYCoCg(cyUpscaleTonemap(colour, exposure));
            firstMoment += ycocg;
            secondMoment += ycocg * ycocg;
            n.luma[tap] = ycocg.x;
            n.ycocg[tap] = ycocg;
            if (tap == 4)
            {
                n.centreWeight = evidenceTap;
            }
            let depth = cyUpscaleReconstructSet.depth.Load(int3(texel, 0));
            if (depth > n.closestDepth)
            {
                n.closestDepth = depth;
                n.closest = texel;
            }
            if (hasReactive)
            {
                let opaque = cyUpscaleReconstructSet.opaque.Load(int3(texel, 0)).rgb;
                n.reactive = max(n.reactive, cyUpscaleReactive(colour, opaque, exposure));
            }
            ++tap;
        }
    }
    n.nearDepth = n.closestDepth;
    for (int ry = -2; ry <= 2; ++ry)
    {
        for (int rx = -2; rx <= 2; ++rx)
        {
            if (rx >= -1 && rx <= 1 && ry >= -1 && ry <= 1)
            {
                continue;
            }
            let texel = cyUpscaleClampTexel(base + int2(rx, ry), render);
            n.nearDepth = max(n.nearDepth, cyUpscaleReconstructSet.depth.Load(int3(texel, 0)));
        }
    }
    n.spatial = n.spatial * (1.0 / spatialWeight);
    n.mean = firstMoment * (1.0 / 9.0);
    let variance = secondMoment * (1.0 / 9.0) - n.mean * n.mean;
    n.deviation = sqrt(max(variance, float3(0.0, 0.0, 0.0)));
    return n;
}

/// `thin_feature` in upscale.cpp: the centre sample is a ridge in one of the four directions.
bool cyUpscaleThinFeature(CyUpscaleNeighbourhood n)
{
    let centre = n.luma[4];
    bool ridge = false;
    for (int index = 0; index < 4; ++index)
    {
        let a = n.luma[kCyUpscaleRidgePairs[index].x];
        let b = n.luma[kCyUpscaleRidgePairs[index].y];
        if (centre - max(a, b) > kCyUpscaleLockContrast ||
            min(a, b) - centre > kCyUpscaleLockContrast)
        {
            ridge = true;
        }
    }
    return ridge;
}

/// `unexplained_fraction` in upscale.cpp.
float cyUpscaleUnexplainedFraction(CyUpscaleNeighbourhood n, float2 motion, float exposure)
{
    let size = cyUpscaleOutputExtent();
    float unexplained = 0.0;
    for (int tap = 0; tap < 9; ++tap)
    {
        let p = (n.sampleUv[tap] + motion) * float2(size) - 0.5;
        let origin = int2(floor(p));
        float3 low = float3(1.0e9, 1.0e9, 1.0e9);
        float3 high = float3(-1.0e9, -1.0e9, -1.0e9);
        for (int b = 0; b < 2; ++b)
        {
            for (int a = 0; a < 2; ++a)
            {
                let texel = cyUpscaleClampTexel(origin + int2(a, b), size);
                let colour = max(cyUpscaleReconstructSet.history.Load(int3(texel, 0)).rgb,
                                 float3(0.0, 0.0, 0.0));
                let value = cyUpscaleToYCoCg(cyUpscaleTonemap(colour, exposure));
                low = min(low, value);
                high = max(high, value);
            }
        }
        let margin = kCyUpscaleShadingMargin + kCyUpscaleShadingRelative * max(high.x, 0.0);
        let sample = n.ycocg[tap];
        let outside = any(sample < low - margin) || any(sample > high + margin);
        unexplained += outside ? 1.0 : 0.0;
    }
    return unexplained * (1.0 / 9.0);
}

/// `rectify` in upscale.cpp.
float3 cyUpscaleRectify(CyUpscaleNeighbourhood n, float3 history, float gamma, float observed,
                        float exposure)
{
    let extent = n.deviation * gamma;
    let offset = cyUpscaleToYCoCg(cyUpscaleTonemap(history, exposure)) - n.mean;
    let outside = max(abs(offset.x) / max(extent.x, 1.0e-5),
                      max(abs(offset.y) / max(extent.y, 1.0e-5),
                          abs(offset.z) / max(extent.z, 1.0e-5)));
    let pull = outside > 1.0 ? observed * (1.0 - 1.0 / outside) : 0.0;
    if (pull <= 0.0)
    {
        return history;
    }
    return cyUpscaleUntonemap(cyUpscaleFromYCoCg(n.mean + offset * (1.0 - pull)), exposure);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyUpscaleReconstruct(uint3 thread : SV_DispatchThreadID)
{
    let output = cyUpscaleOutputExtent();
    if (thread.x >= output.x || thread.y >= output.y)
    {
        return;
    }
    let render = cyUpscaleRenderExtent();
    let exposure = cyUpscale.control.z;
    let flags = cyUpscaleFlags();
    let ratio = float2(output) / float2(render);
    let uv = cyUpscaleOutputUv(thread.xy);
    let p = uv * float2(render);
    let n = cyUpscaleGather(p, ratio, exposure);

    let motion = ((flags & kCyUpscaleVelocity) != 0 && n.closestDepth > 0.0)
                     ? cyUpscaleReconstructSet.velocity.Load(int3(n.closest, 0))
                     : cyUpscaleCameraMotion(uv, n.closestDepth);
    let historyUv = uv + motion;
    let motionPixels = length(motion * float2(output));
    let moving = saturate(motionPixels / cyUpscale.history.z);
    let cap = lerp(cyUpscale.history.x, cyUpscale.history.y, moving);

    float weight = 0.0;
    float lock = 0.0;
    float keptDepth = n.closestDepth;
    float3 historyLinear = float3(0.0, 0.0, 0.0);
    let onScreen = all(historyUv >= float2(0.0, 0.0)) && all(historyUv <= float2(1.0, 1.0));
    if ((flags & kCyUpscaleHistoryValid) != 0 && onScreen)
    {
        let history = cyUpscaleSampleHistory(historyUv);
        let previous = cyUpscalePreviousStateAt(historyUv);
        weight = min(max(history.a, 0.0), cap);
        float trust = 1.0;
        bool disoccluded = false;
        if ((flags & kCyUpscaleRejectDisocclusion) != 0)
        {
            let tolerance = 1.0 - cyUpscale.history.w;
            let near = cyUpscalePreviousClip(uv, n.nearDepth);
            let closest = cyUpscalePreviousClip(uv, n.closestDepth);
            if (cyUpscaleViewDepth(previous.z) < cyUpscaleViewDepth(near.z) * tolerance)
            {
                trust = 0.0;
                disoccluded = true;
            }
            else if (cyUpscaleViewDepth(previous.y) < cyUpscaleViewDepth(closest.z) * tolerance)
            {
                keptDepth = previous.y;
            }
        }
        if ((flags & kCyUpscaleShadingChange) != 0)
        {
            let change = cyUpscaleUnexplainedFraction(n, motion, exposure);
            trust = min(trust,
                        saturate(1.0 - (change - kCyUpscaleShadingKeep) / kCyUpscaleShadingRange));
        }
        trust *= 1.0 - n.reactive * cyUpscale.control.w;
        weight *= trust;

        if (disoccluded || moving > 0.5)
        {
            lock = -1.0;
        }
        else if (previous.x < 0.0)
        {
            lock = min(previous.x + 1.0 / kCyUpscaleSettleFrames, 0.0);
        }
        else if (trust < 0.5)
        {
            lock = 0.0;
        }
        else if ((flags & kCyUpscaleLocks) != 0)
        {
            lock = max(previous.x - 1.0 / kCyUpscaleLockFrames, 0.0);
        }
        let unsettled = previous.x < 0.0 || lock < 0.0;

        let gamma = lerp(cyUpscale.control.x, cyUpscale.control.y, moving);
        let observed =
            unsettled ? 1.0 : max(moving, saturate(n.evidenceWeight)) * (1.0 - lock);
        historyLinear =
            cyUpscaleRectify(n, max(history.rgb, float3(0.0, 0.0, 0.0)), gamma, observed, exposure);
    }
    if ((flags & kCyUpscaleLocks) != 0 && lock >= 0.0 && n.centreWeight >= kCyUpscaleLockReach &&
        cyUpscaleThinFeature(n) && moving <= 0.5)
    {
        lock = 1.0;
    }

    float4 result;
    if (weight <= 0.0)
    {
        result = float4(n.spatial, min(n.evidenceWeight, cap));
    }
    else
    {
        let blended = (historyLinear * weight + n.evidence) * (1.0 / (weight + n.evidenceWeight));
        result = float4(blended, min(weight + n.evidenceWeight, cap));
    }
    cyUpscaleReconstructSet.reconstructed[thread.xy] = result;
    cyUpscaleReconstructSet.state[thread.xy] = float2(lock, keptDepth);
}
