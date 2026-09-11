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
// The billboard basis is the VIEW basis read out of `cyFrame.relativeToView`: rows 0 and 1 of a
// camera-relative-to-view matrix are the camera's right and up expressed in camera-relative space,
// which is exactly the two vectors a screen-facing quad needs. There is no world-space camera
// anywhere in this file, which is design.md §3's rule and the reason the basis is read rather than
// passed.
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

[[vk::binding(0, 2)]]
public StructuredBuffer<CyParticle> cyParticles;

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

/// `SV_VulkanVertexID` rather than `SV_VertexID`, and the difference is a DEVICE FEATURE.
///
/// Slang lowers `SV_VertexID` to `gl_VertexIndex - gl_BaseVertex`, to match Direct3D's semantics —
/// which declares the SPIR-V `DrawParameters` capability, which a Vulkan device only accepts with
/// `VkPhysicalDeviceVulkan11Features::shaderDrawParameters` enabled. `src/backends/rhi/vulkan/` does
/// not request that feature, so a pipeline built from such a module is refused by validation.
/// `SV_VulkanVertexID` is the raw `VertexIndex`, which is what a draw with no vertex buffer and no
/// base vertex means anyway. Recorded here because the alternative fix is in the backend.
[shader("vertex")]
public CyParticleVertex cyParticleVertex(uint vertexId: SV_VulkanVertexID)
{
    let particle = cyParticles[vertexId / 6];
    let corner = kCorners[vertexId % 6];

    // Rows 0 and 1 of camera-relative-to-view: the camera's right and up, in camera-relative space.
    let right = cyFrame.relativeToViewRow0.xyz;
    let up = cyFrame.relativeToViewRow1.xyz;
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
