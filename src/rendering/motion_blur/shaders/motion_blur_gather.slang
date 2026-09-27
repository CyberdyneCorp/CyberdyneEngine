// SPDX-License-Identifier: MIT
// Motion blur, stage three: the reconstruction filter. `rendering-post-processing` — "Motion blur":
// "gather along the dominant direction with depth-aware weighting so background does not smear
// over foreground".
//
// ================================================================================================
// WHAT THIS COMPUTES
// ================================================================================================
//
// McGuire et al.'s reconstruction filter (I3D 2012). Along the neighbourhood's dominant blur vector
// `vn`, `samples - 1` taps are placed symmetrically about the pixel at a screen-fixed sub-step
// offset, and each tap Y contributes to pixel X by three weights:
//
//   Y NEARER than X, by the soft depth      as far as Y's OWN streak reaches:   cone(|X-Y|, |V[Y]|)
//   Y FARTHER than X                         as far as X's streak reveals it:   cone(|X-Y|, |V[X]|)
//   both about as near                       where both streaks cover the tap:  cylinder x cylinder
//
// The pixel itself weighs 1 / |V[X]|, so a still pixel is its own colour unless something nearer
// streaks across it — which is the "background stays sharp" half of the requirement: a still
// background's radius is half a pixel, it reaches nothing, and a background tap farther than a
// moving object's pixel only shows through where that object's own streak uncovers it.
//
// A pixel whose whole neighbourhood blurs less than half a pixel is COPIED, not reconstructed: the
// early return below. That is what makes a closed shutter byte-identical to no stage at all.
//
// OUTPUT: rgb the blurred linear HDR colour, a the pixel's own alpha.
//
// REGENERATING: see motion_blur_tile_max.slang.

#include "motion_blur_common.slang"

struct CyMotionBlurGatherSet
{
    /// The temporal resolve's output — linear HDR scene colour.
    Texture2D<float4> color;
    Texture2D<float2> velocity;
    DepthTexture2D depth;
    Texture2D<float4> neighbours;
    [format("rgba16f")] RWTexture2D<float4> output;
};

[[vk::binding(0, 0)]] ParameterBlock<CyMotionBlurGatherSet> cyMotionBlurGatherSet;

/// Interleaved gradient noise at the pixel centre: the fraction of a step every tap is offset by.
/// `contact_shadows`' noise, fixed in screen space.
float cyMotionBlurNoise(uint2 pixel)
{
    let centre = float2(pixel) + 0.5;
    return frac(52.9829189 * frac(centre.x * 0.06711056 + centre.y * 0.00583715));
}

float cyMotionBlurCone(float distance, float radius)
{
    return saturate(1.0 - distance / radius);
}

float cyMotionBlurCylinder(float distance, float radius)
{
    let edge0 = 0.95 * radius;
    let edge1 = 1.05 * radius;
    let t = saturate((distance - edge0) / (edge1 - edge0));
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

/// 1 when `b` is nearer than `a`, 0 when it is farther by more than the soft depth.
float cyMotionBlurSoftDepthCompare(float a, float b)
{
    return saturate(1.0 - (b - a) / cyMotionBlur.scales.w);
}

float cyMotionBlurRadiusAt(uint2 pixel)
{
    let velocity = cyMotionBlurGatherSet.velocity.Load(int3(int2(pixel), 0));
    let depth = cyMotionBlurGatherSet.depth.Load(int3(int2(pixel), 0));
    return max(length(cyMotionBlurVector(pixel, velocity, depth)), 0.5);
}

float4 cyMotionBlurReconstruct(uint2 pixel)
{
    let centre = cyMotionBlurGatherSet.color.Load(int3(int2(pixel), 0));
    let edge = cyMotionBlur.control.y;
    let dominant = cyMotionBlurGatherSet.neighbours.Load(int3(int2(pixel / edge), 0)).xy;
    if (dot(dominant, dominant) < 0.25)
    {
        return centre;
    }
    let ownRadius = cyMotionBlurRadiusAt(pixel);
    let ownDepth = cyMotionBlurViewDepth(cyMotionBlurGatherSet.depth.Load(int3(int2(pixel), 0)));
    let lastPixel = cyMotionBlur.extent.xy - 1.0;

    float weight = 1.0 / ownRadius;
    float3 sum = centre.rgb * weight;
    let jitter = cyMotionBlurNoise(pixel) - 0.5;
    let samples = cyMotionBlur.control.x;
    let middle = (samples - 1u) / 2u;
    for (uint index = 0u; index < samples; ++index)
    {
        if (index == middle)
        {
            continue;
        }
        let t = -1.0 + 2.0 * (float(index) + jitter + 1.0) / (float(samples) + 1.0);
        let offset = dominant * t;
        let position = clamp(floor(float2(pixel) + 0.5 + offset), float2(0.0, 0.0), lastPixel);
        let tap = uint2(position);
        let distance = length(offset);
        let tapDepth = cyMotionBlurViewDepth(cyMotionBlurGatherSet.depth.Load(int3(int2(tap), 0)));
        let tapRadius = cyMotionBlurRadiusAt(tap);
        let front = cyMotionBlurSoftDepthCompare(ownDepth, tapDepth);
        let back = cyMotionBlurSoftDepthCompare(tapDepth, ownDepth);
        let contribution = front * cyMotionBlurCone(distance, tapRadius) +
                           back * cyMotionBlurCone(distance, ownRadius) +
                           cyMotionBlurCylinder(distance, tapRadius) *
                               cyMotionBlurCylinder(distance, ownRadius) * 2.0;
        weight += contribution;
        sum += cyMotionBlurGatherSet.color.Load(int3(int2(tap), 0)).rgb * contribution;
    }
    return float4(sum / weight, centre.a);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyMotionBlurGather(uint3 thread : SV_DispatchThreadID)
{
    let size = uint2(cyMotionBlur.extent.xy);
    if (thread.x >= size.x || thread.y >= size.y)
    {
        return;
    }
    cyMotionBlurGatherSet.output[int2(thread.xy)] = cyMotionBlurReconstruct(thread.xy);
}
