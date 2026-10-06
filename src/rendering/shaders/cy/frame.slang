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
import cy.decal;
import cy.globals;
import cy.light;
import cy.material;
import cy.packing;
import cy.shadow;
import cy.volumetric_fog;

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
    /// The irradiance volume, appended for the reason `materialTextures` gives. x: the frame
    /// texture slot of the volume's packed probes (`gi::IrradianceVolume::pack_texels`), or
    /// `kCyNoMaterialTexture` — the default, which draws the flat ambient as it was. yzw: probes
    /// along x, y and z.
    public uint4 probeVolumeControl;
    /// xyz: probe (0, 0, 0), CAMERA-RELATIVE like every position here. w: probe spacing in metres.
    public float4 probeVolumeOrigin;
    /// x: the scale the stored coefficients were divided by. y: the normal offset of a query, and
    /// z: the visibility test's slack, both in metres. w: reserved.
    public float4 probeVolumeParams;
    /// The decal table, appended for the reason `materialTextures` gives. x: the frame texture slot
    /// of `decals::DecalTableTexture`, or `kCyNoMaterialTexture` — the default, which draws no
    /// decals and is the frame as it was. y: the texture's rows. z: `kCyDecalListsInTable` and
    /// `kCyDecalWalkAll`. w: reserved.
    public uint4 decalControl;
    /// Volumetric fog, appended for the reason `materialTextures` gives. x: the frame texture slot
    /// of `fog::FogPass`'s volume, or `kCyNoMaterialTexture` — the default, which draws every
    /// surface as it was lit. yzw: reserved.
    public uint4 volumetricFogControl;
    /// Per-object motion, appended for the reason `materialTextures` gives. x: the index in
    /// `cyInstances` of the first PREVIOUS-frame row — row `x + slot` is where instance `slot` was
    /// last frame, already expressed about this frame's camera — or `kCyNoPreviousInstances`, the
    /// default, in which case every instance is taken to be where it is now and the prepass writes
    /// camera motion alone, arithmetic for arithmetic what it always wrote. yzw: reserved.
    /// `pipeline::FrameBindings::upload` fills it from the rows it uploaded the frame before.
    public uint4 motionControl;
    /// The baked lightmap atlas, appended for the reason `materialTextures` gives. x, y, z: the
    /// frame texture slots of its planes (`lightmap_bake::LightmapTexels`), unused ones
    /// `kCyNoMaterialTexture`. w: the encoding — `kCyLightmapNone`, the default, which draws every
    /// surface as it was whatever its draw's `giAddress` says.
    public uint4 lightmapControl;
    /// x: a page's side in texels. y: pages, stacked down the texture. z: the gutter around every
    /// rectangle's interior, in texels. w: the frame texture slot of the baked shadow mask, laid
    /// out like the planes, or `kCyNoMaterialTexture`.
    public uint4 lightmapLayout;
    /// The stationary lights the shadow mask shadows, appended for the reason `materialTextures`
    /// gives: channel c's light, as an index into `cyLights`, or `kCyNoLightmapLight`. Read only
    /// for a lightmapped surface.
    public uint4 lightmapShadowLights;
    /// The lights whose DIRECT term the lightmap's texels already hold — a 128-bit set over
    /// indices into `cyLights`, light i in bit i % 32 of word i / 32. A lightmapped surface does
    /// not shade them again. Zero, the default, names none.
    public uint4 lightmapDirectLights;
    /// The lightmap texel-density view. x: `kCyLightmapDensityView` draws every surface as the
    /// density its lightmap gives it against y — the target, texels per metre, as float bits —
    /// instead of lit. Zero, the default, is the lit frame. zw: reserved.
    public uint4 lightmapDebug;
};

/// `motionControl.x` when the frame carries no previous instance rows.
/// `cy::rendering::pipeline::kNoPreviousInstances` is the same number.
public static const uint kCyNoPreviousInstances = 0xFFFFFFFFu;

