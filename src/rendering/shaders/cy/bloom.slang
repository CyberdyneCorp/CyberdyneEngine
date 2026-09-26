// Bloom: a soft-thresholded, progressive downsample and upsample chain, and the energy-conserving
// composite that puts it back. Part of the engine's shader library (`shader-system`), recorded by
// `cy::rendering::pipeline::BloomRenderer` into the passes `forward/bloom_chain.h` declares.
//
// ================================================================================================
// WHAT EACH ENTRY POINT IS, AGAINST `rendering-post-processing`'s BLOOM REQUIREMENT
// ================================================================================================
//
//   bloomPrefilter   the first downsample: the SOFT THRESHOLD KNEE on every tap, then the 13-tap
//                    filter with a KARIS AVERAGE over its five groups — "to suppress fireflies"
//   bloomDownsample  every later level: the same 13 taps, plainly averaged
//   bloomUpsample    a 3x3 TENT over the coarser level, "to avoid blocky artifacts", blended over
//                    this level's own downsample by `scatter`
//   bloomComposite   scene + intensity * (bloom - prefilter(scene)) — the energy moved, not added.
//                    The optional LENS DIRT scales the bloom term by (1 + dirt * lensDirt); at
//                    its default of zero it is absent. The ANAMORPHIC stretch widens every tent.
//
// Every expression that decides what bloom LOOKS like has a processor twin in
// `src/rendering/post/src/effects.cpp` — `bloom_prefilter`, `karis_weight`, `bloom_level_weight`,
// `bloom_composite` — and the two are kept expression for expression, so a unit test of one is a
// statement about the other.
//
// ================================================================================================
// COORDINATES COME FROM THE PIXEL, NOT FROM THE VERTEX
// ================================================================================================
//
// The vertex stage is `cy/fullscreen.slang`'s `fullscreenVertex`, bound unchanged. Each fragment
// derives its texture coordinate from `SV_Position` and the target's reciprocal extent rather than
// from the interpolated one, so the thirteen taps land on the same source texels on every backend,
// and the composite reads the scene with `Load` — the exact texel, no filter — which is what makes
// an intensity of zero return the scene bit for bit.
//
// ================================================================================================
// THE DESCRIPTOR SET CONVENTION
// ================================================================================================
//
// Pass resources, so set 2 (`cy/backends/shader/reflection.h`). Sets 0 and 1 are the frame's and
// are neither read nor bound by these passes; the pipeline layout names them so that it is the same
// shape as every other pass in the frame. The per-pass numbers are one push constant block.
//
// The compiled modules are checked in. `src/rendering/pipeline/shaders/embed_bloom.py` regenerates
// them: it runs slangc for SPIR-V and Metal and applies the Metal slot rule its header explains.

module bloom;

import cy.color;

public struct BloomPush
{
    /// xy: reciprocal of the source extent. zw: reciprocal of the target extent.
    public float4 extents;
    /// xy: reciprocal of the detail extent (the coarser level, or the bloom). zw: unused.
    public float4 detail;
    /// x: threshold, scene units. y: the knee's half width, scene units. z: 1 / threshold, which
    /// normalises the Karis weight. w: 1 when the Karis average is on.
    public float4 threshold;
    /// x: intensity. y: scatter. z: anamorphic stretch. w: lens dirt intensity (0 is none).
    public float4 blend;
};

[[vk::push_constant]]
public ConstantBuffer<BloomPush> bloomPush;

struct BloomPassSet
{
    Texture2D<float4> source;
    Texture2D<float4> detail;
    SamplerState linearClamp;
    /// The lens dirt mask, sampled over the whole frame. Bound to the source when there is none.
    Texture2D<float4> lensDirt;
};

[[vk::binding(0, 2)]] ParameterBlock<BloomPassSet> bloomSet;

public struct BloomFragment
{
    public float4 position : SV_Position;
    public float2 uv : TEXCOORD0;
};

/// `bloom_prefilter`: zero below the knee, the full excess above it, a quadratic ramp between.
float3 prefilter(float3 colour)
{
    let threshold = bloomPush.threshold.x;
    let knee = bloomPush.threshold.y;
    let luma = luminance(colour);
    float contribution = 0.0;
    if (knee > 1e-6)
    {
        let soft = clamp(luma - threshold + knee, 0.0, 2.0 * knee);
        contribution = (soft * soft) / (4.0 * knee);
    }
    let above = max(luma - threshold, contribution);
    let scale = luma > 1e-6 ? above / luma : 0.0;
    return colour * scale;
}

float3 tap(Texture2D<float4> texture, float2 uv, bool thresholded)
{
    let colour = texture.SampleLevel(bloomSet.linearClamp, uv, 0.0).rgb;
    return thresholded ? prefilter(colour) : colour;
}

/// `karis_weight`, over luminance measured in thresholds so that it suppresses the same outlier in
/// a sun-lit scene as in a dim one.
float karisWeight(float3 group)
{
    return 1.0 / (1.0 + luminance(group) * bloomPush.threshold.z);
}

/// The weights of the 13-tap filter's five boxes.
struct BoxWeights
{
    float centre;
    float topLeft;
    float topRight;
    float bottomLeft;
    float bottomRight;
};

