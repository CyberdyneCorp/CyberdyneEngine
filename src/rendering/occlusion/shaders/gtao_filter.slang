// SPDX-License-Identifier: MIT
// The ambient occlusion term through the shared denoiser's spatial stage, on the device.
//
// ================================================================================================
// THIS IS `cy::rendering::denoise::Denoiser`, NOT A SECOND FILTER
// ================================================================================================
//
// `rendering-post-processing`: "Filtering and temporal accumulation SHALL be performed by the shared
// denoiser (see `denoising`) with the signal declared as a VISIBILITY TERM, so contact detail is
// preserved rather than filtered as colour." The denoiser is a module of plain spans; this entry
// point is its `filter_pixel` and `tap_weight` transcribed expression for expression, and it is
// driven by `denoise::default_config(SignalKind::AmbientOcclusion)` and the quality ladder's first
// position — the four tolerances and the kernel extent arrive as push constants read off that
// table, not as numbers written here. `render.ambient_occlusion` runs the SAME raw term through
// `Denoiser::denoise` on the host and compares the two buffers, which is what makes "the shared
// denoiser" a measured claim rather than a comment.
//
// What is transcribed, stage by stage, for a frame with no reprojected history (every frame here —
// see "WHAT IS NOT HERE"):
//
//   * accumulation with no history is the raw signal, one sample a pixel;
//   * a pixel with fewer than four samples takes its variance SPATIALLY, from the 3x3 of the raw
//     signal around it (`Denoiser::estimate_variance`), computed once and held for the cascade —
//     so every pass reads the raw term to recompute it rather than carrying a fifth channel;
//   * the a-trous cascade: `max_passes` passes at steps 1, 2, 4, each a (2 * extent + 1)^2
//     B3-spline kernel whose taps are stopped by depth (relative), normal (a power) and the
//     VISIBILITY VALUE ITSELF at `sigma_value` standard deviations — the visibility domain, which
//     is what keeps a contact edge an edge.
//
// Roughness is not consulted: the ambient occlusion signal is hemispherical and declares zero
// widening, and the prepass writes no roughness this frame could stop on. Identity is not
// consulted: the forward frame has no visibility buffer.
//
// ================================================================================================
// WHAT IS NOT HERE: TEMPORAL ACCUMULATION
// ================================================================================================
//
// The denoiser's first stage blends with a reprojected history the temporal framework supplies.
// The horizon search's noise is fixed in screen space, so a still view needs no history to be
// still, and none is carried; `docs` and the requirements map record temporal accumulation of this
// signal as not built.

#include "gtao_common.slang"

/// `cy::rendering::occlusion::GtaoFilterConstants`, word for word.
struct CyGtaoFilterConstants
{
    /// x: P00, y: P11, z: m22, w: m32 — only z and w are read, for the view depth.
    float4 projection;
    /// x: sigma_depth, y: sigma_normal, z: sigma_value, w: unused.
    float4 sigma;
    /// x: the a-trous step in pixels, y: the kernel extent, z: width, w: height.
    uint4 control;
};

struct CyGtaoFilterSet
{
    /// A depth texture on Metal (`depth2d`), which is what the prepass target IS there; SPIR-V
    /// ignores the distinction.
    DepthTexture2D depth;
    Texture2D<float4> normals;
    /// The horizon search's output, which the variance is taken from.
    Texture2D<float4> raw;
    /// The previous pass's output: `raw` itself for the first pass.
    Texture2D<float4> source;
    [format("rgba16f")] RWTexture2D<float4> output;
};

[[vk::binding(0, 0)]] ParameterBlock<CyGtaoFilterSet> cyGtaoFilterSet;
[[vk::push_constant]] CyGtaoFilterConstants cyGtaoFilterConstants;

static const float kCyGtaoEpsilon = 1.0e-6;