/// `softShadowControl.x`'s bits. `cy::rendering::pipeline::kSoftShadowPcss` and
/// `kSoftShadowContact` are the same numbers.
public static const uint kCySoftShadowPcss = 1u;
public static const uint kCySoftShadowContact = 2u;

/// `decalControl.z`'s bits. `cy::rendering::decals::kDecalListsInTable` and `kDecalWalkAll` are the
/// same numbers.
public static const uint kCyDecalListsInTable = 1u;
public static const uint kCyDecalWalkAll = 2u;

/// Where `applyFrameDecals` reads the rank of its i-th decal from.
static const uint kCyDecalRanksAll = 0u;
static const uint kCyDecalRanksTable = 1u;
static const uint kCyDecalRanksClusters = 2u;

/// "This material has no texture in this slot", and also "this frame reads no such slot".
///
/// It is `rhi::kInvalidBindlessIndex` — `~0u` — spelled on the shader side, because the value in
/// the material block IS a bindless index and the two sides must agree about which index means
/// nothing. Zero cannot: zero is a perfectly good slot of the global table, and a material block
/// that was never written would name it.
public static const uint kCyNoMaterialTexture = 0xFFFFFFFFu;

/// `lightmapControl.w`: `lightmap_bake::LightmapMode` plus one, so zero is "no lightmap".
public static const uint kCyLightmapNone = 0u;
public static const uint kCyLightmapIrradiance = 1u;
public static const uint kCyLightmapDirectional = 2u;
public static const uint kCyLightmapShL1 = 3u;

/// `lightmapShadowLights`: a channel no light has. `lightmaps::kNoLightmapLight` is the same number.
public static const uint kCyNoLightmapLight = 0xFFFFFFFFu;
/// `lightmapDebug.x`: the texel-density view. `lightmaps::kLightmapDensityView` is the same number.
public static const uint kCyLightmapDensityView = 1u;

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

/// Where this vertex was last frame, camera-relative to THIS frame's camera.
///
/// DERIVED, NOT AUTHORED — `temporal-rendering`'s "Motion vectors are derived". The placement is
/// last frame's row of the same GPU scene slot when the frame carries one, and the model-space
/// position is the previous frame's output of whatever deformed the mesh: the other half of a
/// skinned mesh's double-buffered output, or — for a rigid mesh — the very same stream bound a
/// second time, so the value is the current one bit for bit. With no previous rows the result is
/// `transformToRelative(instance, modelPosition)` exactly, which is the expression this prepass
/// always pushed through the previous projection: camera motion and nothing else.
float3 previousRelativeOf(uint slot, CyInstanceTransform current, float3 previousModelPosition)
{
    let base = cyFrame.motionControl.x;
    let placement = base == kCyNoPreviousInstances ? current : cyInstances[base + slot];
    return transformToRelative(placement, previousModelPosition);
}

[shader("vertex")]
public CyDepthVertex cyDepthVertex(float3 modelPosition : POSITION, float4 packedNormal : NORMAL,
                                   float3 previousModelPosition : PREVIOUS_POSITION)
{
    let draw = cyDrawInstances[cyDraw.drawIndex];
    let instance = cyInstances[draw.instanceSlot];
    CyDepthVertex output;
    let relative = transformToRelative(instance, modelPosition);
    output.position = transformToClip(relative);
    output.currentClip = output.position;
    let point = float4(previousRelativeOf(draw.instanceSlot, instance, previousModelPosition), 1.0);
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
    /// The cooked `TexCoords2` stream: the mesh's own lightmap unit square, which the draw's
    /// `giAddress` places in the atlas. Appended, so the interpolants above keep their locations.
    public float2 lightmapUv : TEXCOORD4;
};

/// THE FOURTH STREAM IS ALWAYS BOUND. A geometry source with no lightmap coordinates binds its UV0
/// buffer there (`FrameRecorder`), which is read and never used: a draw reads `lightmapUv` only when
/// its `giAddress` names a rectangle, and a mesh without UV2 is never given one.
[shader("vertex")]
public CyForwardVertex cyForwardVertex(float3 modelPosition : POSITION,
                                       float4 packedNormal : NORMAL, float2 uv : TEXCOORD0,
                                       float2 lightmapUv : TEXCOORD1)
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
    output.lightmapUv = lightmapUv;
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

