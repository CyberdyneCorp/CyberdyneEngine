// The frame's own shaders: what `src/rendering/pipeline/` records into `FrameAssembly`'s passes.
// M8.c task 1b.1.
//
// ================================================================================================
// WHY THIS FILE EXISTS
// ================================================================================================
//
// M8.b assembled a frame out of the renderer's own parts and stopped one layer short:
// `FrameAssembly` hands each pass's record callback to its CALLER and every caller in the tree
// supplied none. So the frame was declared, compiled, barriered and executed — and recorded nothing.
// This module is the shading half of the layer that closes that: the entry points a pass binds, and
// nothing else.
//
// It is CONTENT, not code: the compiled modules are staged next to the standard library and the
// pipeline layer consumes SPIR-V. Everything below imports the standard library rather than
// restating it — `cy.brdf`, `cy.light`, `cy.cluster`, `cy.material`, `cy.packing` — which is what
// makes "the engine supplies a shader standard library" a fact about a shipped frame rather than a
// directory of unread modules.
//
// ================================================================================================
// FOUR EXPLICIT ROWS INSTEAD OF float4x4, FOR THE REASON samples/03-first-light GIVES
// ================================================================================================
//
// A matrix in a constant block has a layout that is a property of the compiler's flags rather than
// of this file, and `mul(M, v)` against `mul(v, M)` silently transposes the result. Rows and dot
// products have no convention for the two sides to disagree about. `cy.view`'s `CyViewData` is the
// engine's per-view block and declares `float4x4`; this module deliberately does NOT import it, so
// that the frame's correctness does not depend on which matrix-layout flag the shader cook ran
// with. tests/render/shaders/conventions.slang and samples/03-first-light/shaders/ made the same
// decision for the same reason.
//
// ================================================================================================
// THE DESCRIPTOR SET CONVENTION IS THE ENGINE'S, NOT THIS FILE'S
// ================================================================================================
//
// `cy/backends/shader/reflection.h`: set 0 global, set 1 view, set 2 pass, set 3 draw.
//
//   set 0, binding 0   cyGlobals            imported from cy.globals — time, delta, exposure, wind
//   set 0, binding 1   cyMaterialTextures[] imported from cy.material — the global texture table
//   set 0, binding 2   cyMaterialSampler    imported from cy.material — its one sampler
//   set 1, binding 0   cyFrame              this frame's view: rows, cluster grid, counts, offsets
//   set 1, binding 1   cyLights             `GpuLight`, which IS cy.light's `Light`, field for field
//   set 1, binding 2   cyClusterHeaders     `ClusterHeader{offset, count}` per (cluster, type)
//   set 1, binding 3   cyClusterIndices     the concatenated per-cluster lists
//   set 1, binding 4   cyDrawInstances      `GpuDrawInstance`, indexed by the draw's push constant
//   set 1, binding 5   cyInstances          the model-to-camera-relative rows, per GPU scene slot
//   set 1, binding 6   cyMaterialWords      the GPU material table, as words
//
// The per-draw index is a PUSH CONSTANT rather than a fourth set: it is one word, it changes per
// draw, and `rhi-and-render-graph`'s portability limit is 128 bytes — spending four of them here
// costs no descriptor traffic at all.
//
// ================================================================================================
// THE MATERIAL TABLE IS READ BY OFFSET, AND THE OFFSETS ARE DERIVED
// ================================================================================================
//
// M7's closing gate: "the material compiler fills a GPU material table nothing draws with". The
// opaque fragment shader below reads base colour, roughness, metallic and emission OUT OF THAT
// TABLE. It does not know where they are: `cyFrame.materialOffsets` carries the four word offsets,
// and the pipeline layer resolves them from the `MaterialProgram` the table was described with. A
// hardcoded offset here would be correct on the day it was written and wrong on a later day nobody
// can name, which is the same argument that made M1's reflection generated.
//
// ================================================================================================
// AND THE TEXTURE SLOTS ARE READ THE SAME WAY — M11.c task 3.7's last mile
// ================================================================================================
//
// Until this change `surfaceOf()` read FOUR CONSTANTS and sampled nothing, so every object in a
// frame the engine assembled was a flat colour whatever its material was cooked with. The pieces
// were all built and none of them met: `cy/material.slang` declared `cyMaterialTextures[]` at
// (set 0, binding 1), `rhi::Device::global_texture_table()` filled it, and
// `MaterialTextureTable` made cooked pixels resident in it — but this program's set 0 carried
// `cy/globals.slang`'s block at binding 0 and nothing else, AND A PIPELINE BINDS ONE SET PER
// INDEX. The set 0 this module is compiled against now carries BOTH, which is the whole of what
// was missing; `frame_pipelines.h` builds that layout and `frame_bindings.cpp` writes it.
//
// `cyFrame.materialTextures` carries the slot WORD OFFSETS, for the same reason `materialOffsets`
// carries the constant ones and with the same sentinel discipline — see the field.
//
// ================================================================================================
// REGENERATING THE COMPILED MODULES
// ================================================================================================
//
//   slangc frame.slang -target spirv -profile spirv_1_5 -I <shaders dir> \
//          -entry cyDepthVertex    -stage vertex   -o frame_depth_vertex.spv
//   slangc ... -entry cyForwardVertex   -stage vertex   -o frame_forward_vertex.spv
//   slangc ... -entry cyForwardFragment -stage fragment -o frame_forward_fragment.spv
//   slangc ... -entry cyDepthFragment   -stage fragment -o frame_depth_fragment.spv
//
// `src/rendering/pipeline/shaders/embed_spirv.py` turns them into the committed header, for the
// reason samples/03-first-light's does: `CY_SHADER_SLANG` is off by default and a milestone
// artefact that only runs on a machine with a shader toolchain is one nobody can run.
//
// For Metal, compile with CY_FRAME_METAL=1 and CY_MATERIAL_METAL_ARGUMENT_BUFFER=1. The view
// resources below form one argument buffer. `embed_msl.py` checks Slang's compacted entry indices
// and maps them to the RHI's fixed set and push slots. Set 0 carries the bounded material texture
// table at the RHI's argument indices: globals 0, textures 1..128, sampler 129.

