// SPDX-License-Identifier: MIT
// Depth of field, dispatch 1 of 5: the half-resolution layer the gathers read.
//
// Each texel covers a 2x2 block of the target. Its circle of confusion is the block's NEAREST
// surface's — the most negative signed radius — so a near silhouette is never thinned by the
// downsample and a focused silhouette in front of a blurred background stays focused, which is
// what keeps the background from gathering it. Its colour is the mean of the block's texels whose
// radius is within a pixel of that one: the surface the radius describes, not a mix of two.
//
//   rgb  the colour, linear HDR
//   a    the signed radius in pixels of the TARGET, clamped to the largest gathered
//
// See dof_gather.slang for how these files are compiled.

#include "dof_common.slang"

struct CyDofSetupSet
{
    Texture2D<float4> source;
    /// A depth texture on Metal (`depth2d`), which is what the frame's depth IS there; SPIR-V
    /// ignores the distinction.
    DepthTexture2D depth;
    [format("rgba16f")] RWTexture2D<float4> layer;
};

[[vk::binding(0, 0)]] ParameterBlock<CyDofSetupSet> cyDofSetupSet;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyDofSetup(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= cyDof.extent.z || thread.y >= cyDof.extent.w)
    {
        return;
    }
    let last = int2(cyDof.extent.xy) - 1;
    float radii[4];
    float3 colours[4];
    float nearest = cyDof.lens.z;
    for (uint corner = 0; corner < 4; ++corner)
    {
        let texel = min(int2(thread.xy) * 2 + int2(corner & 1, corner >> 1), last);
        radii[corner] = cyDofRadiusAtDepth(cyDofSetupSet.depth.Load(int3(texel, 0)));
        colours[corner] = cyDofSetupSet.source.Load(int3(texel, 0)).rgb;
        nearest = min(nearest, radii[corner]);
    }
    float3 colour = float3(0.0);
    float count = 0.0;
    for (uint corner = 0; corner < 4; ++corner)
    {
        if (radii[corner] - nearest < 1.0)
        {
            colour += colours[corner];
            count += 1.0;
        }
    }
    cyDofSetupSet.layer[thread.xy] = float4(colour / count, nearest);
}