/// The B3-spline row {1, 4, 6, 4, 1} / 16, by |offset|.
float splineWeight(int offset)
{
    let magnitude = abs(offset);
    return magnitude == 0 ? 6.0 / 16.0 : (magnitude == 1 ? 4.0 / 16.0 : (magnitude == 2 ? 1.0 / 16.0 : 0.0));
}

/// Moment subtraction's round-off treated as zero, as `stable_variance` does.
float stableVariance(float first, float second)
{
    let variance = max(0.0, second - first * first);
    let roundoff = kCyGtaoEpsilon * max(1.0, abs(second));
    return variance <= roundoff ? 0.0 : variance;
}

/// The spatial variance estimate of the raw visibility over the 3x3 around a pixel.
float rawVariance(int2 pixel, int2 size)
{
    float sum = 0.0;
    float squares = 0.0;
    float count = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            let tap = pixel + int2(dx, dy);
            if (tap.x < 0 || tap.y < 0 || tap.x >= size.x || tap.y >= size.y)
            {
                continue;
            }
            let value = cyGtaoFilterSet.raw.Load(int3(tap, 0)).a;
            sum += value;
            squares += value * value;
            count += 1.0;
        }
    }
    let mean = sum / count;
    // One sample: the temporal moments are the value and its square, whose variance is zero, so
    // the spatial estimate is the one that survives the `max` in `estimate_variance`.
    return stableVariance(mean, squares / count);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyGtaoFilter(uint3 thread : SV_DispatchThreadID)
{
    let size = int2(cyGtaoFilterConstants.control.zw);
    let pixel = int2(thread.xy);
    if (pixel.x >= size.x || pixel.y >= size.y)
    {
        return;
    }
    let centre = cyGtaoFilterSet.source.Load(int3(pixel, 0));
    let depth = cyGtaoFilterSet.depth.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        // A sky pixel has no surface to stop on.
        cyGtaoFilterSet.output[pixel] = centre;
        return;
    }

    let projection = cyGtaoFilterConstants.projection;
    let sigma = cyGtaoFilterConstants.sigma;
    let viewDepth = cyGtaoViewDepth(projection, depth);
    let normal = cyGtaoNormal(cyGtaoFilterSet.normals.Load(int3(pixel, 0)));
    let deviation = sqrt(rawVariance(pixel, size));
    let step = int(cyGtaoFilterConstants.control.x);
    let extent = int(cyGtaoFilterConstants.control.y);

    float4 sum = float4(0.0);
    float weightSum = 0.0;
    for (int dy = -extent; dy <= extent; ++dy)
    {
        for (int dx = -extent; dx <= extent; ++dx)
        {
            let tap = pixel + int2(dx, dy) * step;
            if (tap.x < 0 || tap.y < 0 || tap.x >= size.x || tap.y >= size.y)
            {
                continue;
            }
            let tapDepth = cyGtaoFilterSet.depth.Load(int3(tap, 0));
            if (tapDepth <= 0.0)
            {
                continue;
            }
            let tapViewDepth = cyGtaoViewDepth(projection, tapDepth);
            float weight = splineWeight(dx) * splineWeight(dy);
            weight *= exp(-abs(tapViewDepth - viewDepth) / (sigma.x * abs(viewDepth) + kCyGtaoEpsilon));
            let tapNormal = cyGtaoNormal(cyGtaoFilterSet.normals.Load(int3(tap, 0)));
            weight *= pow(max(0.0, dot(normal, tapNormal)), sigma.y);
            let value = cyGtaoFilterSet.source.Load(int3(tap, 0));
            weight *= exp(-abs(value.a - centre.a) / (sigma.z * deviation + kCyGtaoEpsilon));
            weight = max(weight, 0.0);
            if (weight <= 0.0)
            {
                continue;
            }
            sum += value * weight;
            weightSum += weight;
        }
    }
    cyGtaoFilterSet.output[pixel] = weightSum > 0.0 ? sum / weightSum : centre;
}