module frame;

import cy.brdf;
import cy.cluster;
import cy.globals;
import cy.light;
import cy.material;
import cy.packing;
import cy.shadow;

// --- The per-view block -------------------------------------------------------------------------

/// What one assembled frame tells its passes. `cy::rendering::pipeline::FrameViewData` is this
/// struct's C++ half and the two are asserted equal, field offset for field offset.
public struct CyFrameData
{
    /// Camera-relative space to clip space. Reversed-Z: near maps to 1, far to 0. See the header
    /// comment for why these are rows and not a `float4x4`.
    public float4 relativeToClipRow0;
    public float4 relativeToClipRow1;
    public float4 relativeToClipRow2;
    public float4 relativeToClipRow3;
    public float4 previousRelativeToClipRow0;
    public float4 previousRelativeToClipRow1;
    public float4 previousRelativeToClipRow2;
    public float4 previousRelativeToClipRow3;
    /// Camera-relative space to VIEW space. Only the third row is ever read — the cluster slice
    /// needs a view-space depth and nothing else does — but all four are carried so a pass that
    /// wants a view-space normal has them without a second upload.
    public float4 relativeToViewRow0;
    public float4 relativeToViewRow1;
    public float4 relativeToViewRow2;
    public float4 relativeToViewRow3;

    /// The sky's irradiance contribution, and the ambient occlusion strength in `w`.
    public float4 ambientAndOcclusion;
    /// xy: the render extent in pixels. zw: its reciprocal.
    public float4 extentAndInverse;

    public ClusterGrid clusterGrid;