/// What the lightmap says about the lights, for one surface: whether the surface is lightmapped at
/// all, and its shadow mask, sampled once outside the light loop.
struct CyBakedLights
{
    bool active;
    float4 mask;
};

/// How much of one light's direct term a lightmapped surface still shades: none of a light whose
/// direct term is in the texels, a stationary light's baked visibility, and all of any other.
float bakedLightFactor(uint index, float4 mask)
{
    if (index < 128u && ((cyFrame.lightmapDirectLights[index >> 5u] >> (index & 31u)) & 1u) != 0u)
    {
        return 0.0;
    }
    for (uint channel = 0u; channel < 4u; ++channel)
    {
        if (cyFrame.lightmapShadowLights[channel] == index)
        {
            return mask[channel];
        }
    }
    return 1.0;
}

/// A punctual light's sample, scaled by what the lightmap left of it. Unchanged, bit for bit, on a
/// surface with no lightmap.
LightSample bakedLightSample(uint index, float3 relativePosition, CyBakedLights baked)
{
    var sample = evaluateLight(cyLights[index], relativePosition);
    if (baked.active)
    {
        sample.illuminance *= bakedLightFactor(index, baked.mask);
    }
    return sample;
}

float3 accumulateLights(Surface surface, float3 relativePosition, float3 normal, float3 viewDir,
                        float2 fragmentCentre, uint instanceFlags, CyBakedLights baked)
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
        var visibility = global == cyFrame.shadowControl.y && (instanceFlags & 8u) != 0u
                             ? min(directionalShadowVisibility(relativePosition, normal,
                                                               fragmentCentre),
                                   contactShadowVisibility(fragmentCentre))
                             : 1.0;
        // A STATIONARY SUN ON A LIGHTMAPPED SURFACE: the baked mask is its shadow from the static
        // world, the map (when there is one) its shadow from everything, and the darker of the two
        // counts an occluder both see once. A static sun's direct term is in the texels: zero.
        if (baked.active)
        {
            visibility = min(visibility, bakedLightFactor(global, baked.mask));
        }
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
            let sample = bakedLightSample(index, relativePosition, baked);
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
        let sample = bakedLightSample(index, relativePosition, baked);
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

// --- The irradiance volume ----------------------------------------------------------------------
//
// A TRANSCRIPTION of `gi::IrradianceVolume::sample` and `ambient`, over the texels its
// `pack_texels` writes: five per probe — the red, green and blue channels' SH L1 coefficients,
// then (validity, +x, -x, +y) and (-y, +z, -z, 0) world distances. The host is the reference and
// `render.light_probes` holds the two to the same pictures.

static const uint kCyProbeTexels = 5u;

float4 probeVolumeTexel(uint3 probe, uint texel)
{
    let counts = cyFrame.probeVolumeControl.yzw;
    let coordinate =
        float2(float(probe.x * kCyProbeTexels + texel), float(probe.y + counts.y * probe.z));
    let size = float2(float(counts.x * kCyProbeTexels), float(counts.y * counts.z));
    // Texel centres, so the table's filtering returns the texel.
    return cyMaterialSampleTextureLevel(cyFrame.probeVolumeControl.x, (coordinate + 0.5) / size,
                                        0.0);
}

/// Backface times distance visibility, `IrradianceVolume::visibility`.
float probeVisibility(float4 distances0, float4 distances1, float3 probePosition,
                      float3 position, float3 normal, float3 query)
{
    let towards = probePosition - position;
    let towardsSquared = dot(towards, towards);
    let toProbe = towardsSquared > 1.0e-12 ? towards * rsqrt(towardsSquared) : normal;
    let facing = max(1.0e-4, (dot(toProbe, normal) + 1.0) * 0.5);
    let backface = facing * facing + 0.2;

    // The six distances bound a box of free space around the probe; outside it on any axis, the
    // query is behind something.
    let offset = query - probePosition;
    let free = float3(offset.x >= 0.0 ? distances0.y : distances0.z,
                      offset.y >= 0.0 ? distances0.w : distances1.x,
                      offset.z >= 0.0 ? distances1.y : distances1.z);
    let slack = cyFrame.probeVolumeParams.z;
    let reach = saturate((free + slack - abs(offset)) / slack);
    return backface * min(reach.x, min(reach.y, reach.z));
}

