// SPDX-License-Identifier: MIT
// The temporal upscaler, stage four: the depth and motion the stages after it read, at the OUTPUT
// extent. Depth of field, motion blur and the selection outlines run after the temporal stage — the
// chain's order, `rendering-post-processing` steps 7 and 8 — and so at the extent it reconstructs;
// they read the prepass's depth and motion texel for texel, and those are at the render extent.
//
// Each output pixel takes the render texel whose jittered sample is nearest its centre — the same
// texel the reconstruction centres its neighbourhood on — with no blending: a depth averaged across
// an edge is a surface that is not there. A graphics pass rather than a dispatch because the depth
// is written as DEPTH (`SV_Depth`), into the frame's depth format: motion blur reads a depth
// texture, which on Metal is a different type from a colour one.
//
// OUTPUT: depth, and the motion in normalised screen space pointing at the past.
//
// REGENERATING: see upscale_reconstruct.slang.

struct CyUpscaleResampleSet
{
    DepthTexture2D depth;
    /// Read only with `kCyUpscaleVelocity`.
    Texture2D<float2> velocity;
};

[[vk::binding(0, 0)]] ParameterBlock<CyUpscaleResampleSet> cyUpscaleResampleSet;

// AFTER the set, so Slang gives the set Metal buffer 0 and the push constants buffer 1 — the Metal
// RHI's convention: set N at buffer N, push constants at the set count.
#include "upscale_common.slang"


struct CyUpscaleResampleVertex
{
    float4 position : SV_Position;
};

/// One oversized triangle, from `SV_VertexID` — `cy/fullscreen.slang`'s, without the UV.
[shader("vertex")]
CyUpscaleResampleVertex cyUpscaleResampleVertex(uint vertexId : SV_VertexID)
{
    CyUpscaleResampleVertex output;
    let uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    return output;
}

struct CyUpscaleResampleOutput
{
    float2 velocity : SV_Target0;
    float depth : SV_Depth;
};

[shader("fragment")]
CyUpscaleResampleOutput cyUpscaleResampleFragment(CyUpscaleResampleVertex input)
{
    let pixel = uint2(input.position.xy);
    let uv = cyUpscaleOutputUv(pixel);
    let render = cyUpscaleRenderExtent();
    let texel = cyUpscaleClampTexel(cyUpscaleNearestTexel(uv * float2(render)), render);
    let depth = cyUpscaleResampleSet.depth.Load(int3(texel, 0));
    let hasVelocity = (cyUpscaleFlags() & kCyUpscaleVelocity) != 0;
    CyUpscaleResampleOutput output;
    output.depth = depth;
    output.velocity = (hasVelocity && depth > 0.0)
                          ? cyUpscaleResampleSet.velocity.Load(int3(texel, 0))
                          : cyUpscaleCameraMotion(uv, depth);
    return output;
}