    /// x: how many lights `cyLights` holds. y: how many words one material block is.
    /// z: `kClusterElementTypeCount`, so the header stride is data rather than a constant repeated
    /// on two sides. w: 1 when the cluster lists were assigned, 0 when the frame had no grid — in
    /// which case the shading loop walks every light instead, which is the honest fallback and is
    /// what a frame with `cluster_count() == 0` actually gets.
    public uint4 counts;
    /// Word offsets into a material block: base colour, roughness, metallic, emission. Derived from
    /// the `MaterialProgram`; see the header comment.
    public uint4 materialOffsets;
    /// x: previous history is valid. y: feedback weight.
    public float4 temporalFeedback;
    /// Current jitter in xy and previous jitter in zw, in pixel units.
    public float4 temporalJitter;
    /// Word offsets of the material block's TEXTURE SLOTS. x is the base colour texture's slot;
    /// y, z and w are unclaimed and carry `kCyNoMaterialTexture`. Derived from the
    /// `MaterialProgram` exactly as `materialOffsets` above is, and for the same reason.
    ///
    /// APPENDED RATHER THAN INSERTED, which is what keeps the other committed modules valid:
    /// `cy/particle.slang` imports this module and its SPIR-V is embedded in
    /// `src/rendering/particles/shaders/`, compiled against the block as it was. Every field it
    /// reads is at the offset it was at, the buffer is simply longer than the block it declares,
    /// and it needs no regeneration. An INSERTED field would have moved `temporalJitter` under it.
    ///
    /// `kCyNoMaterialTexture` in x means THE FRAME DECLARED NO TEXTURED BASE COLOUR, and the
    /// surface below is the four constants it has always been. That is the default of
    /// `FrameViewData::material_textures`, so a caller that knows nothing about this field gets
    /// the frame it got before it existed — the picture does not change under a caller that did
    /// not ask for it, which is what keeps every committed capture in the tree honest.
    public uint4 materialTextures;
    /// Optional directional shadow map, appended to preserve existing frame-block offsets.
    public float4 shadowToClipRow0;
    public float4 shadowToClipRow1;
    public float4 shadowToClipRow2;
    public float4 shadowToClipRow3;
    /// x: frame texture slot; y: light index; z: map extent; w: enabled.
    public uint4 shadowControl;
    /// The ambient occlusion term, appended for the reason `materialTextures` gives. x: the frame
    /// texture slot of `occlusion::AmbientOcclusionPass`'s target — visibility in `.a` — or
    /// `kCyNoMaterialTexture`, which is the default and draws the frame as it was. y: 1 to occlude
    /// the direct term too, the non-physical option `rendering-post-processing` requires to be off
    /// by default. z: that option's strength, as float bits.
    public uint4 occlusionControl;
    /// Soft and contact shadows for the directional shadow above, appended for the reason
    /// `materialTextures` gives. x: flags — `kCySoftShadowPcss` filters with a blocker search and a
    /// penumbra from the light's angular radius, `kCySoftShadowContact` adds the contact term. y:
    /// the frame texture slot of `contact::ContactShadowPass`'s target, or `kCyNoMaterialTexture`.
    /// z: blocker search taps. w: filter taps. ZERO FLAGS, THE DEFAULT, IS THE 3x3 FILTER THE FRAME
    /// ALWAYS HAD, arithmetic for arithmetic.
    public uint4 softShadowControl;
    /// `PcssShape` as floats: x penumbra UV per unit of light-space depth, y smallest kernel (UV),
    /// z largest kernel (UV). w: unused.
    public float4 softShadowShape;
};

/// `softShadowControl.x`'s bits. `cy::rendering::pipeline::kSoftShadowPcss` and
/// `kSoftShadowContact` are the same numbers.
public static const uint kCySoftShadowPcss = 1u;
public static const uint kCySoftShadowContact = 2u;

/// "This material has no texture in this slot", and also "this frame reads no such slot".
///
/// It is `rhi::kInvalidBindlessIndex` — `~0u` — spelled on the shader side, because the value in
/// the material block IS a bindless index and the two sides must agree about which index means
/// nothing. Zero cannot: zero is a perfectly good slot of the global table, and a material block
/// that was never written would name it.
public static const uint kCyNoMaterialTexture = 0xFFFFFFFFu;

/// `GpuDrawInstance` — 32 bytes, the per-draw indirection `first_instance` names.
public struct CyDrawInstance
{
    public uint instanceSlot;
    public uint material;
    public uint parameterOffset;
    public uint giAddress;
    public uint layerMask;
    public uint lodAndFade;
    public uint surface;
    public uint flags;
};

/// One instance's placement, model to CAMERA-RELATIVE. Never world space (design.md §3).
public struct CyInstanceTransform
{
    public float4 row0;
    public float4 row1;
    public float4 row2;
    /// rgb: a per-instance tint the material's base colour is multiplied by. a: unused.
    public float4 tint;
};

