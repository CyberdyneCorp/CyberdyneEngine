// SPDX-License-Identifier: MIT
// Depth of field, dispatch 5 of 5: the fields over the sharp image, at full resolution.
//
//   result = lerp(sharp, far, defocus(r))   where the pixel is behind the focus plane
//   result = result (1 - near.a) + near.rgb  the near field over everything, by its coverage
//
// `r` is the pixel's own radius from the full-resolution depth, so the focus band is decided per
// pixel rather than per half-resolution texel. The far field is upsampled BILATERALLY: of the
// four half-resolution texels around the pixel, only those that are themselves in the far field
// contribute, so a focused silhouette's texel never leaks into the background beside it. The near
// field is upsampled plainly — premultiplied, so the interpolation is of light rather than of a
// colour and a coverage separately — because spreading is what it is for.
//
// A pixel whose radius is inside the focus band and that no near-field disc covers is written as
// it was read, the same bits: an in-focus plane is unchanged, and a pinhole (K = 0) is the stage's
// identity. render.depth_of_field asserts both.
//
// See dof_gather.slang for how these files are compiled.

#include "dof_common.slang"

struct CyDofCompositeSet
{
    Texture2D<float4> source;
    DepthTexture2D depth;
    Texture2D<float4> layer;
    Texture2D<float4> farField;
    Texture2D<float4> nearField;
    [format("rgba16f")] RWTexture2D<float4> target;
};

[[vk::binding(0, 0)]] ParameterBlock<CyDofCompositeSet> cyDofCompositeSet;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyDofComposite(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= cyDof.extent.x || thread.y >= cyDof.extent.y)
    {
        return;
    }
    let pixel = int2(thread.xy);
    let sharp = cyDofCompositeSet.source.Load(int3(pixel, 0));
    let radius = cyDofRadiusAtDepth(cyDofCompositeSet.depth.Load(int3(pixel, 0)));

    // The pixel centre in the half-resolution layers, and its four bilinear neighbours.
    let position = (float2(pixel) + 0.5) * 0.5 - 0.5;
    let base = int2(floor(position));
    let fraction = position - float2(base);
    let last = int2(cyDof.extent.zw) - 1;

    float3 result = sharp.rgb;
    let defocus = radius > 0.0 ? cyDofDefocus(radius) : 0.0;
    if (defocus > 0.0)
    {
        float3 colour = float3(0.0);
        float weight = 0.0;
        for (uint corner = 0; corner < 4; ++corner)
        {
            let offset = int2(corner & 1, corner >> 1);
            let texel = clamp(base + offset, int2(0), last);
            let bilinear = (offset.x == 0 ? 1.0 - fraction.x : fraction.x) *
                           (offset.y == 0 ? 1.0 - fraction.y : fraction.y);
            if (cyDofCompositeSet.layer.Load(int3(texel, 0)).a > cyDof.lens.w && bilinear > 0.0)
            {
                colour += cyDofCompositeSet.farField.Load(int3(texel, 0)).rgb * bilinear;
                weight += bilinear;
            }
        }
        if (weight > 0.0)
        {
            result = lerp(sharp.rgb, colour / weight, defocus);
        }
    }

    float4 covered = float4(0.0);
    for (uint corner = 0; corner < 4; ++corner)
    {
        let offset = int2(corner & 1, corner >> 1);
        let texel = clamp(base + offset, int2(0), last);
        let bilinear = (offset.x == 0 ? 1.0 - fraction.x : fraction.x) *
                       (offset.y == 0 ? 1.0 - fraction.y : fraction.y);
        covered += cyDofCompositeSet.nearField.Load(int3(texel, 0)) * bilinear;
    }
    if (covered.a > 0.0)
    {
        result = result * (1.0 - covered.a) + covered.rgb;
    }
    cyDofCompositeSet.target[thread.xy] = float4(result, sharp.a);
}
