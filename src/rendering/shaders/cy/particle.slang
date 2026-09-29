// Particle sprites, drawn through the frame's own pipeline layer. M8.c task 1b.4.
//
// ================================================================================================
// THE FIRST CONSUMER, AND IT PROVES THE LAYER BY USING IT
// ================================================================================================
//
// `import cy.frame` is the whole argument of this file. The particle pass reads the SAME per-view
// block the opaque pass reads, at the same set and the same binding, through the same descriptor
// sets the recorder already bound — so a particle and a mesh cannot disagree about where the camera
// is, what the projection is, or which space anything is in. A particle renderer with its own view
// constants is a second answer to a question that has one, and it is the arrangement this file
// exists not to have.
//
// It adds exactly one binding of its own: `cyParticles`, at set 2 (pass), which is where the
// descriptor set convention puts a resource that belongs to one pass.
//
// ================================================================================================
// NO VERTEX BUFFER, AND CAMERA-RELATIVE BILLBOARDS
// ================================================================================================
//
// Six vertices per particle derived from `SV_VertexID`: one quad, no index buffer, no vertex
// stream, no per-particle draw call. `vfx-system` requires a GPU-first path and this is the drawing
// half of it — the CPU supplies a buffer of records and issues ONE draw.
//
// The billboard basis is the VIEW basis read out of `cyFrameView.frame.relativeToView`: rows 0 and 1
// of a camera-relative-to-view matrix are the camera's right and up expressed in camera-relative
// space, which is exactly the two vectors a screen-facing quad needs. There is no world-space camera
// anywhere in this file, which is design.md §3's rule and the reason the basis is read rather than
// passed.
//
// `cyFrameView.frame` AND NOT `cyFrame`. `cy/frame.slang` spells the view block `cyFrame` through a
// `#define`, and a macro does not cross an `import`: when the block became a parameter block this
// file stopped compiling for every target, and nothing saw it because `particle_spirv.h` had been
// compiled before the change. `smoke.shader_targets` compiles this file on every smoke run now.
//
// REGENERATING: see cy/frame.slang's header. The entry points are `cyParticleVertex` and
// `cyParticleFragment`.

module particle;

import cy.frame;

/// One particle, as the simulation publishes it. 32 bytes.
///
/// `cy::rendering::particles::ParticleInstance` is this struct's C++ half and the two are asserted
/// equal field by field.
public struct CyParticle
{
    /// xyz: the centre, CAMERA-RELATIVE. w: the half-extent in metres.
    public float4 positionAndSize;
    /// Linear, un-premultiplied. The alpha is what the soft edge is multiplied into.
    public float4 color;
};

public struct CyParticlePassSet
{
    public StructuredBuffer<CyParticle> particles;
};

[[vk::binding(0, 2)]]
public ParameterBlock<CyParticlePassSet> cyParticlePass;

public struct CyParticleVertex
{
    public float4 position : SV_Position;
    public float2 corner : TEXCOORD0;
    public float4 color : TEXCOORD1;
};

/// The six corners of a quad, as two triangles. A static table rather than arithmetic on the vertex
/// id: the winding is what decides whether the sprite survives a cull mode, and a table is the form
/// in which a reader can check it.
static const float2 kCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0),
};

/// `SV_VertexID`, which all three targets accept, for the reason and at the cost
/// `cy/fullscreen.slang`'s `fullscreenVertex` records: the Vulkan device carries
/// `shaderDrawParameters`, and the one draw of this stage starts at vertex zero
/// (`particle_renderer.cpp`), the one base on which SPIR-V and Metal agree about the index.
[shader("vertex")]
public CyParticleVertex cyParticleVertex(uint vertexId: SV_VertexID)
{
    let particle = cyParticlePass.particles[vertexId / 6];
    let corner = kCorners[vertexId % 6];

    // Rows 0 and 1 of camera-relative-to-view: the camera's right and up, in camera-relative space.
    let right = cyFrameView.frame.relativeToViewRow0.xyz;
    let up = cyFrameView.frame.relativeToViewRow1.xyz;
    let centre = particle.positionAndSize.xyz;
    let half = particle.positionAndSize.w;
    let offset = (right * corner.x + up * corner.y) * half;

    CyParticleVertex output;
    output.position = transformToClip(centre + offset);
    output.corner = corner;
    output.color = particle.color;
    return output;
}

[shader("fragment")]
public float4 cyParticleFragment(CyParticleVertex input) : SV_Target
{
    // A soft round sprite rather than a square: squared distance, so there is no square root, and a
    // smooth shoulder so the edge does not alias. Nothing here samples a texture — a particle atlas
    // is a data interface `vfx-system` owns and this file is the compositing half.
    let radiusSquared = dot(input.corner, input.corner);
    let falloff = saturate(1.0 - radiusSquared);
    let alpha = input.color.a * falloff * falloff;
    // PREMULTIPLIED. The pipeline blends `One, OneMinusSourceAlpha`, which is what lets one pipeline
    // composite both an additive spark (alpha 0) and an opaque smoke puff (alpha 1) without a second
    // blend state — and `rendering-post-processing` wants the number reaching the tonemap to be a
    // radiance rather than a colour multiplied twice.
    return float4(input.color.rgb * alpha, alpha);
}
