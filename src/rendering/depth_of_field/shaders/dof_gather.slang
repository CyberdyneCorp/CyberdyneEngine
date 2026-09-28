// SPDX-License-Identifier: MIT
// Depth of field, dispatch 4 of 5: the far and near fields, gathered at half resolution.
//
// ================================================================================================
// SCATTER AS GATHER
// ================================================================================================
//
// A defocused point is a disc — the aperture's image — of the radius its circle of confusion
// gives, carrying the point's light spread evenly over the disc's area. A pixel receives light from
// every texel whose disc COVERS it: the gather visits texels around the pixel and keeps the ones
// whose own radius reaches it (`cyDofReach`), each weighted by the share of its disc the tap
// stands for — the tap's area over the disc's area, `a / (S rho^2)`. That is the scatter written as
// a gather, and it is what makes the blur's extent the lens's rather than the kernel's.
//
// THE FAR FIELD — texels behind the focus plane — is gathered over the pixel's OWN radius, and
// only from far texels. A focused pixel has no radius and gathers nothing, so a blurred background
// never spreads onto the focused object in front of it; and a focused texel has no radius to reach
// a background pixel with, so the object's colour never enters the background's blur either. The
// weights are normalised: the far field is the background as it would be seen without the
// objects in front of it.
//
// THE NEAR FIELD — texels in front of the focus plane — is gathered over how far the nearest
// out-of-focus surface around the pixel can reach (the dilated tiles), and its weights are NOT
// normalised: their sum is the fraction of the pixel covered by near-field discs. A focused pixel
// beside a defocused foreground is covered in part, and the composite lays the foreground's blur
// over it by exactly that fraction — near-field bleeding, the blur extending past the silhouette
// rather than being clipped to it.
//
//   far   rgb the far field's colour
//   near  rgb the near field's colour premultiplied by its coverage, a the coverage
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   python3 src/rendering/depth_of_field/shaders/regenerate.py --slangc <build>/Development/bin/slangc
//
// which compiles every entry point of the five dispatches, from the repository root, as
//
//   slangc src/rendering/depth_of_field/shaders/<file>.slang -I src/rendering/shaders \
//          -entry <entry> -stage compute -target spirv -profile spirv_1_5 -o <entry>.spv
//   slangc ... -target metal -o <entry>.metal
//
// then embed_spirv.py and embed_msl.py into ../src/dof_spirv.h and ../src/dof_msl.h, and
// clang-format over both.

#include "dof_common.slang"

struct CyDofGatherSet
{
    /// dof_setup.slang's layer: colour, and the signed radius in pixels of the target.
    Texture2D<float4> layer;
    /// The dilated tiles: x the near field's reach.
    Texture2D<float4> tiles;
    [format("rgba16f")] RWTexture2D<float4> farField;
    [format("rgba16f")] RWTexture2D<float4> nearField;
};

[[vk::binding(0, 0)]] ParameterBlock<CyDofGatherSet> cyDofGatherSet;

float4 cyDofLayerAt(int2 texel)
{
    return cyDofGatherSet.layer.Load(int3(clamp(texel, int2(0), int2(cyDof.extent.zw) - 1), 0));
}

/// The far field over the centre's own radius, normalised.
float3 cyDofGatherFar(int2 pixel, float4 centre)
{
    if (centre.a <= cyDof.lens.w)
    {
        return centre.rgb;
    }
    let radius = centre.a * 0.5;
    let rings = cyDofRings(radius);
    let span = cyDofSpan(radius, rings);
    float3 colour = float3(0.0);
    float weight = 0.0;
    for (uint index = 0; index < cyDofTapCount(rings); ++index)
    {
        let tap = cyDofTap(index, rings, span);
        let offset = int2(round(tap.xy));
        let taken = cyDofLayerAt(pixel + offset);
        if (taken.a <= cyDof.lens.w)
        {
            continue;
        }
        let rho = taken.a * 0.5;
        let w = tap.z * cyDofReach(float2(offset), rho) / (cyDof.aperture.z * rho * rho);
        colour += taken.rgb * w;
        weight += w;
    }
    return weight > 0.0 ? colour / weight : centre.rgb;
}

/// The near field over the tiles' reach, premultiplied by its coverage.
float4 cyDofGatherNear(int2 pixel)
{
    let reach = cyDofGatherSet.tiles.Load(int3(int2(uint2(pixel) / cyDof.tiles.x), 0)).x;
    if (reach <= cyDof.lens.w)
    {
        return float4(0.0);
    }
    let radius = reach * 0.5;
    let rings = cyDofRings(radius);
    let span = cyDofSpan(radius, rings);
    float3 colour = float3(0.0);
    float coverage = 0.0;
    for (uint index = 0; index < cyDofTapCount(rings); ++index)
    {
        let tap = cyDofTap(index, rings, span);
        let offset = int2(round(tap.xy));
        let taken = cyDofLayerAt(pixel + offset);
        if (taken.a >= -cyDof.lens.w)
        {
            continue;
        }
        let rho = -taken.a * 0.5;
        let w = tap.z * cyDofReach(float2(offset), rho) / (cyDof.aperture.z * rho * rho) *
                cyDofDefocus(taken.a);
        colour += taken.rgb * w;
        coverage += w;
    }
    if (coverage <= 0.0)
    {
        return float4(0.0);
    }
    let alpha = saturate(coverage);
    return float4(colour / coverage * alpha, alpha);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyDofGather(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= cyDof.extent.z || thread.y >= cyDof.extent.w)
    {
        return;
    }
    let pixel = int2(thread.xy);
    cyDofGatherSet.farField[thread.xy] = float4(cyDofGatherFar(pixel, cyDofLayerAt(pixel)), 1.0);
    cyDofGatherSet.nearField[thread.xy] = cyDofGatherNear(pixel);
}