public struct CyFrameViewSet
{
    public ConstantBuffer<CyFrameData> frame;
    public StructuredBuffer<Light> lights;
    public StructuredBuffer<uint2> clusterHeaders;
    public StructuredBuffer<uint> clusterIndices;
    public StructuredBuffer<CyDrawInstance> drawInstances;
    public StructuredBuffer<CyInstanceTransform> instances;
    public StructuredBuffer<uint> materialWords;
};

[[vk::binding(0, 1)]]
public ParameterBlock<CyFrameViewSet> cyFrameView;

#define cyFrame cyFrameView.frame
#define cyLights cyFrameView.lights
#define cyClusterHeaders cyFrameView.clusterHeaders
#define cyClusterIndices cyFrameView.clusterIndices
#define cyDrawInstances cyFrameView.drawInstances
#define cyInstances cyFrameView.instances
#define cyMaterialWords cyFrameView.materialWords

#if defined(CY_FRAME_METAL)
public struct CyFrameGlobalSet
{
    public ConstantBuffer<CyGlobalsData> globals;
    public Texture2D<float4> textures[128];
    public SamplerState sampler;
};

[[vk::binding(0, 0)]]
public ParameterBlock<CyFrameGlobalSet> cyFrameGlobals;

public float4 cyMaterialSampleTexture(uint bindlessIndex, float2 uv)
{
    return cyFrameGlobals.textures[bindlessIndex].Sample(cyFrameGlobals.sampler, uv);
}

public float4 cyMaterialSampleTextureLevel(uint bindlessIndex, float2 uv, float level)
{
    return cyFrameGlobals.textures[bindlessIndex].SampleLevel(cyFrameGlobals.sampler, uv, level);
}
#endif

/// The one push constant: which draw is being recorded.
public struct CyDrawPush
{
    public uint drawIndex;
};

[[vk::push_constant]]
public ConstantBuffer<CyDrawPush> cyDraw;

// --- Shared helpers -------------------------------------------------------------------------

public float4 transformToClip(float3 relative)
{
    let point = float4(relative, 1.0);
    return float4(dot(cyFrame.relativeToClipRow0, point), dot(cyFrame.relativeToClipRow1, point),
                  dot(cyFrame.relativeToClipRow2, point), dot(cyFrame.relativeToClipRow3, point));
}

public float4 transformToShadowClip(float3 relative)
{
    let point = float4(relative, 1.0);
    return float4(dot(cyFrame.shadowToClipRow0, point),
                  dot(cyFrame.shadowToClipRow1, point),
                  dot(cyFrame.shadowToClipRow2, point),
                  dot(cyFrame.shadowToClipRow3, point));
}

public float3 transformToRelative(CyInstanceTransform instance, float3 modelPosition)
{
    let point = float4(modelPosition, 1.0);
    return float3(dot(instance.row0, point), dot(instance.row1, point), dot(instance.row2, point));
}

/// The rotation half of the placement, applied to a direction. No inverse transpose: the frame's
/// instances are rigid plus uniform scale, and `rendering-geometry-and-resources` says a
/// non-uniform scale is normalised at import rather than compensated for per pixel.
public float3 rotateToRelative(CyInstanceTransform instance, float3 direction)
{
    return normalize(float3(dot(instance.row0.xyz, direction), dot(instance.row1.xyz, direction),
                            dot(instance.row2.xyz, direction)));
}

public float viewDepthOf(float3 relative)
{
    let point = float4(relative, 1.0);
    // -Z forward (`core-math`), so a point in front of the camera has a negative view-space z and
    // the positive depth the cluster slice wants is its negation.
    return -dot(cyFrame.relativeToViewRow2, point);
}

float readMaterialFloat(uint material, uint wordOffset)
{
    return asfloat(cyMaterialWords[material * cyFrame.counts.y + wordOffset]);
}

float3 readMaterialFloat3(uint material, uint wordOffset)
{
    let base = material * cyFrame.counts.y + wordOffset;
    return float3(asfloat(cyMaterialWords[base]), asfloat(cyMaterialWords[base + 1]),
                  asfloat(cyMaterialWords[base + 2]));
}

