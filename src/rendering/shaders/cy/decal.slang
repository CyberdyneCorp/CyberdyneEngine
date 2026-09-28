// SPDX-License-Identifier: MIT
// Projected box decals, applied to a surface before it is lit. Part of the engine's Slang standard
// library.
//
// `rendering-lighting-and-shadows` — "Decals": "projected oriented boxes writing into albedo,
// normal, roughness, metallic, and emission BEFORE LIGHTING, blended by per-channel weights", with
// "angle-based fade against the receiving surface normal, distance fade, a sort order, a layer
// mask, and normal blending that preserves the receiver's detail".
//
// ================================================================================================
// A DECAL CHANGES THE SURFACE, AND THE LIGHT LOOP THEN LIGHTS WHAT IT CHANGED
// ================================================================================================
//
// Nothing here adds colour to a pixel. Every function takes a `CyDecalSurface` — the receiver's
// albedo, roughness, metallic, emission and shading normal — and returns it modified, and the
// caller hands the result to its light loop. So a scorch mark in the sun is lit by the sun, in a
// shadow it is as dark as the floor around it, and ambient occlusion darkens it as it darkens the
// floor: the three are properties of the receiver's lighting and the decal inherits them by being
// applied first. A decal composited over the lit colour would glow in every shadow it crossed.
//
// ================================================================================================
// THE TABLE
// ================================================================================================
//
// `cy::rendering::decals::pack_decal_table` writes it and `decal_table.h` is the layout's other
// half; the offsets below are asserted there. Everything is a 32-bit WORD — floats as their bits —
// and a source hands words out by index (`ICyDecalSource::word`), so the same functions read a
// table from a texture in the frame's bindless table or from a storage buffer.
//
//   header (24 words)
//     0 magic 'CYDC'     1 version       2 decal count      3 words per decal
//     4 first record     5 lists in table (0 or 1)          6 first list header   7 first index
//     8..10 cluster grid dimensions      11 max per cluster
//     12 slice scale     13 slice bias   14 near plane      15 far plane
//     16 extent width    17 extent height
//     18..21 the row that takes a RELATIVE position to positive view depth
//     22, 23 reserved
//   records, in APPLICATION ORDER (36 words each; rank r is the r-th decal applied)
//     0..2  centre, relative        3  cosine of the fade angle
//     4..6  axis x / half-extent x  7  albedo weight
//     8..10 axis y / half-extent y  11 roughness weight
//     12..14 axis z / half-extent z 15 normal weight
//     16..18 albedo                 19 roughness
//     20..22 emission               23 metallic
//     24 fade start  25 fade end    26 metallic weight  27 emission weight
//     28 channels    29 shape       30 shape parameter a 31 shape parameter b
//     32 mask texture slot          33 relief, metres   34 edge softness      35 seed
//   then, when word 5 is 1, one (offset, count) pair per cluster — the offset a table word, not
//   relative to word 7 — and the ranks those name
//
// The axes are stored DIVIDED by their half-extents, so a position's coordinates in the box are
// three dot products and "inside" is all three in [-1, 1].

module decal;

import cy.cluster;
import cy.noise;

/// Where a table's words come from, and how a mask texture is read. The frame reads both through
/// its bindless texture table; a sample with a storage buffer and no table implements `mask` as 1.
public interface ICyDecalSource
{
    /// The table's word `index`.
    uint word(uint index);
    /// The mask texture in bindless slot `slot` at `uv`: coverage in alpha.
    float mask(uint slot, float2 uv);
};

/// The receiver as the decal sees it: where it is, which way it faces, how far from the eye, and
/// which channels it belongs to.
public struct CyDecalReceiver
{
    /// In the table's relative space — the frame's camera-relative space.
    public float3 relativePosition;
    /// The GEOMETRIC normal, unit length: what the angle fade compares with the projection axis.
    public float3 geometricNormal;
    /// Metres from the eye, for the distance fade.
    public float eyeDistance;
    /// The receiver's channel mask; a decal applies only where its own channels intersect it.
    public uint channels;
};

