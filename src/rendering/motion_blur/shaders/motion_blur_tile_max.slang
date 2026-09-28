// SPDX-License-Identifier: MIT
// Motion blur, stage one: every tile's longest blur vector. `rendering-post-processing` — "Motion
// blur": "compute per-tile maximum velocity".
//
// One thread a tile, walking the tile's pixels row by row: `max_radius_pixels` squared texels, the
// longest blur vector among them kept — the first on a tie, the order the host reference walks.
// A tile at the right or bottom edge is partial and walks only the pixels the image has.
//
// OUTPUT: rg the longest blur vector in pixels, ba zero.
//
// REGENERATING: `python3 src/rendering/motion_blur/shaders/regenerate.py --slangc <slangc>`; the
// script's docstring carries the invocations.

#include "motion_blur_common.slang"

struct CyMotionBlurTileSet
{
    /// The prepass motion vectors: normalised screen space, pointing at the past.
    Texture2D<float2> velocity;
    /// A depth texture on Metal (`depth2d`), which is what the prepass target IS there.
    DepthTexture2D depth;
    [format("rgba32f")] RWTexture2D<float4> tiles;
};

[[vk::binding(0, 0)]] ParameterBlock<CyMotionBlurTileSet> cyMotionBlurTileSet;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyMotionBlurTileMax(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= cyMotionBlur.control.z || thread.y >= cyMotionBlur.control.w)
    {
        return;
    }
    let edge = cyMotionBlur.control.y;
    let size = uint2(cyMotionBlur.extent.xy);
    let first = thread.xy * edge;
    let last = min(first + edge, size);
    float2 longest = float2(0.0, 0.0);
    for (uint y = first.y; y < last.y; ++y)
    {
        for (uint x = first.x; x < last.x; ++x)
        {
            let pixel = uint2(x, y);
            let velocity = cyMotionBlurTileSet.velocity.Load(int3(int2(pixel), 0));
            let depth = cyMotionBlurTileSet.depth.Load(int3(int2(pixel), 0));
            longest = cyMotionBlurLonger(longest, cyMotionBlurVector(pixel, velocity, depth));
        }
    }
    cyMotionBlurTileSet.tiles[int2(thread.xy)] = float4(longest, 0.0, 0.0);
}
