// SPDX-License-Identifier: MIT
// Depth of field, dispatches 2 and 3 of 5: how far the near field reaches.
//
// A near-field blur spreads OUTWARD from its surface — over the focused pixels beside it, which
// have no blur of their own and so no reason to gather. Each focused pixel must therefore know
// how far the nearest out-of-focus surface around it can reach. `cyDofTiles` takes the largest
// near-field radius (and the largest far one) in each tile of the half-resolution layer; tiles are
// as wide as the largest radius gathered, so `cyDofDilate`'s 3x3 of tiles holds every texel whose
// blur can reach a pixel of the centre tile.
//
//   x  the largest near-field radius in the tile (or its 3x3 neighbourhood), pixels of the target
//   y  the largest far-field radius, likewise
//
// See dof_gather.slang for how these files are compiled.

#include "dof_common.slang"

struct CyDofTilesSet
{
    Texture2D<float4> source;
    [format("rgba16f")] RWTexture2D<float4> tiles;
};

[[vk::binding(0, 0)]] ParameterBlock<CyDofTilesSet> cyDofTilesSet;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyDofTiles(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= cyDof.tiles.y || thread.y >= cyDof.tiles.z)
    {
        return;
    }
    let edge = cyDof.tiles.x;
    let origin = thread.xy * edge;
    let end = min(origin + edge, cyDof.extent.zw);
    float nearReach = 0.0;
    float farReach = 0.0;
    for (uint y = origin.y; y < end.y; ++y)
    {
        for (uint x = origin.x; x < end.x; ++x)
        {
            let radius = cyDofTilesSet.source.Load(int3(int(x), int(y), 0)).a;
            nearReach = max(nearReach, -radius);
            farReach = max(farReach, radius);
        }
    }
    cyDofTilesSet.tiles[thread.xy] = float4(nearReach, farReach, 0.0, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyDofDilate(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= cyDof.tiles.y || thread.y >= cyDof.tiles.z)
    {
        return;
    }
    let last = int2(cyDof.tiles.yz) - 1;
    float2 reach = float2(0.0);
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            let tile = clamp(int2(thread.xy) + int2(dx, dy), int2(0), last);
            reach = max(reach, cyDofTilesSet.source.Load(int3(tile, 0)).xy);
        }
    }
    cyDofTilesSet.tiles[thread.xy] = float4(reach, 0.0, 0.0);
}