/// What a decal writes into. The caller fills it from its own surface and reads it back before
/// lighting.
public struct CyDecalSurface
{
    public float3 albedo;
    public float roughness;
    public float metallic;
    public float3 emission;
    /// The shading normal, unit length.
    public float3 normal;
};

public static const uint kCyDecalMagic = 0x43594443u;  // 'CYDC'
public static const uint kCyDecalHeaderWords = 24u;
public static const uint kCyDecalRecordWords = 36u;
/// The texture a frame reads a table from is this many texels wide: one word per Rgba8 texel.
public static const uint kCyDecalTableWidth = 1024u;
/// `cy::rendering::decals::DecalShape`'s values.
public static const uint kCyDecalShapeBox = 0u;
public static const uint kCyDecalShapeSplat = 1u;
public static const uint kCyDecalShapeRing = 2u;
public static const uint kCyDecalShapePatches = 3u;
/// "No mask texture": `kCyNoMaterialTexture`, the bindless table's own sentinel.
public static const uint kCyDecalNoMask = 0xFFFFFFFFu;

float wordFloat<S : ICyDecalSource>(S source, uint index)
{
    return asfloat(source.word(index));
}

float3 wordFloat3<S : ICyDecalSource>(S source, uint index)
{
    return float3(asfloat(source.word(index)), asfloat(source.word(index + 1u)),
                  asfloat(source.word(index + 2u)));
}

/// One word out of an Rgba8Unorm texel, as `DecalTableTexture` uploads it: byte 0 in red. Sampled
/// at a texel centre the filter returns the texel, and the rounding undoes the 1/255 scale exactly.
public uint cyDecalWordFromTexel(float4 texel)
{
    let bytes = uint4(round(saturate(texel) * 255.0));
    return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}

/// The texture coordinate of word `index`'s texel in a table `rows` texels tall.
public float2 cyDecalWordUv(uint index, uint rows)
{
    let texel = float2(float(index % kCyDecalTableWidth), float(index / kCyDecalTableWidth));
    return (texel + 0.5) / float2(float(kCyDecalTableWidth), float(max(rows, 1u)));
}

/// How many decals the table holds; zero for a table that is not one.
public uint cyDecalCount<S : ICyDecalSource>(S source)
{
    return source.word(0u) == kCyDecalMagic ? source.word(2u) : 0u;
}

// --- Shapes ---------------------------------------------------------------------------------
//
// The coverage of a decal over its own [0, 1] square. AUTHORED, not measured: a scorch mark's
// outline and a moss patch's are what an artist would paint into a mask, and these are the
// procedural stand-ins for one, each with two parameters a material states. A mask texture, when
// the material names one, multiplies whatever the shape gives.

float decalFbm(float2 uv, float frequency, uint seed)
{
    let offset = float(seed % 4096u) * 1.618;
    var sum = 0.0;
    var amplitude = 0.5;
    var scale = frequency;
    for (uint octave = 0u; octave < 3u; ++octave)
    {
        sum += valueNoise(float3(uv * scale, offset)) * amplitude;
        amplitude *= 0.5;
        scale *= 2.03;
    }
    return sum / 0.875;
}

/// `a` and `b` per shape:
///   Box      —, —              full coverage: the whole box
///   Splat    noise frequency, breakup   a burn: a disc whose rim the noise breaks up
///   Ring     radius, half width         a marker: a band at `a` of the half-extent
///   Patches  noise frequency, threshold growth: noise above `b`, gone before the box's edge
public float cyDecalShapeCoverage(uint shape, float2 uv, float a, float b, uint seed)
{
    let radius = length(uv - 0.5) * 2.0;
    if (shape == kCyDecalShapeSplat)
    {
        let breakup = (decalFbm(uv, a, seed) - 0.5) * b;
        return 1.0 - smoothstep(0.45, 0.95, radius + breakup);
    }
    if (shape == kCyDecalShapeRing)
    {
        let band = abs(radius - a);
        return 1.0 - smoothstep(b * 0.7, b, band);
    }
    if (shape == kCyDecalShapePatches)
    {
        let growth = smoothstep(b, b + 0.12, decalFbm(uv, a, seed));
        return growth * (1.0 - smoothstep(0.75, 1.0, radius));
    }
    return 1.0;
}

