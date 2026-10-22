// SPDX-License-Identifier: MIT
// Fog of war over the scene: one team's visibility texture applied to every surface the frame
// drew, on linear HDR after the temporal resolve and before depth of field, bloom and exposure.
//
// For each pixel: the surface's world position from the depth (reversed Z; the cleared sky, depth
// zero, is left as it is), the visibility texture sampled there — four bilinear taps half a cell
// apart, averaged, the edge clamped, each channel eased by a smoothstep; a point off the grid is
// never explored — and the colour shaded:
//
//   explored = lerp(luma, colour, saturation) * brightness
//   hidden   = lerp(unexplored colour, explored, explored level)
//   result   = lerp(hidden, colour, visible level)
//
// A fully visible pixel is written as it was read, the same bits, so a frame whose team sees
// everything is the frame without the stage. render.fog_of_war asserts that, and holds the device
// to the host twins of every step here — `sample_visibility` and `shade_fog` in
// src/rendering/fog_of_war/src/visibility_field.cpp, written in the same order.
//
// Compiled by shaders/regenerate.py (SPIR-V 1.5 and MSL) into the checked-in headers.

struct CyFogOfWarSet
{
    /// The colour the post chain has reached, linear HDR.
    Texture2D<float4> source;
    /// The frame's depth at this extent, reversed Z.
    DepthTexture2D depth;
    /// One texel a grid cell: red visible, green explored, as 8-bit unorm.
    Texture2D<float4> visibility;
    /// `fog_of_war::FogOfWarConstants`, word for word — `pack_fog_of_war_constants` is the layout.
    StructuredBuffer<float4> words;
    [format("rgba16f")] RWTexture2D<float4> target;
};

[[vk::binding(0, 0)]] ParameterBlock<CyFogOfWarSet> cyFogOfWarSet;

/// Words 0 to 3: clip to camera-relative position, by column. Word 4: the eye, world metres.
/// Word 5: the grid's origin x and z, one over its cell size. Word 6: explored brightness and
/// saturation. Word 7: the unexplored colour. Word 8: the target's extent, then the grid's.
float4 fogOfWarWord(uint index)
{
    return cyFogOfWarSet.words[index];
}

float fogOfWarSmooth(float value)
{
    let t = clamp(value, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// The texture bilinearly between texel centres at `position`, in texels, the edge clamped.
float2 fogOfWarBilinear(float2 position)
{
    let extent = fogOfWarWord(8);
    let lower = floor(position);
    let fraction = position - lower;
    let last = extent.zw - 1.0;
    let x0 = int(clamp(lower.x, 0.0, last.x));
    let x1 = int(clamp(lower.x + 1.0, 0.0, last.x));
    let z0 = int(clamp(lower.y, 0.0, last.y));
    let z1 = int(clamp(lower.y + 1.0, 0.0, last.y));
    let t00 = cyFogOfWarSet.visibility.Load(int3(x0, z0, 0)).xy;
    let t10 = cyFogOfWarSet.visibility.Load(int3(x1, z0, 0)).xy;
    let t01 = cyFogOfWarSet.visibility.Load(int3(x0, z1, 0)).xy;
    let t11 = cyFogOfWarSet.visibility.Load(int3(x1, z1, 0)).xy;
    let nearRow = lerp(t00, t10, fraction.x);
    let farRow = lerp(t01, t11, fraction.x);
    return lerp(nearRow, farRow, fraction.y);
}

/// `sample_visibility`: x the visible level, y the explored one. The mean of four bilinear taps
/// half a cell apart — a tent two cells wide, which rounds the cells' corners — each eased. A point
/// off the grid is never explored: zero for both, whatever the edge cells hold.
float2 fogOfWarSample(float2 world)
{
    let grid = fogOfWarWord(5);
    let extent = fogOfWarWord(8);
    let cell = (world - grid.xy) * grid.z;
    // `kFogEdgeSlack`: a surface on the edge reconstructs a hair either side of it.
    let slack = 1.0 / 16.0;
    if (cell.x < -slack || cell.y < -slack || cell.x > extent.z + slack || cell.y > extent.w + slack)
    {
        return float2(0.0);
    }
    let position = cell - 0.5;
    var level = float2(0.0);
    for (uint tap = 0; tap < 4; ++tap)
    {
        let offset = float2((tap & 1) != 0 ? 0.5 : -0.5, (tap & 2) != 0 ? 0.5 : -0.5);
        level += fogOfWarBilinear(position + offset);
    }
    level *= 0.25;
    return float2(fogOfWarSmooth(level.x), fogOfWarSmooth(level.y));
}

/// `shade_fog`.
float3 fogOfWarShade(float3 colour, float2 levels)
{
    let style = fogOfWarWord(6);
    let luma = 0.2126 * colour.x + 0.7152 * colour.y + 0.0722 * colour.z;
    let muted = lerp(float3(luma), colour, style.y);
    let explored = muted * style.x;
    let hidden = lerp(fogOfWarWord(7).xyz, explored, levels.y);
    return lerp(hidden, colour, levels.x);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyFogOfWar(uint3 thread : SV_DispatchThreadID)
{
    let extent = fogOfWarWord(8);
    if (float(thread.x) >= extent.x || float(thread.y) >= extent.y)
    {
        return;
    }
    let pixel = int2(thread.xy);
    let colour = cyFogOfWarSet.source.Load(int3(pixel, 0));
    let depth = cyFogOfWarSet.depth.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        cyFogOfWarSet.target[thread.xy] = colour;
        return;
    }
    // The pixel centre in normalised device coordinates, y up, then camera-relative metres.
    let uv = (float2(pixel) + 0.5) / extent.xy;
    let clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    let relative = fogOfWarWord(0) * clip.x + fogOfWarWord(1) * clip.y + fogOfWarWord(2) * clip.z +
                   fogOfWarWord(3) * clip.w;
    let world = relative.xyz / relative.w + fogOfWarWord(4).xyz;
    let levels = fogOfWarSample(world.xz);
    if (levels.x >= 1.0)
    {
        cyFogOfWarSet.target[thread.xy] = colour;
        return;
    }
    cyFogOfWarSet.target[thread.xy] = float4(fogOfWarShade(colour.rgb, levels), colour.a);
}