/// The ambient radiance at a surface with the volume attached: `flat` outside it, the volume's
/// visibility-weighted trilinear blend inside, and a blend over one cell at its boundary.
float3 probeVolumeAmbient(float3 position, float3 normal, float3 flat)
{
    let counts = cyFrame.probeVolumeControl.yzw;
    let spacing = cyFrame.probeVolumeOrigin.w;
    let query = position + normal * cyFrame.probeVolumeParams.y;
    let grid = (query - cyFrame.probeVolumeOrigin.xyz) / spacing;
    let last = float3(counts) - 1.0;
    let outside = max(max(-grid, grid - last), 0.0);
    let coverage = saturate(1.0 - max(outside.x, max(outside.y, outside.z)));
    if (coverage <= 0.0)
    {
        return flat;
    }
    let clamped = clamp(grid, 0.0, last);
    let base = min(uint3(floor(clamped)), uint3(max(int3(counts) - 2, int3(0))));
    let fraction = clamped - float3(base);

    // SH L1's cosine convolution, divided by pi: `decode_payload`'s basis, in its (y, z, x) order.
    let basis = float4(0.282095, 0.488603 * (2.0 / 3.0) * normal.y,
                       0.488603 * (2.0 / 3.0) * normal.z, 0.488603 * (2.0 / 3.0) * normal.x);
    var total = float3(0.0);
    var weightSum = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        let step = uint3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        let probe = min(base + step, counts - 1u);
        let pick = float3(step);
        let t = pick * fraction + (1.0 - pick) * (1.0 - fraction);
        let distances0 = probeVolumeTexel(probe, 3u);
        let weight = t.x * t.y * t.z * distances0.x;
        if (weight <= 0.0)
        {
            continue;
        }
        let distances1 = probeVolumeTexel(probe, 4u);
        let probePosition = cyFrame.probeVolumeOrigin.xyz + float3(probe) * spacing;
        let visible =
            weight * probeVisibility(distances0, distances1, probePosition, position, normal, query);
        let irradiance = float3(dot(probeVolumeTexel(probe, 0u), basis),
                                dot(probeVolumeTexel(probe, 1u), basis),
                                dot(probeVolumeTexel(probe, 2u), basis)) *
                         cyFrame.probeVolumeParams.x;
        total += max(irradiance, 0.0) * visible;
        weightSum += visible;
    }
    let volume = weightSum > 1.0e-6 ? total / weightSum : flat;
    return lerp(flat, volume, coverage);
}

// --- Decals -------------------------------------------------------------------------------------
//
// `cy.decal` does the work; what is here is where the table and the lists come from. The table is a
// texture in the frame's own texture table, one word per texel, and a decal's rank is found in the
// cluster lists the light loop reads — the header of type 1, `ClusterElementType::Decal` — because
// the assembly assigns decals in the same pass as lights, as their own element type.

/// The table, read out of the frame's texture table at the slot `decalControl.x` names.
struct CyFrameDecalSource : ICyDecalSource
{
    uint word(uint index)
    {
        let uv = cyDecalWordUv(index, cyFrame.decalControl.y);
        return cyDecalWordFromTexel(cyMaterialSampleTextureLevel(cyFrame.decalControl.x, uv, 0.0));
    }
    float mask(uint slot, float2 uv)
    {
        return cyMaterialSampleTextureLevel(slot, uv, 0.0).a;
    }
};