/// A `ParameterKind::Texture` is four bytes like everything else — "a texture parameter is a
/// number, which is what makes a GPU-generated draw able to reach it" — so it is read as a word and
/// not reinterpreted as a float.
uint readMaterialUint(uint material, uint wordOffset)
{
    return cyMaterialWords[material * cyFrame.counts.y + wordOffset];
}

/// Which slot of the global texture table one material's slot word names, and
/// `kCyNoMaterialTexture` when either the FRAME declares no such slot or the MATERIAL has nothing
/// bound in it. Both answers have to be possible: the offset is a property of the program and the
/// value is a property of the material, and a frame that read the value without checking the offset
/// would index the table with whatever word happens to sit at offset zero.
uint materialTextureSlot(uint material, uint wordOffset)
{
    if (wordOffset == kCyNoMaterialTexture)
    {
        return kCyNoMaterialTexture;
    }
    return readMaterialUint(material, wordOffset);
}

/// The surface the frame shades, read OUT OF THE MATERIAL TABLE. See the header comment.
///
/// AND SAMPLED OUT OF THE GLOBAL TEXTURE TABLE, which until M11.c nothing in the engine's own
/// forward path did: this function read four constants and every object in a frame the engine
/// assembled was one flat colour. The base colour factor is now a FACTOR — it multiplies the
/// sampled texel, which is glTF's own rule and what makes "the same surface with the texture
/// replaced by its declared average" a comparable picture rather than a differently-lit one.
///
/// A PLAIN INDEX, AND THE CONDITION IT RESTS ON IS WRITTEN DOWN RATHER THAN ASSUMED.
/// `FrameRecorder` records one draw per `DrawItem` with an instance count of ONE
/// (`frame_recorder.cpp`: `draw_indexed(..., 1, ...)`), and a material is a property of the draw —
/// so every fragment of a drawing command shades the same material and the slot is dynamically
/// uniform, which is what indexing a descriptor array without `NonUniformResourceIndex` requires.
/// The day this layer merges draws that share geometry and differ in material, that stops being
/// true and this call needs the non-uniform form: Slang's `NonUniformResourceIndex` is REFUSED BY
/// THE METAL TARGET today ("uses features that are not available in 'fragment' stage for 'metal'
/// compilation target"), and `frame_msl.h` is a shipped artefact, so writing it here would trade a
/// condition that holds for a backend that does not build.
///
/// The array and the sampler are `cy.material`'s, imported rather than restated, so the Slang
/// compiler checks the set and binding numbers against the declaration the RHI's own global table
/// is built from.
///
/// THE IMPLICIT LEVEL OF DETAIL, which is task 3.8's own point. `cyMaterialSampleTextureLevel(...,
/// 0.0)` is what the generated prelude calls because it cannot know it is in a pixel stage; this
/// function can, so a minified surface reads the cooked mip chain instead of aliasing against
/// level 0.
Surface surfaceOf(uint material, float3 tint, float2 uv)
{
    Surface surface = defaultSurface();
    surface.albedo = readMaterialFloat3(material, cyFrame.materialOffsets.x) * tint;
    surface.roughness = clamp(readMaterialFloat(material, cyFrame.materialOffsets.y), 0.02, 1.0);
    surface.metallic = saturate(readMaterialFloat(material, cyFrame.materialOffsets.z));
    surface.emission = readMaterialFloat3(material, cyFrame.materialOffsets.w);

    let albedoSlot = materialTextureSlot(material, cyFrame.materialTextures.x);
    if (albedoSlot != kCyNoMaterialTexture)
    {
        surface.albedo *= cyMaterialSampleTexture(albedoSlot, uv).rgb;
    }
    return surface;
}

// --- The depth prepass ------------------------------------------------------------------------

public struct CyShadowVertex
{
    public float4 position : SV_Position;
};

[shader("vertex")]
public CyShadowVertex cyShadowVertex(float3 modelPosition : POSITION)
{
    let draw = cyDrawInstances[cyDraw.drawIndex];
    let instance = cyInstances[draw.instanceSlot];
    CyShadowVertex output;
    output.position = transformToShadowClip(transformToRelative(instance, modelPosition));
    return output;
}

