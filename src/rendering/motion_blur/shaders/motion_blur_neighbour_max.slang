// SPDX-License-Identifier: MIT
// Motion blur, stage two: the longest tile vector of each tile's 3x3 neighbourhood — "then gather
// along the dominant direction". A pixel can be reached by a streak from any tile within one
// tile's reach, because no blur radius is longer than the tile edge, so this is the direction and
// the reach every pixel of the tile gathers along.
//
// OUTPUT: rg the neighbourhood's longest blur vector in pixels, ba zero.
//
// REGENERATING: see motion_blur_tile_max.slang.

#include "motion_blur_common.slang"

struct CyMotionBlurNeighbourSet
{
    Texture2D<float4> tiles;
    [format("rgba32f")] RWTexture2D<float4> neighbours;
};

[[vk::binding(0, 0)]] ParameterBlock<CyMotionBlurNeighbourSet> cyMotionBlurNeighbourSet;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyMotionBlurNeighbourMax(uint3 thread : SV_DispatchThreadID)
{
    let across = int(cyMotionBlur.control.z);
    let down = int(cyMotionBlur.control.w);
    let tile = int2(thread.xy);
    if (tile.x >= across || tile.y >= down)
    {
        return;
    }
    float2 longest = float2(0.0, 0.0);
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            let neighbour = tile + int2(dx, dy);
            if (neighbour.x < 0 || neighbour.y < 0 || neighbour.x >= across || neighbour.y >= down)
            {
                continue;
            }
            let candidate = cyMotionBlurNeighbourSet.tiles.Load(int3(neighbour, 0)).xy;
            longest = cyMotionBlurLonger(longest, candidate);
        }
    }
    cyMotionBlurNeighbourSet.neighbours[tile] = float4(longest, 0.0, 0.0);
}
