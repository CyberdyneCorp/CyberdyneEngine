// SPDX-License-Identifier: MIT
// The temporal upscaler, stage three: contrast-adaptive sharpening of the reconstruction, into the
// colour the post chain continues from. AMD FidelityFX's robust contrast-adaptive sharpening
// (RCAS): the largest negative lobe the five-tap cross's own minimum and maximum allow without
// clipping, times the sharpness — in the reconstruction's tone curve, where the bounds are 0 and 1.
//
// THE HISTORY IS NOT SHARPENED. This reads the reconstruction and writes a separate target, because
// a sharpened history feeds back into itself and compounds every frame.
//
// At zero sharpness the reconstruction is copied, bit for bit.
//
// OUTPUT: rgb the sharpened linear colour, a one.
//
// REGENERATING: see upscale_reconstruct.slang.

struct CySharpenConstants
{
    /// xy: the output extent.
    uint4 extent;
    /// x: sharpness in [0, 1], y: exposure.
    float4 control;
};

struct CyUpscaleSharpenSet
{
    Texture2D<float4> reconstructed;
    [format("rgba16f")] RWTexture2D<float4> sharpened;
};

[[vk::binding(0, 0)]] ParameterBlock<CyUpscaleSharpenSet> cyUpscaleSharpenSet;

// After the set: Metal buffer 0 is the set, buffer 1 the push constants (the Metal RHI's
// convention, set N at buffer N and push constants at the set count).
[[vk::push_constant]] CySharpenConstants cySharpen;

float3 cySharpenTonemap(float3 c, float exposure)
{
    let exposed = c * exposure;
    return exposed * (1.0 / (1.0 + max(max(exposed.r, max(exposed.g, exposed.b)), 0.0)));
}

float3 cySharpenUntonemap(float3 t, float exposure)
{
    let headroom = max(1.0 - max(t.r, max(t.g, t.b)), 1.0e-4);
    return t * (1.0 / (exposure * headroom));
}

float3 cySharpenAt(int2 texel)
{
    let size = int2(cySharpen.extent.xy);
    let clamped = clamp(texel, int2(0, 0), size - int2(1, 1));
    return cyUpscaleSharpenSet.reconstructed.Load(int3(clamped, 0)).rgb;
}

float cySharpenLobe(float low, float high)
{
    let hitLow = low / max(4.0 * high, 1.0e-6);
    let hitHigh = (1.0 - high) / min(4.0 * low - 4.0, -1.0e-6);
    return max(-hitLow, hitHigh);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyUpscaleSharpen(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= cySharpen.extent.x || thread.y >= cySharpen.extent.y)
    {
        return;
    }
    let sharpness = cySharpen.control.x;
    let exposure = cySharpen.control.y;
    let pixel = int2(thread.xy);
    let centre = cySharpenAt(pixel);
    float3 result = centre;
    if (sharpness > 0.0)
    {
        let b = cySharpenTonemap(cySharpenAt(pixel + int2(0, -1)), exposure);
        let d = cySharpenTonemap(cySharpenAt(pixel + int2(-1, 0)), exposure);
        let e = cySharpenTonemap(centre, exposure);
        let f = cySharpenTonemap(cySharpenAt(pixel + int2(1, 0)), exposure);
        let h = cySharpenTonemap(cySharpenAt(pixel + int2(0, 1)), exposure);
        let low = min(min(min(b, d), min(e, f)), h);
        let high = max(max(max(b, d), max(e, f)), h);
        let lobeChannels = max(cySharpenLobe(low.r, high.r),
                               max(cySharpenLobe(low.g, high.g), cySharpenLobe(low.b, high.b)));
        let lobe = max(-0.1875, min(lobeChannels, 0.0)) * sharpness;
        let sharpened = ((b + d + f + h) * lobe + e) * (1.0 / (4.0 * lobe + 1.0));
        result = cySharpenUntonemap(max(sharpened, float3(0.0, 0.0, 0.0)), exposure);
    }
    cyUpscaleSharpenSet.sharpened[thread.xy] = float4(result, 1.0);
}