[shader("fragment")]
public float cyShadowFragment(CyShadowVertex input) : SV_Target
{
    return input.position.z;
}
//
// POSITION ONLY. `render::kDepthPassStreams` is `stream_bit(Position)` and this shader is why that
// constant exists: binding the normal and UV streams for a pass that writes depth is exactly the
// bandwidth `rendering-geometry-and-resources` split the streams to avoid.

public struct CyDepthVertex
{
    public float4 position : SV_Position;
    public float3 normal : TEXCOORD0;
    public float4 currentClip : TEXCOORD1;
    public float4 previousClip : TEXCOORD2;
};

[shader("vertex")]
public CyDepthVertex cyDepthVertex(float3 modelPosition : POSITION, float4 packedNormal : NORMAL)
{
    let draw = cyDrawInstances[cyDraw.drawIndex];
    let instance = cyInstances[draw.instanceSlot];
    CyDepthVertex output;
    let relative = transformToRelative(instance, modelPosition);
    output.position = transformToClip(relative);
    output.currentClip = output.position;
    let point = float4(relative, 1.0);
    output.previousClip = float4(dot(cyFrame.previousRelativeToClipRow0, point),
                                 dot(cyFrame.previousRelativeToClipRow1, point),
                                 dot(cyFrame.previousRelativeToClipRow2, point),
                                 dot(cyFrame.previousRelativeToClipRow3, point));
    output.normal = rotateToRelative(instance, decodeOctahedral(packedNormal.xy));
    return output;
}

public struct CyDepthOutput
{
    public float4 normalRoughness : SV_Target0;
    public float2 velocity : SV_Target1;
};

[shader("fragment")]
public CyDepthOutput cyDepthFragment(CyDepthVertex input)
{
    CyDepthOutput output;
    output.normalRoughness = float4(encodeOctahedral(normalize(input.normal)), 1.0, 1.0);
    // JITTER REMOVED HERE, which is `temporal-rendering`'s convention and the one
    // `src/rendering/temporal/README.md` states: `history_uv = current_uv + motion`, and a still
    // surface under a still camera has a motion vector of exactly zero. `currentClip` is the
    // JITTERED position the rasteriser used and `previousClip` the unjittered one, so the offset
    // `upload_for` added (2 * jitter / extent in NDC) is taken back out. Leaving it in made every
    // still pixel's motion the current jitter, and the resolve re-sampled its history at a
    // different sub-pixel offset every frame.
    let currentNdc = input.currentClip.xy / input.currentClip.w -
                     cyFrame.temporalJitter.xy * 2.0 * cyFrame.extentAndInverse.zw;
    let previousNdc = input.previousClip.xy / input.previousClip.w;
    output.velocity = float2((previousNdc.x - currentNdc.x) * 0.5,
                             (currentNdc.y - previousNdc.y) * 0.5);
    return output;
}

// --- The forward pass ---------------------------------------------------------------------------

public struct CyForwardVertex
{
    public float4 position : SV_Position;
    public float3 relativePosition : TEXCOORD0;
    public float3 normal : TEXCOORD1;
    public float2 uv : TEXCOORD2;
    public nointerpolation uint drawIndex : TEXCOORD3;
};

[shader("vertex")]
public CyForwardVertex cyForwardVertex(float3 modelPosition : POSITION,
                                       float4 packedNormal : NORMAL, float2 uv : TEXCOORD0)
{
    let draw = cyDrawInstances[cyDraw.drawIndex];
    let instance = cyInstances[draw.instanceSlot];

    CyForwardVertex output;
    output.relativePosition = transformToRelative(instance, modelPosition);
    output.position = transformToClip(output.relativePosition);
    // `render::PackedNormalTangent` is two 16-bit signed normalised pairs: the octahedral normal in
    // xy and the octahedral tangent in zw. Only the normal is read here; a material that wants a
    // tangent frame reads zw the same way.
    output.normal = rotateToRelative(instance, decodeOctahedral(packedNormal.xy));
    output.uv = uv;
    output.drawIndex = cyDraw.drawIndex;
    return output;
}

