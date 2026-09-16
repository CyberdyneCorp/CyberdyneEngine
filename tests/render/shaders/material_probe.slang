// The material probe: the smallest shader that can answer "does a texture the engine uploaded reach
// a material program" — and it answers it through the binding path the standard library declares
// and nothing else.
//
// COMPILED OFFLINE AND CHECKED IN, exactly as `conventions.slang` and `field_probe.slang` beside it
// are, and for the same reason: `CY_SHADER_SLANG` is off in Profile and Shipping, and a suite that
// needed a shader compiler would not run on the machines that most need to run it. `embed_spirv.py`
// beside this file regenerates the header; the invocation is at the bottom of this comment.
//
// ================================================================================================
// IT DECLARES NOTHING OF ITS OWN IN SET 0, AND THAT IS THE POINT
// ================================================================================================
//
// `import cy.material` is where `cyMaterialTextures[]` at (set 0, binding 1) and `cyMaterialSampler`
// at (set 0, binding 2) come from. This file restates neither. So the set this probe's pipeline
// layout names is `rhi::Device::global_texture_table_layout()` — the device's OWN global table,
// which until M11.c reached no pipeline layout and no command buffer in the whole tree — and the
// agreement between the two is checked by the Slang compiler and the Vulkan validation layers
// rather than by a comment. A probe that declared its own texture array would prove that a texture
// can be sampled and prove nothing about the table.
//
// ================================================================================================
// AND IT SAMPLES AT THE IMPLICIT LEVEL
// ================================================================================================
//
// `cyMaterialSampleTexture` and not `cyMaterialSampleTextureLevel(..., 0.0)`. The explicit form
// reads mip 0 whatever the footprint, which is how a frame with eight of nine cooked levels never
// uploaded came out BYTE-IDENTICAL in M11.c's spike. A probe drawn at the texture's own size
// samples level 0 either way — but a probe written with the explicit form would not notice the day
// a chain stopped arriving, and this one is drawn through a pixel stage where the implicit form is
// the honest call.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc material_probe.slang -I ../../../src/rendering/shaders -target spirv -profile spirv_1_5 \
//          -entry materialProbeVertex   -stage vertex   -o material_probe_vertex.spv
//   slangc material_probe.slang -I ../../../src/rendering/shaders -target spirv -profile spirv_1_5 \
//          -entry materialProbeFragment -stage fragment -o material_probe_fragment.spv
//   python3 embed_spirv.py --source material_probe.slang material_probe_spirv.h \
//          kMaterialProbeVertexSpirv=material_probe_vertex.spv \
//          kMaterialProbeFragmentSpirv=material_probe_fragment.spv
//   clang-format -i material_probe_spirv.h

import cy.material;

/// Which slot of the global table this draw reads. A push constant and not a uniform block, so the
/// probe needs no set of its own at all: the ONLY descriptor set this pipeline has is the device's
/// table.
struct MaterialProbePush
{
    uint slot;
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::push_constant]] MaterialProbePush push;

struct MaterialProbeVertex
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct MaterialProbeVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

/// One triangle covering the target, with the texture's whole [0,1] square across the visible
/// square of it.
///
/// A VERTEX BUFFER AND NOT `SV_VertexID`, which would be the shorter way to write it. Slang lowers
/// that semantic to `VertexIndex - BaseVertex`, and the subtraction declares the SPIR-V
/// `DrawParameters` capability: a device created without `shaderDrawParameters` — which this
/// engine's is — refuses the module by name. Three vertices of position and texture coordinate cost
/// 48 bytes and no device feature.
///
/// THE Y FLIP IS THE ENGINE'S, and the caller's three vertices honour it.
/// `VulkanCommandBuffer::set_viewport` gives the viewport a NEGATIVE height because "Vulkan's clip
/// space has +Y down and the engine is Y-up", so clip +Y is the TOP of the target — and a texture's
/// v = 0 is its top row. A probe that paired v = 0 with clip -1 would read a perfectly plausible
/// upside-down texture and call the binding proved.
[shader("vertex")]
MaterialProbeVaryings materialProbeVertex(MaterialProbeVertex vertex)
{
    MaterialProbeVaryings varyings;
    varyings.uv = vertex.uv;
    varyings.position = float4(vertex.position, 0.0, 1.0);
    return varyings;
}

[shader("fragment")]
float4 materialProbeFragment(MaterialProbeVaryings varyings) : SV_Target
{
    return cyMaterialSampleTexture(push.slot, varyings.uv);
}