/// Apply the decals that reach this pixel to its surface and its shading normal, BEFORE the light
/// loop sees either.
void applyFrameDecals(inout Surface surface, inout float3 normal, float3 relativePosition,
                      float2 fragmentCentre, uint receiverChannels)
{
    CyFrameDecalSource source;
    CyDecalReceiver receiver;
    receiver.relativePosition = relativePosition;
    receiver.geometricNormal = normal;
    receiver.eyeDistance = length(relativePosition);
    receiver.channels = receiverChannels;
    CyDecalSurface decalled;
    decalled.albedo = surface.albedo;
    decalled.roughness = surface.roughness;
    decalled.metallic = surface.metallic;
    decalled.emission = surface.emission;
    decalled.normal = normal;

    // ONE LOOP, WHEREVER THE RANKS COME FROM: the walk over every decal, the table's own lists
    // and the assembly's cluster lists differ only in which rank slot `i` names, so the decal
    // itself is applied from one call site rather than one per source.
    let flags = cyFrame.decalControl.z;
    let count = cyDecalCount(source);
    var mode = kCyDecalRanksAll;
    var list = uint2(0u, count);
    if ((flags & kCyDecalWalkAll) != 0u)
    {
        mode = kCyDecalRanksAll;
    }
    else if ((flags & kCyDecalListsInTable) != 0u)
    {
        mode = kCyDecalRanksTable;
        list = count != 0u ? cyDecalTableList(source, relativePosition, fragmentCentre) : uint2(0u);
    }
    else if (cyFrame.counts.w != 0 && cyFrame.clusterGrid.dimensions.z != 0)
    {
        let extent = uint2(cyFrame.extentAndInverse.xy);
        let coord = clusterCoordOf(cyFrame.clusterGrid, uint2(fragmentCentre), extent,
                                   viewDepthOf(relativePosition));
        let cluster = clusterIndexOf(cyFrame.clusterGrid, coord);
        mode = kCyDecalRanksClusters;
        list = cyClusterHeaders[cluster * cyFrame.counts.z + 1u];
    }
    // Otherwise no grid: the declared fallback the light loop takes, for the same reason, and
    // every decal is walked.
    for (uint slot = 0; slot < list.y; ++slot)
    {
        var rank = slot;
        if (mode == kCyDecalRanksTable)
        {
            rank = cyDecalTableListEntry(source, list.x + slot);
        }
        else if (mode == kCyDecalRanksClusters)
        {
            rank = cyClusterIndices[list.x + slot];
        }
        if (rank < count)
        {
            cyApplyDecal(source, rank, receiver, decalled);
        }
    }

    surface.albedo = decalled.albedo;
    surface.roughness = clamp(decalled.roughness, 0.02, 1.0);
    surface.metallic = saturate(decalled.metallic);
    surface.emission = decalled.emission;
    normal = decalled.normal;
}

// --- Volumetric fog -----------------------------------------------------------------------------
//
// `fog::FogPass`'s volume, read texel by texel out of the frame's texture table: the lookup
// `cy.volumetric_fog` makes interpolates the columns and the slices itself, so the table's sampler
// never filters a texel of it.

struct CyFrameFogVolume : ICyFogVolumeSource
{
    uint slot;
    float4 texel(int2 coordinate)
    {
#if defined(CY_FRAME_METAL)
        return cyFrameGlobals.textures[slot].Load(int3(coordinate, 0));
#else
        return cyMaterialTextures[slot].Load(int3(coordinate, 0));
#endif
    }
};

// --- The lightmap -------------------------------------------------------------------------------
//
// A TRANSCRIPTION of `lightmap_bake::atlas_coordinate` and `sample_lightmap`: the draw's
// `giAddress` is the rectangle — seven bits each of block x, block y, width and height, and the
// page plus one — and the mesh's UV2 spans its interior, one gutter in from each edge. The host is
// the reference and `render.lightmaps` holds the two to the same picture.

static const uint kCyLightmapBlocks = 128u;