// --- One decal ------------------------------------------------------------------------------

/// The angle fade, `cy::rendering::decal_angle_fade` transcribed: 1 facing the projector, 0 at and
/// past the fade angle, smooth between, and 0 on a surface facing away.
public float cyDecalAngleFade(float cosine, float limit)
{
    if (cosine <= 0.0 || cosine <= limit)
    {
        return 0.0;
    }
    let t = (cosine - limit) / max(1.0 - limit, 1.0e-4);
    return t * t * (3.0 - 2.0 * t);
}

/// The distance fade, `cy::rendering::decal_distance_fade` transcribed.
public float cyDecalDistanceFade(float metres, float start, float end)
{
    let farEdge = max(end, start + 1.0e-4);
    if (metres <= start)
    {
        return 1.0;
    }
    if (metres >= farEdge)
    {
        return 0.0;
    }
    let t = 1.0 - ((metres - start) / (farEdge - start));
    return t * t * (3.0 - 2.0 * t);
}

/// Apply the decal of rank `rank` to `surface`, or leave it untouched when the receiver is outside
/// the box, in another channel, too steep or too far.
public void cyApplyDecal<S : ICyDecalSource>(S source, uint rank, CyDecalReceiver receiver,
                                             inout CyDecalSurface surface)
{
    let base = source.word(4u) + rank * kCyDecalRecordWords;
    if ((source.word(base + 28u) & receiver.channels) == 0u)
    {
        return;
    }
    let axisX = wordFloat3(source, base + 4u);
    let axisY = wordFloat3(source, base + 8u);
    let axisZ = wordFloat3(source, base + 12u);
    let offset = receiver.relativePosition - wordFloat3(source, base);
    let local = float3(dot(offset, axisX), dot(offset, axisY), dot(offset, axisZ));
    // OUTSIDE THE BOX IS MOST OF THE WORLD, and it is the only test that decides which pixels a
    // decal can change at all.
    if (any(abs(local) > 1.0))
    {
        return;
    }

    let projection = normalize(axisZ);
    var weight =
        cyDecalAngleFade(dot(receiver.geometricNormal, projection), wordFloat(source, base + 3u));
    weight *= cyDecalDistanceFade(receiver.eyeDistance, wordFloat(source, base + 24u),
                                  wordFloat(source, base + 25u));
    let softness = wordFloat(source, base + 34u);
    if (softness > 0.0)
    {
        weight *= 1.0 - smoothstep(1.0 - softness, 1.0, abs(local.z));
    }
    if (weight <= 0.0)
    {
        return;
    }

    let uv = local.xy * 0.5 + 0.5;
    let shape = source.word(base + 29u);
    let a = wordFloat(source, base + 30u);
    let b = wordFloat(source, base + 31u);
    let seed = source.word(base + 35u);
    let maskSlot = source.word(base + 32u);
    let maskAt = (maskSlot == kCyDecalNoMask) ? 1.0 : source.mask(maskSlot, uv);
    let coverage = cyDecalShapeCoverage(shape, uv, a, b, seed) * maskAt;
    let amount = weight * coverage;
    if (amount <= 0.0)
    {
        return;
    }

    let albedo = wordFloat3(source, base + 16u);
    let emission = wordFloat3(source, base + 20u);
    surface.albedo = lerp(surface.albedo, albedo, amount * wordFloat(source, base + 7u));
    surface.roughness = lerp(surface.roughness, wordFloat(source, base + 19u),
                             amount * wordFloat(source, base + 11u));
    surface.metallic = lerp(surface.metallic, wordFloat(source, base + 23u),
                            amount * wordFloat(source, base + 26u));
    surface.emission = lerp(surface.emission, emission, amount * wordFloat(source, base + 27u));

    // THE NORMAL, BY SURFACE GRADIENT — the receiver's normal is TILTED by the decal's relief and
    // never replaced, which is "normal blending that preserves the receiver's detail". The decal's
    // height is its coverage times `relief` metres; its gradient in the decal's plane is taken by
    // central differences in uv and carried to metres by the axes' own scale (`uv` moves by
    // |axis| / 2 per metre). Projected onto the receiver's tangent plane and subtracted, it bends
    // the normal the way a heightfield of that relief would.
    let relief = wordFloat(source, base + 33u) * wordFloat(source, base + 15u) * weight;
    if (relief > 0.0)
    {
        let delta = 1.0 / 256.0;
        let du = cyDecalShapeCoverage(shape, uv + float2(delta, 0.0), a, b, seed) -
                 cyDecalShapeCoverage(shape, uv - float2(delta, 0.0), a, b, seed);
        let dv = cyDecalShapeCoverage(shape, uv + float2(0.0, delta), a, b, seed) -
                 cyDecalShapeCoverage(shape, uv - float2(0.0, delta), a, b, seed);
        let perUvX = length(axisX) * 0.5;
        let perUvY = length(axisY) * 0.5;
        let gradient = relief * ((du / (2.0 * delta)) * perUvX * normalize(axisX) +
                                 (dv / (2.0 * delta)) * perUvY * normalize(axisY));
        let tangential = gradient - surface.normal * dot(surface.normal, gradient);
        surface.normal = normalize(surface.normal - tangential);
    }
}