/// The frame's shadow map, as `cy.shadow`'s soft filter reads it: through the texture table, the
/// way the 3x3 filter below reads it.
struct CyFrameShadowMap : IShadowDepthSource
{
    uint slot;
    float storedDepth(float2 uv)
    {
        return cyMaterialSampleTextureLevel(slot, uv, 0.0).r;
    }
};

/// Percentage-closer soft shadows over the frame's map: `cy.shadow`'s three steps with the shape
/// `softShadowShape` carries. See `softShadowControl`.
float softShadowVisibility(float2 uv, float reference, float2 pixel)
{
    CyFrameShadowMap map;
    map.slot = cyFrame.shadowControl.x;
    PcssShape shape;
    shape.penumbraPerDepth = cyFrame.softShadowShape.x;
    shape.minRadius = cyFrame.softShadowShape.y;
    shape.maxRadius = cyFrame.softShadowShape.z;
    shape.blockerTaps = int(max(cyFrame.softShadowControl.z, 1u));
    shape.filterTaps = int(max(cyFrame.softShadowControl.w, 1u));
    return pcssVisibility(map, uv, reference, shape, shadowDiscRotation(pixel));
}

/// The contact term at a pixel: `contact::ContactShadowPass`'s visibility, 1 where its screen-space
/// trace found nothing. The target is the frame's own extent, so the fragment's centre is a
/// texel's centre.
float contactShadowVisibility(float2 fragmentCentre)
{
    if ((cyFrame.softShadowControl.x & kCySoftShadowContact) == 0u ||
        cyFrame.softShadowControl.y == kCyNoMaterialTexture)
    {
        return 1.0;
    }
    let uv = fragmentCentre * cyFrame.extentAndInverse.zw;
    return cyMaterialSampleTextureLevel(cyFrame.softShadowControl.y, uv, 0.0).r;
}

/// The light loop, over the cluster this pixel falls in.
///
/// `counts.w` of zero means the frame declared no cluster grid — a view with no lights, or a grid
/// configured to nothing — and the loop then walks every light. That is a DECLARED fallback rather
/// than a silent one: it is slower and it is correct, and a frame that quietly shaded nothing
/// because its grid was empty is the failure this branch exists to make impossible.
float directionalShadowVisibility(float3 relativePosition, float3 normal, float2 fragmentCentre)
{
    if (cyFrame.shadowControl.w == 0 || cyFrame.shadowControl.x == kCyNoMaterialTexture)
    {
        return 1.0;
    }
    let clip = transformToShadowClip(relativePosition + normal * 0.005);
    if (clip.w <= 0.0)
    {
        return 1.0;
    }
    let ndc = clip.xyz / clip.w;
    let uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        return 1.0;
    }
    let radius = 1.0 / float(max(cyFrame.shadowControl.z, 1u));
    let reference = ndc.z + 0.0005;
    if ((cyFrame.softShadowControl.x & kCySoftShadowPcss) != 0u)
    {
        return softShadowVisibility(uv, reference, fragmentCentre);
    }
    float visible = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            let sampleUv = uv + float2(float(x), float(y)) * radius;
            let stored = cyMaterialSampleTextureLevel(cyFrame.shadowControl.x, sampleUv, 0.0).r;
            visible += reference >= stored ? 1.0 : 0.0;
        }
    }
    return visible / 9.0;
}

