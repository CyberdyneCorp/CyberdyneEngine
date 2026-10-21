// SPDX-License-Identifier: MIT
// The temporal upscaler's copy of the colour before translucency — the frame's `OpaqueColorCopy`
// stage, declared by the upscaler for its reactive mask. A dispatch rather than a transfer because
// the RHI copies between a texture and a buffer only, and a texture-to-buffer-to-texture round trip
// inside one pass would need a barrier between its halves that only the graph may emit.
//
// OUTPUT: the colour, texel for texel.
//
// REGENERATING: see upscale_reconstruct.slang.

struct CyUpscaleCopyConstants
{
    /// xy: the extent.
    uint4 extent;
};

struct CyUpscaleCopySet
{
    Texture2D<float4> source;
    [format("rgba16f")] RWTexture2D<float4> copy;
};

[[vk::binding(0, 0)]] ParameterBlock<CyUpscaleCopySet> cyUpscaleCopySet;

// After the set: Metal buffer 0 is the set, buffer 1 the push constants (the Metal RHI's
// convention, set N at buffer N and push constants at the set count).
[[vk::push_constant]] CyUpscaleCopyConstants cyUpscaleCopyExtent;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyUpscaleCopy(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= cyUpscaleCopyExtent.extent.x || thread.y >= cyUpscaleCopyExtent.extent.y)
    {
        return;
    }
    cyUpscaleCopySet.copy[thread.xy] = cyUpscaleCopySet.source.Load(int3(int2(thread.xy), 0));
}