/// The 13-tap downsample: four overlapping 2x2 boxes and a centre one, each bilinear tap already a
/// 2x2 average. Weighted 0.5 for the centre and 0.125 for each corner box, which sums to one. On the
/// first downsample, with the Karis average on, each box is also weighted by its own Karis weight and the result renormalised.
float3 downsample13(Texture2D<float4> texture, float2 uv, float2 texel, bool first)
{
    let a = tap(texture, uv + texel * float2(-2.0, -2.0), first);
    let b = tap(texture, uv + texel * float2(0.0, -2.0), first);
    let c = tap(texture, uv + texel * float2(2.0, -2.0), first);
    let d = tap(texture, uv + texel * float2(-1.0, -1.0), first);
    let e = tap(texture, uv + texel * float2(1.0, -1.0), first);
    let f = tap(texture, uv + texel * float2(-2.0, 0.0), first);
    let g = tap(texture, uv, first);
    let h = tap(texture, uv + texel * float2(2.0, 0.0), first);
    let i = tap(texture, uv + texel * float2(-1.0, 1.0), first);
    let j = tap(texture, uv + texel * float2(1.0, 1.0), first);
    let k = tap(texture, uv + texel * float2(-2.0, 2.0), first);
    let l = tap(texture, uv + texel * float2(0.0, 2.0), first);
    let m = tap(texture, uv + texel * float2(2.0, 2.0), first);

    let centre = (d + e + i + j) * 0.25;
    let topLeft = (a + b + f + g) * 0.25;
    let topRight = (b + c + g + h) * 0.25;
    let bottomLeft = (f + g + k + l) * 0.25;
    let bottomRight = (g + h + l + m) * 0.25;

    BoxWeights weights = { 0.5, 0.125, 0.125, 0.125, 0.125 };
    if (first && bloomPush.threshold.w > 0.5)
    {
        weights.centre *= karisWeight(centre);
        weights.topLeft *= karisWeight(topLeft);
        weights.topRight *= karisWeight(topRight);
        weights.bottomLeft *= karisWeight(bottomLeft);
        weights.bottomRight *= karisWeight(bottomRight);
    }
    let total = weights.centre + weights.topLeft + weights.topRight + weights.bottomLeft +
                weights.bottomRight;
    let sum = centre * weights.centre + topLeft * weights.topLeft + topRight * weights.topRight +
              bottomLeft * weights.bottomLeft + bottomRight * weights.bottomRight;
    return sum / max(total, 1e-6);
}

/// The 3x3 tent, 1-2-1 in each axis, one texel of `texture` out — horizontally, `anamorphic`
/// texels.
float3 tent(Texture2D<float4> texture, float2 uv, float2 texel)
{
    let offset = texel * float2(bloomPush.blend.z, 1.0);
    float3 sum = tap(texture, uv, false) * 4.0;
    sum += tap(texture, uv + float2(-offset.x, 0.0), false) * 2.0;
    sum += tap(texture, uv + float2(offset.x, 0.0), false) * 2.0;
    sum += tap(texture, uv + float2(0.0, -offset.y), false) * 2.0;
    sum += tap(texture, uv + float2(0.0, offset.y), false) * 2.0;
    sum += tap(texture, uv + float2(-offset.x, -offset.y), false);
    sum += tap(texture, uv + float2(offset.x, -offset.y), false);
    sum += tap(texture, uv + float2(-offset.x, offset.y), false);
    sum += tap(texture, uv + float2(offset.x, offset.y), false);
    return sum * (1.0 / 16.0);
}

float2 targetUv(BloomFragment input)
{
    return input.position.xy * bloomPush.extents.zw;
}

[shader("fragment")]
public float4 bloomPrefilter(BloomFragment input) : SV_Target
{
    let colour = downsample13(bloomSet.source, targetUv(input), bloomPush.extents.xy, true);
    return float4(colour, 1.0);
}

[shader("fragment")]
public float4 bloomDownsample(BloomFragment input) : SV_Target
{
    let colour = downsample13(bloomSet.source, targetUv(input), bloomPush.extents.xy, false);
    return float4(colour, 1.0);
}

/// `(1 - s) * own + s * tent(coarser)`: the level weights `(1 - s) s^k` that sum to one.
[shader("fragment")]
public float4 bloomUpsample(BloomFragment input) : SV_Target
{
    let uv = targetUv(input);
    let own = tap(bloomSet.source, uv, false);
    let coarser = tent(bloomSet.detail, uv, bloomPush.detail.xy);
    return float4(lerp(own, coarser, bloomPush.blend.y), 1.0);
}

/// `bloom_composite`. The scene is read with `Load`, so a zero intensity is the scene exactly.
[shader("fragment")]
public float4 bloomComposite(BloomFragment input) : SV_Target
{
    let scene = bloomSet.source.Load(int3(int2(input.position.xy), 0));
    let uv = targetUv(input);
    float3 bloom = tent(bloomSet.detail, uv, bloomPush.detail.xy);
    if (bloomPush.blend.w > 0.0)
    {
        let dirt = bloomSet.lensDirt.SampleLevel(bloomSet.linearClamp, uv, 0.0).rgb;
        bloom *= 1.0 + dirt * bloomPush.blend.w;
    }
    let moved = bloomPush.blend.x * (bloom - prefilter(scene.rgb));
    return float4(scene.rgb + moved, scene.a);
}
