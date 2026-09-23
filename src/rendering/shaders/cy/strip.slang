// SPDX-License-Identifier: MIT
// Particle strips — ribbons, trails and beams — drawn through the frame's own pipeline layer.
// M11.c task 6.3.
//
// ================================================================================================
// WHAT THIS IS, AND WHY IT IS `particle.slang`'s SIBLING RATHER THAN A SECOND ARRANGEMENT
// ================================================================================================
//
// `vfx-system`'s `publish_ribbons`, `publish_trails` and `publish_beams` derive a strip's vertices
// from particle state — positions in strip order, a half-width, a radiance, where along the strip
// each vertex is, and which strip it belongs to. Until this file nothing drew them. It draws all
// three kinds, because all three publish the same thing: an ordered run of vertices whose strip
// identifier says where one strip ends and the next begins.
//
// It is built exactly the way `particle.slang` is and for the same reasons: `import cy.frame`, so a
// strip and a mesh cannot disagree about the projection; one binding of its own at set 2; no vertex
// buffer and no index buffer; ONE DRAW for every strip in the frame.
//
// ================================================================================================
// ONE QUAD A SEGMENT, AND A SEGMENT THAT CROSSES TWO STRIPS COLLAPSES
// ================================================================================================
//
// Six vertices for each pair of neighbouring records. When the pair belongs to two different strips
// — the end of one trail and the head of the next — all six collapse onto one point and the two
// triangles have no area, so nothing is rasterised. That is the renderer's half of `vfx-system`'s
// "a ribbon killed mid-chain SHALL TERMINATE CLEANLY rather than connecting across the gap": the
// publication already split the chain into two strips, and this is where the split stays split.
//
// The strip faces the camera. The camera is at the origin of camera-relative space, so the vector
// from a vertex to the eye is `-position`, and the side a strip widens along is the cross product of
// its tangent with that. There is no world-space camera in this file, which is design.md §3's rule.
//
// REGENERATING: see cy/frame.slang's header. The entry points are `cyStripVertex` and
// `cyStripFragment`, and `src/rendering/particles/shaders/embed_spirv.py` writes the header.

module strip;

import cy.frame;

/// One strip vertex, as a publication derives it. 48 bytes.
///
/// `cy::rendering::particles::StripVertex` is this struct's C++ half and the two are asserted equal
/// field by field.
public struct CyStripVertex
{
    /// xyz: CAMERA-RELATIVE. w: the half-width in metres; zero draws nothing, which is how the tail
    /// of a tapering trail ends.
    public float4 positionAndWidth;
    /// Linear, un-premultiplied radiance. The alpha is what the fade along the strip and the soft
    /// edge across it are multiplied into.
    public float4 color;
    /// x: where along its strip this vertex is, in [0, 1]. y: the strip, as the bits of a uint.
    /// zw: unused.
    public float4 alongAndStrip;
};

[[vk::binding(0, 2)]]
public StructuredBuffer<CyStripVertex> cyStrips;

public struct CyStripOutput
{
    public float4 position : SV_Position;
    /// x: along the strip. y: across it, in [-1, 1].
    public float2 coordinates : TEXCOORD0;
    public float4 color : TEXCOORD1;
};

/// The six corners of a segment's quad: x selects the end (0 is this record, 1 the next), y the
/// side. A table for the reason `particle.slang` gives — the winding is checkable in this form.
static const float2 kCorners[6] = {
    float2(0.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(0.0, -1.0), float2(1.0, 1.0),  float2(0.0, 1.0),
};

/// The side a strip widens along at `position`: perpendicular to the tangent AND to the line of
/// sight, so the ribbon is always seen face on. Zero where the two are parallel, which collapses a
/// segment pointed straight at the camera rather than drawing it edge on as a line of noise.
float3 sideAt(float3 position, float3 tangent)
{
    let side = cross(tangent, -position);
    let lengthSquared = dot(side, side);
    return lengthSquared > 1.0e-12 ? side * rsqrt(lengthSquared) : float3(0.0, 0.0, 0.0);
}

/// `SV_VulkanVertexID` for the reason `particle.slang` records: `SV_VertexID` lowers to a
/// DrawParameters capability the Vulkan backend does not request.
[shader("vertex")]
public CyStripOutput cyStripVertex(uint vertexId: SV_VulkanVertexID)
{
    let segment = vertexId / 6;
    let corner = kCorners[vertexId % 6];
    let from = cyStrips[segment];
    let to = cyStrips[segment + 1];

    let chosen = corner.x > 0.5 ? to : from;
    let position = chosen.positionAndWidth.xyz;

    CyStripOutput output;
    output.coordinates = float2(chosen.alongAndStrip.x, corner.y);
    output.color = chosen.color;

    // TWO STRIPS: collapse. Every vertex of this segment lands on `from`, so both triangles have no
    // area and the gap between one strip's end and the next one's head is never drawn.
    let sameStrip = asuint(from.alongAndStrip.y) == asuint(to.alongAndStrip.y);
    let tangent = to.positionAndWidth.xyz - from.positionAndWidth.xyz;
    if (!sameStrip || dot(tangent, tangent) <= 1.0e-12)
    {
        output.position = transformToClip(from.positionAndWidth.xyz);
        return output;
    }

    let side = sideAt(position, tangent);
    output.position = transformToClip(position + side * (corner.y * chosen.positionAndWidth.w));
    return output;
}

[shader("fragment")]
public float4 cyStripFragment(CyStripOutput input) : SV_Target
{
    // A soft edge across the strip, and a fade to nothing at its tail: the same smooth shoulder the
    // sprite uses across its radius, so a trail leaving a mote reads as the mote's own light.
    let across = saturate(1.0 - input.coordinates.y * input.coordinates.y);
    let fade = saturate(1.0 - input.coordinates.x);
    let alpha = input.color.a * across * fade;
    // ADDITIVE, through the premultiplied blend: the colour is premultiplied and the alpha written is
    // ZERO, so `One, OneMinusSourceAlpha` takes nothing away from what is behind. A trail is the
    // light a mote left along its path, not matter in front of the scene, and the first capture
    // measured what happens otherwise: a mote cools to 1 800 of radiance against a sky of about
    // 5 000 in blue (`embers.h`), so an occluding trail behind a cooling mote photographed as a
    // DARK streak across the sky — soot again, the failure `embers.h` already records for sprites.
    return float4(input.color.rgb * alpha, 0.0);
}