/// Where UV2 lands in the stacked atlas, normalised to the texture.
float2 lightmapCoordinate(uint address, float2 uv2)
{
    let pageSize = float(cyFrame.lightmapLayout.x);
    let block = float(cyFrame.lightmapLayout.x / kCyLightmapBlocks);
    let gutter = float(cyFrame.lightmapLayout.z);
    let page = float((address >> 28u) - 1u);
    let corner = float2(float(address & 127u), float((address >> 7u) & 127u)) * block;
    let extent = float2(float(((address >> 14u) & 127u) + 1u),
                        float(((address >> 21u) & 127u) + 1u)) * block;
    let texel = corner + gutter + uv2 * (extent - 2.0 * gutter) + float2(0.0, page * pageSize);
    return texel / float2(pageSize, pageSize * float(cyFrame.lightmapLayout.y));
}

/// The ambient radiance the atlas holds for this surface, decoded by the encoding. `normal` is the
/// shading normal: the directional and SH L1 encodings respond to it, irradiance does not.
///
/// THE IMPLICIT LEVEL OF DETAIL: a minified surface reads the mip chain the atlas's gutter and
/// chart padding were laid out for (`lightmap_bake::build_lightmap_mips`), which is filtered per
/// chart so a coarse tap reads only its own. An atlas uploaded with one level reads level 0, as it
/// always did. Called only under a draw-uniform branch, so the derivatives are defined.
float3 lightmapAmbient(float2 uv, float3 normal)
{
    let first = cyMaterialSampleTexture(cyFrame.lightmapControl.x, uv);
    let mode = cyFrame.lightmapControl.w;
    if (mode == kCyLightmapDirectional)
    {
        let direction = cyMaterialSampleTexture(cyFrame.lightmapControl.y, uv);
        let factor = max(0.0, 1.0 + dot(direction.xyz, normal)) / max(direction.w, 1.0e-3);
        return first.rgb * factor;
    }
    if (mode == kCyLightmapShL1)
    {
        let green = cyMaterialSampleTexture(cyFrame.lightmapControl.y, uv);
        let blue = cyMaterialSampleTexture(cyFrame.lightmapControl.z, uv);
        return float3(max(0.0, first.w + dot(first.xyz, normal)),
                      max(0.0, green.w + dot(green.xyz, normal)),
                      max(0.0, blue.w + dot(blue.xyz, normal)));
    }
    return first.rgb;
}

/// The lights a lightmapped surface's lightmap speaks for: its shadow mask, read at the atlas
/// coordinate the ambient term reads, before the light loop so the read is in uniform control flow.
CyBakedLights bakedLightsOf(bool lightmapped, float2 uv)
{
    CyBakedLights baked;
    baked.active = lightmapped;
    baked.mask = float4(1.0);
    if (lightmapped && cyFrame.lightmapLayout.w != kCyNoMaterialTexture)
    {
        baked.mask = cyMaterialSampleTexture(cyFrame.lightmapLayout.w, uv);
    }
    return baked;
}

// --- The lightmap texel-density view ------------------------------------------------------------
//
// An engine debug view the editor requests (`editor-viewport-and-gizmos`: a view mode is "an engine
// debug view requested by the editor, not editor-side drawing"). Every lightmapped surface is drawn
// as a checker of its own lightmap texels, coloured by the density the atlas gives it against the
// level's target: green on target, toward blue at half of it and below, toward red at twice it and
// above. A surface with no lightmap is flat grey. The density is measured, not looked up: texels
// per metre are the screen-space derivatives of the atlas coordinate over those of the position,
// so what is drawn is what the frame actually samples.

static const float3 kCyDensityUnder = float3(0.1, 0.25, 1.0);
static const float3 kCyDensityOnTarget = float3(0.1, 0.9, 0.2);
static const float3 kCyDensityOver = float3(1.0, 0.15, 0.1);
static const float3 kCyDensityUnlit = float3(0.18, 0.18, 0.18);

/// The frame's exposure, which the resolve applies to every pixel after this pass.
float frameExposureStops()
{
#if defined(CY_FRAME_METAL)
    return cyFrameGlobals.globals.exposureStops;
#else
    return cyGlobalSet.data.exposureStops;
#endif
}