float3 accumulateLights(Surface surface, float3 relativePosition, float3 normal, float3 viewDir,
                        float2 fragmentCentre, uint instanceFlags)
{
    let pixel = uint2(fragmentCentre);
    float3 lit = float3(0.0);
    let lightCount = cyFrame.counts.x;

    // A DIRECTIONAL LIGHT IS NEVER IN A CLUSTER LIST, and this loop is why the frame is not black.
    // `FrameAssembly::build_lights` says so in as many words — "a directional light has an infinite
    // bound and is not clustered: it reaches every cluster, and putting it in every list would be
    // the whole grid holding one index" — so the cluster list carries the PUNCTUAL lights and the
    // sun is evaluated globally. The first version of this shader looped the cluster list alone; the
    // captured frame came back lit by two point lights and nothing else, which is how the omission
    // was found. There are a handful of directional lights in any frame, so the loop is short.
    for (uint global = 0; global < lightCount; ++global)
    {
        if (cyLights[global].kind != kLightDirectional)
        {
            continue;
        }
        let sunSample = evaluateLight(cyLights[global], relativePosition);
        // THE CONTACT TERM IS AN ADDITION TO THE MAP'S, NOT A REPLACEMENT: `virtual-shadows`'
        // "Contact and traced refinement". The darker of the two, so an occluder both see is not
        // counted twice and one only the trace sees still darkens the contact.
        let visibility = global == cyFrame.shadowControl.y && (instanceFlags & 8u) != 0u
                             ? min(directionalShadowVisibility(relativePosition, normal,
                                                               fragmentCentre),
                                   contactShadowVisibility(fragmentCentre))
                             : 1.0;
        lit += shadeSurfaceWithLight(surface, normal, viewDir, sunSample) * visibility;
    }

    if (cyFrame.counts.w == 0 || cyFrame.clusterGrid.dimensions.z == 0)
    {
        for (uint index = 0; index < lightCount; ++index)
        {
            if (cyLights[index].kind == kLightDirectional)
            {
                continue;  // already accumulated above
            }
            let sample = evaluateLight(cyLights[index], relativePosition);
            lit += shadeSurfaceWithLight(surface, normal, viewDir, sample);
        }
        return lit;
    }

    let extent = uint2(cyFrame.extentAndInverse.xy);
    let coord = clusterCoordOf(cyFrame.clusterGrid, pixel, extent, viewDepthOf(relativePosition));
    let cluster = clusterIndexOf(cyFrame.clusterGrid, coord);
    // The headers are indexed `cluster * kClusterElementTypeCount + type`, and the light type is
    // zero. The stride comes from `counts.z` so the two sides cannot disagree about it.
    let header = cyClusterHeaders[cluster * cyFrame.counts.z];
    for (uint slot = 0; slot < header.y; ++slot)
    {
        let index = cyClusterIndices[header.x + slot];
        if (index >= lightCount || cyLights[index].kind == kLightDirectional)
        {
            continue;
        }
        let sample = evaluateLight(cyLights[index], relativePosition);
        lit += shadeSurfaceWithLight(surface, normal, viewDir, sample);
    }
    return lit;
}

/// The ambient occlusion term's visibility at a fragment. The target is the frame's own extent, so
/// the fragment's centre is a texel's centre and the table's filtering returns that texel.
float occlusionVisibility(float2 fragmentCentre)
{
    let uv = fragmentCentre * cyFrame.extentAndInverse.zw;
    return cyMaterialSampleTextureLevel(cyFrame.occlusionControl.x, uv, 0.0).a;
}

[shader("fragment")]
public float4 cyForwardFragment(CyForwardVertex input) : SV_Target
{
    let draw = cyDrawInstances[input.drawIndex];
    let instance = cyInstances[draw.instanceSlot];
    let surface = surfaceOf(draw.material, instance.tint.rgb, input.uv);

    let normal = normalize(input.normal);
    // Camera-relative, so the camera is at the origin and the direction to it is the negated
    // position. There is nowhere in this file to put a world-space camera, which is the point.
    let viewDir = normalize(-input.relativePosition);

    var color = accumulateLights(surface, input.relativePosition, normal, viewDir,
                                 input.position.xy, draw.flags);
    // AMBIENT OCCLUSION DARKENS THE AMBIENT TERM AND NOTHING ELSE. `rendering-post-processing`:
    // "AO SHALL modulate indirect lighting only; applying it to direct light SHALL be an explicitly
    // non-physical artistic option, off by default." The direct sum above is untouched unless that
    // option is on, and with no occlusion slot the arithmetic below is the frame's as it was.
    var ambient = surface.albedo * cyFrame.ambientAndOcclusion.rgb * surface.occlusion;
    if (cyFrame.occlusionControl.x != kCyNoMaterialTexture)
    {
        let visibility = occlusionVisibility(input.position.xy);
        ambient *= visibility;
        if (cyFrame.occlusionControl.y != 0u)
        {
            color *= lerp(1.0, visibility, asfloat(cyFrame.occlusionControl.z));
        }
    }
    color += surface.emission;
    color += ambient;
    return float4(color, surface.opacity);
}