// --- Lists ----------------------------------------------------------------------------------

/// The cluster a receiver falls in, by the grid the TABLE carries — for a shader with no cluster
/// buffers of its own. Returns the (offset, count) of its decal list, or a count of zero when the
/// table carries no lists.
public uint2 cyDecalTableList<S : ICyDecalSource>(S source, float3 relativePosition, float2 pixel)
{
    if (source.word(5u) == 0u)
    {
        return uint2(0u, 0u);
    }
    ClusterGrid grid;
    grid.dimensions = uint3(source.word(8u), source.word(9u), source.word(10u));
    grid.maxLightsPerCluster = source.word(11u);
    grid.sliceScale = wordFloat(source, 12u);
    grid.sliceBias = wordFloat(source, 13u);
    grid.nearPlane = wordFloat(source, 14u);
    grid.farPlane = wordFloat(source, 15u);
    if (grid.dimensions.z == 0u)
    {
        return uint2(0u, 0u);
    }
    let extent = uint2(source.word(16u), source.word(17u));
    let row = float4(wordFloat3(source, 18u), wordFloat(source, 21u));
    let depth = dot(row.xyz, relativePosition) + row.w;
    let coord = clusterCoordOf(grid, uint2(pixel), extent, depth);
    let cluster = clusterIndexOf(grid, coord);
    let header = source.word(6u) + cluster * 2u;
    return uint2(source.word(header), source.word(header + 1u));
}

/// The rank a table-carried list holds at table word `index`: a list's offset is already into the
/// table (word 7 is only where the first list starts), so nothing is added to it here.
public uint cyDecalTableListEntry<S : ICyDecalSource>(S source, uint index)
{
    return source.word(index);
}

/// Every decal in the receiver's cluster, in application order, through the lists the table
/// carries. The whole of what a shader with no cluster buffers needs.
public void cyApplyTableDecals<S : ICyDecalSource>(S source, CyDecalReceiver receiver, float2 pixel,
                                                   inout CyDecalSurface surface)
{
    let count = cyDecalCount(source);
    if (count == 0u)
    {
        return;
    }
    let list = cyDecalTableList(source, receiver.relativePosition, pixel);
    for (uint slot = 0u; slot < list.y; ++slot)
    {
        let rank = cyDecalTableListEntry(source, list.x + slot);
        if (rank < count)
        {
            cyApplyDecal(source, rank, receiver, surface);
        }
    }
}

/// Every decal in the table, in application order, with no cluster lookup at all. The declared
/// fallback — and the reference a clustered frame is compared against, since a cluster list that
/// dropped a decal it should have held changes the picture and this does not.
public void cyApplyAllDecals<S : ICyDecalSource>(S source, CyDecalReceiver receiver,
                                                 inout CyDecalSurface surface)
{
    let count = cyDecalCount(source);
    for (uint rank = 0u; rank < count; ++rank)
    {
        cyApplyDecal(source, rank, receiver, surface);
    }
}