/// The view's colour, divided by the exposure the resolve will multiply it by, so the view reads
/// the same under a 100 000 lux sun as in a cellar.
float3 lightmapDensityColour(bool lightmapped, float2 uv, float3 relativePosition)
{
    return lightmapDensityHue(lightmapped, uv, relativePosition) * exp2(-frameExposureStops());
}

float3 lightmapDensityHue(bool lightmapped, float2 uv, float3 relativePosition)
{
    if (!lightmapped)
    {
        return kCyDensityUnlit;
    }
    let size = float2(float(cyFrame.lightmapLayout.x),
                      float(cyFrame.lightmapLayout.x * cyFrame.lightmapLayout.y));
    let texel = uv * size;
    let texels = sqrt(dot(ddx(texel), ddx(texel)) + dot(ddy(texel), ddy(texel)));
    let metres = sqrt(dot(ddx(relativePosition), ddx(relativePosition)) +
                      dot(ddy(relativePosition), ddy(relativePosition)));
    let target = max(asfloat(cyFrame.lightmapDebug.y), 1.0e-6);
    let octaves = clamp(log2(max(texels / max(metres, 1.0e-9), 1.0e-9) / target), -1.0, 1.0);
    let colour = octaves < 0.0 ? lerp(kCyDensityOnTarget, kCyDensityUnder, -octaves)
                               : lerp(kCyDensityOnTarget, kCyDensityOver, octaves);
    let cell = uint2(floor(texel));
    return ((cell.x + cell.y) & 1u) == 0u ? colour : colour * 0.55;
}

/// Shared forward lighting for the standard material and a compiled scene material surface.
/// The caller supplies a resolved surface; both paths keep the frame's decals, lights, lightmap,
/// probes, AO and fog.
public float4 cyShadeForward(Surface resolved, CyForwardVertex input)
{
    let draw = cyDrawInstances[input.drawIndex];
    var surface = resolved;
    // A LIGHTMAPPED SURFACE: `lightmaps::write_lightmaps` set `lightmapControl.w` only under a mode
    // `gi::exclusion_for()` admits lightmaps in. Uniform over the draw.
    let lightmapped = draw.giAddress != 0u && cyFrame.lightmapControl.w != kCyLightmapNone;
    let lightmapUv = lightmapped ? lightmapCoordinate(draw.giAddress, input.lightmapUv)
                                 : float2(0.0);
    if (cyFrame.lightmapDebug.x == kCyLightmapDensityView)
    {
        return float4(lightmapDensityColour(lightmapped, lightmapUv, input.relativePosition), 1.0);
    }

    var normal = normalize(input.normal);
    // DECALS CHANGE THE SURFACE BEFORE IT IS LIT, so the light loop, the shadow and the ambient
    // occlusion below all see the decal as part of the receiver. No slot, the default, and nothing
    // here runs.
    if (cyFrame.decalControl.x != kCyNoMaterialTexture)
    {
        applyFrameDecals(surface, normal, input.relativePosition, input.position.xy,
                         draw.layerMask);
    }
    // Camera-relative, so the camera is at the origin and the direction to it is the negated
    // position. There is nowhere in this file to put a world-space camera, which is the point.
    let viewDir = normalize(-input.relativePosition);

    // THE BAKED LIGHTS: a stationary light's direct term through its shadow-mask channel, and no
    // direct term at all from a light whose direct term the texels hold. With no lightmap on the
    // draw nothing here changes a bit of the light loop's arithmetic.
    let baked = bakedLightsOf(lightmapped, lightmapUv);
    var color = accumulateLights(surface, input.relativePosition, normal, viewDir,
                                 input.position.xy, draw.flags, baked);
    // AMBIENT OCCLUSION DARKENS THE AMBIENT TERM AND NOTHING ELSE. `rendering-post-processing`:
    // "AO SHALL modulate indirect lighting only; applying it to direct light SHALL be an explicitly
    // non-physical artistic option, off by default." The direct sum above is untouched unless that
    // option is on, and with no occlusion slot the arithmetic below is the frame's as it was.
    // THE IRRADIANCE VOLUME REPLACES THE FLAT AMBIENT, and AO still multiplies what it replaced
    // with. With no volume slot the ambient is the frame's as it was.
    //
    // A LIGHTMAPPED SURFACE TAKES THE LIGHTMAP AND NOTHING ELSE: `gi::exclusion_for()`'s rule, which
    // `lightmaps::write_lightmaps` consulted before it set `lightmapControl.w` — a mode that
    // excludes lightmaps leaves it at `kCyLightmapNone`, and every mode that admits one excludes
    // the volume for the surface that has it. The lightmap already holds the sky and the bounces,
    // so it replaces the flat ambient as the volume does, and a surface without one is lit exactly
    // as before.
    var ambientRadiance = cyFrame.ambientAndOcclusion.rgb;
    if (lightmapped)
    {
        ambientRadiance = lightmapAmbient(lightmapUv, normal);
    }
    else if (cyFrame.probeVolumeControl.x != kCyNoMaterialTexture)
    {
        ambientRadiance = probeVolumeAmbient(input.relativePosition, normal, ambientRadiance);
    }
    var ambient = surface.albedo * ambientRadiance * surface.occlusion;
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
    // THE FOG BETWEEN THE EYE AND THE SURFACE, last: what the medium transmits of the surface's
    // radiance and what it scatters toward the eye on the way. With no volume slot the colour is
    // the frame's as it was.
    if (cyFrame.volumetricFogControl.x != kCyNoMaterialTexture)
    {
        CyFrameFogVolume fog;
        fog.slot = cyFrame.volumetricFogControl.x;
        color = cyApplyFog(fog, color, input.relativePosition);
    }
    return float4(color, surface.opacity);
}

