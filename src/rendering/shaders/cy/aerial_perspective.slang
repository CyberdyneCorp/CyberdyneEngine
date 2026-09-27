// SPDX-License-Identifier: MIT
// Aerial perspective, read from the atmosphere's own froxel table. Part of the engine's Slang
// standard library.
//
// `atmosphere-sky-and-clouds` — "Aerial perspective": "Distance attenuation SHALL be produced by the
// atmosphere model — scattering and transmittance over distance — and applied to opaque shading",
// and "Ad-hoc distance fog with independently tuned parameters SHALL NOT be the engine's model of
// distance". So there is no fog colour and no fog density anywhere in this file. What it reads is
// `sky::AerialPerspectiveTable`, integrated on the processor from the same `Atmosphere` and the same
// transmittance and multiple-scattering tables the sky is drawn from, and packed by
// `sky::pack_aerial_perspective()`.
//
// ================================================================================================
// THE LAYOUT, RESTATED FROM `cy/rendering/sky/tables.h`
// ================================================================================================
//
//   word 0   forward.xyz, enabled (1 or 0)
//   word 1   right.xyz,   tan of half the horizontal field of view
//   word 2   up.xyz,      tan of half the vertical field of view
//   word 3   width, height, depth, the slice distribution's exponent
//   word 4   near plane, far plane, radiance scale, 0
//   then     two words per froxel, slice-major then row-major: transmittance, in-scattering
//
// The slices are `rendering-post-processing`'s froxel distribution: slice i's far edge is at
// `lerp(near, far, ((i + 1) / depth) ^ exponent)` metres along the forward axis.
//
// ================================================================================================
// A SURFACE IS INTERPOLATED IN DEPTH, AND THE EYE IS A SLICE
// ================================================================================================
//
// A froxel volume integrated front to back answers "what lies between the eye and this slice's far
// edge". A surface sits somewhere between two far edges, so the sampler interpolates between them,
// and in front of the first slice it interpolates from the eye itself, where nothing has been
// attenuated and nothing added. Without that, a surface a metre away would receive the whole first
// slice's haze. `AerialPerspectiveTable::sample_at()` is this function on the processor, and
// `render.world_aerial_perspective` compares the two.

module aerial_perspective;

/// What the air between the eye and a surface does to it: multiply by `transmittance`, then add
/// `inScattering`, which is already in the frame's radiance units.
public struct CyAerialPerspective
{
    public float3 transmittance;
    public float3 inScattering;
};

/// The table's own bindings: one read-only buffer of float4 words.
public struct CyAerialPerspectiveTable
{
    public StructuredBuffer<float4> words;
};

static const uint kCyAerialHeaderWords = 5;

/// Whether the frame bound a table at all. A frame with aerial perspective off binds the header
/// alone with this word zero, and every surface is then left exactly as it was lit.
public bool cyAerialPerspectiveEnabled(CyAerialPerspectiveTable table)
{
    return table.words[0].w > 0.5;
}

float cyAerialSliceDepth(float4 shape, float4 planes, float slice)
{
    const float t = min(slice + 1.0, shape.z) / shape.z;
    return lerp(planes.x, planes.y, pow(t, max(shape.w, 1.0)));
}

float3 cyAerialFetch(CyAerialPerspectiveTable table, float4 shape, uint slice, uint x, uint y,
                     uint which)
{
    const uint width = uint(shape.x);
    const uint height = uint(shape.y);
    const uint froxel = (slice * height * width) + (y * width) + x;
    return table.words[kCyAerialHeaderWords + (froxel * 2u) + which].xyz;
}

float3 cyAerialPlane(CyAerialPerspectiveTable table, float4 shape, uint slice, float2 texel,
                     uint which)
{
    const uint width = uint(shape.x);
    const uint height = uint(shape.y);
    const float2 clamped = clamp(texel, float2(0.0), float2(shape.x - 1.0, shape.y - 1.0));
    const uint x0 = uint(clamped.x);
    const uint y0 = uint(clamped.y);
    const uint x1 = min(x0 + 1u, width - 1u);
    const uint y1 = min(y0 + 1u, height - 1u);
    const float tx = clamped.x - float(x0);
    const float ty = clamped.y - float(y0);
    const float3 top = lerp(cyAerialFetch(table, shape, slice, x0, y0, which),
                            cyAerialFetch(table, shape, slice, x1, y0, which), tx);
    const float3 bottom = lerp(cyAerialFetch(table, shape, slice, x0, y1, which),
                               cyAerialFetch(table, shape, slice, x1, y1, which), tx);
    return lerp(top, bottom, ty);
}

/// What the air does to a surface `offset` metres from the eye, in the world axes the table was
/// built in. A surface behind the eye, or a table that is off, is returned untouched.
public CyAerialPerspective cyAerialPerspectiveAt(CyAerialPerspectiveTable table, float3 offset)
{
    CyAerialPerspective result;
    result.transmittance = float3(1.0);
    result.inScattering = float3(0.0);

    const float4 forward = table.words[0];
    const float4 right = table.words[1];
    const float4 up = table.words[2];
    const float4 shape = table.words[3];
    const float4 planes = table.words[4];
    const float depth = dot(offset, forward.xyz);
    if (forward.w < 0.5 || depth <= 0.0)
    {
        return result;
    }

    const float ndcX = dot(offset, right.xyz) / (depth * right.w);
    const float ndcY = dot(offset, up.xyz) / (depth * up.w);
    const float2 texel = float2(((ndcX * 0.5 + 0.5) * shape.x) - 0.5,
                                ((ndcY * 0.5 + 0.5) * shape.y) - 0.5);

    // Where the depth falls: before the first slice's far edge the near end is the eye itself.
    const uint last = uint(shape.z) - 1u;
    const float first = cyAerialSliceDepth(shape, planes, 0.0);
    float3 nearT = float3(1.0);
    float3 nearS = float3(0.0);
    uint farSlice = 0u;
    float fraction = saturate(depth / max(first, 1e-6));
    if (depth > first)
    {
        const float span = max(planes.y - planes.x, 1e-6);
        const float normalised = saturate((depth - planes.x) / span);
        const float continuous = pow(normalised, 1.0 / max(shape.w, 1.0)) * shape.z - 1.0;
        const uint slice = min(uint(max(continuous, 0.0)), last);
        nearT = cyAerialPlane(table, shape, slice, texel, 0u);
        nearS = cyAerialPlane(table, shape, slice, texel, 1u);
        farSlice = min(slice + 1u, last);
        const float nearEdge = cyAerialSliceDepth(shape, planes, float(slice));
        const float farEdge = cyAerialSliceDepth(shape, planes, float(slice + 1u));
        fraction = slice == last ? 1.0 : saturate((depth - nearEdge) / max(farEdge - nearEdge, 1e-6));
    }
    const float3 farT = cyAerialPlane(table, shape, farSlice, texel, 0u);
    const float3 farS = cyAerialPlane(table, shape, farSlice, texel, 1u);
    result.transmittance = lerp(nearT, farT, fraction);
    result.inScattering = lerp(nearS, farS, fraction);
    return result;
}

/// A lit surface's radiance as the eye receives it through the air: attenuated, then added to.
public float3 cyApplyAerialPerspective(CyAerialPerspectiveTable table, float3 radiance,
                                       float3 offset)
{
    const CyAerialPerspective air = cyAerialPerspectiveAt(table, offset);
    return radiance * air.transmittance + air.inScattering;
}