[shader("fragment")]
public float4 cyForwardFragment(CyForwardVertex input) : SV_Target
{
    let draw = cyDrawInstances[input.drawIndex];
    let instance = cyInstances[draw.instanceSlot];
    return cyShadeForward(surfaceOf(draw.material, instance.tint.rgb, input.uv), input);
}

// --- Skinned geometry -----------------------------------------------------------------------------
//
// A SKINNED DRAW BINDS THE SKINNING PASS'S OWN OUTPUT, whose frame is `render::PackedNormalTangent`:
// the octahedral pairs as 16-bit SIGNED normalised components, bound as `Rgba16Snorm`, so the
// vertex fetch hands these entries values in [-1, 1]. The rigid streams carry the same pairs
// remapped to [0, 1] as half floats (`pack_normal_stream`), and that is the range
// `decodeOctahedral` takes. These two entries move the signed pair into that range and call the
// rigid entry points, so every line after the fetch is the rigid path's and cannot drift from it.
// The rigid entries are unchanged, which is what keeps a frame with no skinned draw the frame it
// was before skinned draws existed — and the section is at the END of the file so that no line of
// the rigid entries moves, and their MSL, which carries #line directives, is the same text.
//
//   slangc ... -entry cySkinnedDepthVertex   -stage vertex -o frame_skinned_depth_vertex.spv
//   slangc ... -entry cySkinnedForwardVertex -stage vertex -o frame_skinned_forward_vertex.spv
//
// `src/rendering/pipeline/shaders/regenerate.py` runs both with the rest.

float4 cySkinnedFrameToStream(float4 frame)
{
    return frame * 0.5 + 0.5;
}

[shader("vertex")]
public CyDepthVertex cySkinnedDepthVertex(float3 modelPosition : POSITION, float4 frame : NORMAL,
                                          float3 previousModelPosition : PREVIOUS_POSITION)
{
    return cyDepthVertex(modelPosition, cySkinnedFrameToStream(frame), previousModelPosition);
}

[shader("vertex")]
public CyForwardVertex cySkinnedForwardVertex(float3 modelPosition : POSITION, float4 frame : NORMAL,
                                              float2 uv : TEXCOORD0, float2 lightmapUv : TEXCOORD1)
{
    return cyForwardVertex(modelPosition, cySkinnedFrameToStream(frame), uv, lightmapUv);
}
