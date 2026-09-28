// samples/12-beauty — the frame the beauty shot is drawn with. M11.c tasks 3.7, 6.1c and 7.1.
//
// ================================================================================================
// THIS FILE IS THE FRAGMENT-STAGE LOWERING THE ENGINE DOES NOT GENERATE
// ================================================================================================
//
// `slang_program.h` says it in as many words: "The probe entry point is a compute shader, not the
// frame's fragment shader ... wiring a compiled material into `src/rendering/pipeline/`'s forward
// pass is a permutation and pipeline-cache question". M11.c's spike measured what stands in for that
// wiring — 102 non-comment lines, per material, per attribute set, per stage — and this is that
// stand-in, written once and shared by every material in the shot.
//
// `CY_MATERIAL_MODULE` is substituted by `just capture-beauty-shot`: it is the path of the module
// `cy_material author` generated from the AUTHORED GRAPH — prelude, the emitter's own bytes, and
// nothing edited. `CY_MATERIAL_ENTRY` is the entry point name the engine's own
// `entry_point_name` produced. So the shading below is the compiled material's, not this file's, and
// the only thing this file decides is what a pixel stage has to decide: which attributes the
// material sees, and what the engine's light loop does with the surface it returns.
//
// ================================================================================================
// WHAT THIS FILE SAMPLES THAT THE MATERIAL PROGRAM CANNOT, AND WHY IT IS A FINDING
// ================================================================================================
//
// THE NORMAL MAP. `cy/material.slang`'s `CySurface` is `{ CyClosure closures; float opacity; float3
// preview; }` and `CyClosure` is five terms — diffuse, specular, emission, roughness, weight. There
// is NO NORMAL TERM anywhere in the compiled-material vocabulary, and `cyResolveSurface` therefore
// leaves `Surface::normal` at (0,0,1) whatever the material did. A material compiler with no normal
// output cannot express a normal-mapped material, and every textured surface in this engine would be
// smooth.
//
// So the tangent-space normal is sampled HERE, from the same bindless table and the same slot the
// material's own declarations named, and the artefact's provenance says so rather than implying the
// material program produced it. `docs/design/beauty-shot.md` states it and `m11c.toml` declares it
// as a gap with the rung that owes it.
//
// ================================================================================================
// THE TABLE IS AT THE BINDING THE STANDARD LIBRARY DECLARES
// ================================================================================================
//
// `cy/material.slang` declares `cyMaterialTextures[]` at (set 0, binding 1) and `cyMaterialSampler`
// at (set 0, binding 2), and this file imports that declaration rather than restating it — so the
// set the sample allocates is checked against the standard library by the Slang compiler rather than
// by a comment. The material's own parameter block is at (set 3, binding 0), which is the prelude's.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   see just/run.just's `capture-beauty-shot`: it runs `cy_material author`, then slangc once per
//   material and once for the sky and the shadow pass, and hands the .spv files to the sample.

import cy.material;
import cy.brdf;
import cy.decal;
import cy.packing;
import cy.shadow;
import cy.volumetric_fog;

// THIS FILE IS A PIXEL STAGE, SO IT SAYS SO BEFORE THE MATERIAL IS INCLUDED. The generated prelude's
// `cy_material_sample` takes the IMPLICIT level of detail only under `CY_MATERIAL_PIXEL_STAGE`, and
// without it every albedo and data map in the shot was `OpImageSampleExplicitLod ... Lod 0` — the
// cooked chain beneath level 0 was uploaded and never read (M11.c task 3.8, refuted at the gate by
// disassembling these modules). It is defined HERE rather than on the recipe's slangc line because
// this file is the one place that knows it is a fragment-stage lowering, so every compile of it gets
// the answer. The material program is called only from `sceneFragment`, where derivatives exist.
// `m11c:beauty-shot-reads-the-mip-chain` is the criterion; deleting this line turns it red.
#define CY_MATERIAL_PIXEL_STAGE 1
#include CY_MATERIAL_MODULE

// --- What the frame hands every stage -------------------------------------------------------------
//
// FOUR EXPLICIT ROWS INSTEAD OF float4x4, the same decision samples/03-first-light and
// tests/render/shaders/conventions.slang make and for the same reason: a matrix in a constant block
// has a layout that is a property of the compiler's flags, and `mul(M, v)` against `mul(v, M)`
// silently transposes the result.

/// What the frame is, for every stage. Set 2 is "per view" in `cy/backends/shader/reflection.h`'s
/// convention, and that is exactly what this is.
///
/// A UNIFORM BLOCK AND NOT PUSH CONSTANTS, measured rather than preferred: eleven `float4` is 176
/// bytes and Vulkan guarantees 128. `rhi::kMaxPushConstantBytes` says the same thing, and a layout
/// that exceeded it would fail at pipeline creation on some devices and not on this one.
struct BeautyFrame
{
    float4 viewProjection0;
    float4 viewProjection1;
    float4 viewProjection2;
    float4 viewProjection3;
    float4 sunToClip0;
    float4 sunToClip1;
    float4 sunToClip2;
    float4 sunToClip3;
    /// xyz: the direction TOWARDS the sun, camera-relative. w: the shadow depth bias.
    float4 sunDirection;
    /// xyz: the sun's illuminance, linear. w: one texel of the shadow map.
    float4 sunColor;
    /// xyz: the sky's mean radiance, from the engine's own `compose_sky_lighting`.
    /// w: how far along the surface normal the shadow lookup moves, in metres.
    float4 ambient;
    /// xyz: where the eye is, in the same camera-RELATIVE space the vertices are baked into. Zero
    /// for the still, and non-zero for every frame of the turntable but the first — the geometry is
    /// baked once against the shot's own camera and the moving camera is expressed as an offset
    /// from it, which is what lets a video cost one vertex buffer rather than two hundred and forty.
    float4 eye;
    /// SOFT SHADOWS, read only by `sceneFragmentSoft`: `cy.shadow`'s `PcssShape` — x the penumbra
    /// UV per unit of light-space depth, y and z the smallest and largest kernel in UV, w the
    /// blocker search's taps. APPENDED, so every entry point that predates it reads the block it
    /// always read.
    float4 softShadow;
    /// x: the filter's taps. y: 1 when the ambient occlusion term is bound and read. z: 1 when the
    /// contact term is. w: unused.
    uint4 softControl;
    /// DECALS: x the material table slot holding the decal table, or 0xFFFFFFFF — the default,
    /// which draws the frame this file always drew. y: the table texture's rows. z: flags, of which
    /// this file reads none — the lists are always the table's, because this program binds no
    /// cluster buffers. APPENDED, like `softShadow`.
    uint4 decalControl;
    /// VOLUMETRIC FOG, read by every scene and sky entry point: x is 1 when `fog::FogPass`'s volume
    /// is bound at (set 1, binding 4) and every fragment is seen through it, and 0 — the published
    /// frame — when it is not, which leaves every fragment exactly as it was. APPENDED, for the
    /// reason `softShadow` gives. yzw: unused.
    uint4 fogControl;
    /// MOTION BLUR, read only by `scenePrepassMotionFragment`: LAST FRAME'S baked-space to clip
    /// transform, as `viewProjection` is this frame's. The geometry is baked once and the camera is
    /// what moves, so this and `viewProjection` are the whole of the frame's motion. APPENDED, so
    /// every entry point that predates it reads the block it always read.
    float4 previousViewProjection0;
    float4 previousViewProjection1;
    float4 previousViewProjection2;
    float4 previousViewProjection3;
};

[[vk::binding(0, 2)]]
ConstantBuffer<BeautyFrame> frame;

/// The only thing that differs per draw. Sixteen bytes, which every Vulkan device has.
///
/// x: the bindless slot of this surface's tangent-space normal map, bit-cast, or 0xFFFFFFFF.
/// y: the UV scale this surface tiles its material at. z: normal strength.
/// w: the bindless slot of its packed data map, bit-cast — see the occlusion note below.
struct BeautyPush
{
    float4 surface;
};

[[vk::push_constant]] BeautyPush push;

float4 transform(float4 r0, float4 r1, float4 r2, float4 r3, float4 v)
{
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

// --- The shadow map -------------------------------------------------------------------------------
//
// Reversed Z, like every other depth buffer in this engine: near is 1, far is 0, cleared to 0,
// compared GreaterOrEqual. The occluder nearest the light holds the LARGEST value, which is why the
// bias below is ADDED to the reference — it moves the fragment towards the light, and that is the
// direction that removes acne under this convention and causes it under the other one.

struct ShadowVarying
{
    float4 position : SV_Position;
};

[shader("vertex")]
ShadowVarying shadowVertex(float3 position : POSITION)
{
    ShadowVarying output;
    output.position = transform(frame.sunToClip0, frame.sunToClip1, frame.sunToClip2,
                                frame.sunToClip3, float4(position, 1.0));
    return output;
}

// --- The scene ------------------------------------------------------------------------------------

struct SceneVarying
{
    float4 position : SV_Position;
    float3 relativePosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
};

[shader("vertex")]
SceneVarying sceneVertex(float3 position : POSITION, float3 normal : NORMAL,
                         float4 tangent : TANGENT, float2 uv : TEXCOORD0)
{
    SceneVarying output;
    output.position = transform(frame.viewProjection0, frame.viewProjection1, frame.viewProjection2,
                                frame.viewProjection3, float4(position, 1.0));
    output.relativePosition = position;
    output.normal = normal;
    output.tangent = tangent;
    output.uv = uv * push.surface.y;
    return output;
}

[[vk::binding(0, 1)]] Texture2D<float> shadowMap;
[[vk::binding(1, 1)]] SamplerComparisonState shadowSampler;

/// Percentage-closer filtering over a 3x3 kernel. Nine taps rather than one, because a single tap
/// is a hard, aliased edge and the whole point of this artefact is what the picture looks like.
float sunVisibility(float3 relativePosition, float3 geometricNormal)
{
    // THE LOOKUP IS OFFSET ALONG THE SURFACE NORMAL, not only biased in depth. A constant depth bias
    // is a constant in LIGHT space and the error it has to cover is proportional to the slope, so at
    // a sun this low it either leaves acne or detaches every contact shadow. Moving the lookup a few
    // centimetres off the surface is the same correction expressed where the error actually is.
    float3 offset = relativePosition + (geometricNormal * frame.ambient.w);
    float4 lightClip = transform(frame.sunToClip0, frame.sunToClip1, frame.sunToClip2,
                                 frame.sunToClip3, float4(offset, 1.0));
    if (lightClip.w <= 0.0)
    {
        return 1.0;
    }
    float3 projected = lightClip.xyz / lightClip.w;
    float2 uv = (projected.xy * float2(0.5, 0.5)) + float2(0.5, 0.5);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0;
    }
    float reference = projected.z + frame.sunDirection.w;
    float total = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * frame.sunColor.w;
            total += shadowMap.SampleCmpLevelZero(shadowSampler, uv + offset, reference);
        }
    }
    return total / 9.0;
}

/// The shadow map as `cy.shadow`'s soft filter reads it: the stored depth of the nearest texel,
/// loaded rather than compared. The comparison sampler above stays the hard filter's.
struct BeautyShadowMap : IShadowDepthSource
{
    float storedDepth(float2 uv)
    {
        uint width;
        uint height;
        shadowMap.GetDimensions(width, height);
        let texel = clamp(int2(floor(uv * float2(width, height))), int2(0, 0),
                          int2(int(width) - 1, int(height) - 1));
        return shadowMap.Load(int3(texel, 0));
    }
};

/// Percentage-closer soft shadows over the sun's map, with the lookup `sunVisibility` makes: the
/// same normal offset, the same bias, and a kernel sized by the sun's angular radius instead of a
/// fixed 3x3.
float softSunVisibility(float3 relativePosition, float3 geometricNormal, float2 pixel)
{
    float3 offset = relativePosition + (geometricNormal * frame.ambient.w);
    float4 lightClip = transform(frame.sunToClip0, frame.sunToClip1, frame.sunToClip2,
                                 frame.sunToClip3, float4(offset, 1.0));
    if (lightClip.w <= 0.0)
    {
        return 1.0;
    }
    float3 projected = lightClip.xyz / lightClip.w;
    float2 uv = (projected.xy * float2(0.5, 0.5)) + float2(0.5, 0.5);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0;
    }
    PcssShape shape;
    shape.penumbraPerDepth = frame.softShadow.x;
    shape.minRadius = frame.softShadow.y;
    shape.maxRadius = frame.softShadow.z;
    shape.blockerTaps = int(max(frame.softShadow.w, 1.0));
    shape.filterTaps = int(max(frame.softControl.x, 1u));
    BeautyShadowMap map;
    return pcssVisibility(map, uv, projected.z + frame.sunDirection.w, shape,
                          shadowDiscRotation(pixel));
}

/// THE AMBIENT OCCLUSION TERM, `occlusion::AmbientOcclusionPass`'s target: rgb the bent normal,
/// a the visibility. Read only by `sceneFragmentOccluded`, so a frame with the setting off binds
/// nothing here and draws with `sceneFragment` — the entry point the frame always had.
[[vk::binding(2, 1)]] Texture2D<float4> ambientOcclusion;

/// THE CONTACT TERM, `contact_shadows::ContactShadowPass`'s target: visibility in `.r`. Read only by
/// `sceneFragmentSoft`, and only when `softControl.z` says it was written.
[[vk::binding(3, 1)]] Texture2D<float4> contactShadows;

/// The decal table, read out of the material table at the slot `decalControl.x` names: one word per
/// texel, as `decals::DecalTableTexture` uploaded it.
struct BeautyDecalSource : ICyDecalSource
{
    uint word(uint index)
    {
        let uv = cyDecalWordUv(index, frame.decalControl.y);
        return cyDecalWordFromTexel(cyMaterialSampleTextureLevel(frame.decalControl.x, uv, 0.0));
    }
    float mask(uint slot, float2 uv)
    {
        return cyMaterialSampleTextureLevel(slot, uv, 0.0).a;
    }
};

/// The decals that reach this pixel, applied to the material's surface and to the shading normal
/// the normal map produced — BEFORE the sun and the sky light either. The receiver's position is
/// in the shot camera's space, which is the table's; its distance is from the eye actually drawn
/// from, which the turntable moves.
void applyDecals(inout Surface surface, inout float3 shadingNormal, float3 geometricNormal,
                 float3 relativePosition, float2 pixel)
{
    BeautyDecalSource source;
    CyDecalReceiver receiver;
    receiver.relativePosition = relativePosition;
    receiver.geometricNormal = geometricNormal;
    receiver.eyeDistance = length(relativePosition - frame.eye.xyz);
    // Every surface in the shot is in the default layer.
    receiver.channels = 1u;
    CyDecalSurface decalled;
    decalled.albedo = surface.albedo;
    decalled.roughness = surface.roughness;
    decalled.metallic = surface.metallic;
    decalled.emission = surface.emission;
    decalled.normal = shadingNormal;
    cyApplyTableDecals(source, receiver, pixel, decalled);
    surface.albedo = decalled.albedo;
    surface.roughness = clamp(decalled.roughness, 0.02, 1.0);
    surface.metallic = saturate(decalled.metallic);
    surface.emission = decalled.emission;
    shadingNormal = decalled.normal;
}

/// THE FOG VOLUME, `fog::FogPass`'s target: `cy.volumetric_fog`'s layout, read texel by texel —
/// the lookup interpolates the columns and the slices itself.
[[vk::binding(4, 1)]] Texture2D<float4> fogVolume;

struct BeautyFogVolume : ICyFogVolumeSource
{
    float4 texel(int2 coordinate)
    {
        return fogVolume.Load(int3(coordinate, 0));
    }
};

/// A fragment's radiance as the eye receives it through the fog: attenuated by what the medium
/// between transmits and added to by what it scatters toward the eye. `relativePosition` is in
/// the space the geometry is baked into, which is the space the volume's columns start from.
float4 throughFog(float4 shaded, float3 relativePosition)
{
    if (frame.fogControl.x == 0u)
    {
        return shaded;
    }
    BeautyFogVolume volume;
    return float4(cyApplyFog(volume, shaded.rgb, relativePosition), shaded.a);
}

/// The surface's shading, with the ambient term's visibility supplied by the caller: 1 for
/// `sceneFragment`, the ambient occlusion term for `sceneFragmentOccluded`. The sun's visibility is
/// the hard 3x3 filter's unless the caller supplies its own — `sceneFragmentSoft` does.
float4 shadeScene(SceneVarying input, float ambientVisibility)
{
    return shadeSceneWithSun(input, ambientVisibility, -1.0);
}

/// `shadeScene`, with the sun's visibility supplied; a negative `sunOverride` computes the hard
/// filter's as `sceneFragment` always has.
float4 shadeSceneWithSun(SceneVarying input, float ambientVisibility, float sunOverride)
{
    // THE MATERIAL PROGRAM, CALLED. Everything above this line is the frame's; everything the
    // surface says is the authored graph's, lowered by `compile_material` and emitted by the
    // engine's own emitter.
    CyMaterialContext ctx;
    ctx.params = cyMaterialParameters;
    ctx.attributes = cyZeroAttributes();
    ctx.attributes.uv0 = input.uv;

    CySurface compiled = cyDefaultSurface();
    CY_MATERIAL_ENTRY(ctx, compiled);
    Surface surface = cyResolveSurface(compiled);

    float3 geometricNormal = normalize(input.normal);
    float3 shadingNormal = geometricNormal;
    // The normal map, sampled here rather than by the material — see the header. `push.surface.x`
    // carries the slot the material's own texture declarations named, so the table and the index
    // are still the material's; only the SAMPLE is this file's.
    uint normalSlot = asuint(push.surface.x);
    if (normalSlot != 0xFFFFFFFFu)
    {
        // BC5 STORES TWO CHANNELS AND Z IS RECONSTRUCTED. `texture.h` chose the format for exactly
        // that reason — "Z is reconstructed in the shader from X and Y, so storing it would cost a
        // third of the budget for a value the shader already knows" — and a shader that read `.xyz`
        // from it gets a blue channel of ZERO, which decodes to z = -1 and turns every normal inside
        // out. That is how this was found: every upward-facing surface in the frame was black, and
        // nothing else in the picture said so.
        float2 packed = cyMaterialSampleTexture(normalSlot, input.uv).xy;
        float2 tangentXY = ((packed * 2.0) - 1.0) * push.surface.z;
        float3 tangentNormal =
            float3(tangentXY, sqrt(saturate(1.0 - dot(tangentXY, tangentXY))));
        float3 tangent = normalize(input.tangent.xyz);
        float3 bitangent = cross(geometricNormal, tangent) * input.tangent.w;
        shadingNormal = normalize((tangent * tangentNormal.x) + (bitangent * tangentNormal.y) +
                                  (geometricNormal * tangentNormal.z));
    }

    // DECALS CHANGE THE SURFACE BEFORE IT IS LIT: the scorch in a pillar's shadow is as dark as the
    // gravel around it. No table, the default, and nothing here runs.
    if (frame.decalControl.x != 0xFFFFFFFFu)
    {
        applyDecals(surface, shadingNormal, geometricNormal, input.relativePosition,
                    input.position.xy);
    }

    float3 view = normalize(frame.eye.xyz - input.relativePosition);
    float3 light = normalize(frame.sunDirection.xyz);
    float normalDotLight = saturate(dot(shadingNormal, light));
    float visibility = sunOverride < 0.0 ? sunVisibility(input.relativePosition, geometricNormal)
                                         : sunOverride;

    float3 f0 = computeF0(surface.albedo, surface.metallic);
    float3 diffuse = diffuseLambert(surface.albedo * (1.0 - surface.metallic));
    float3 specular = specularGgx(shadingNormal, view, light, surface.roughness, f0);

    float3 lit = (diffuse + specular) * frame.sunColor.xyz * normalDotLight * visibility;

    // The ambient term is the assembly's own sky table, hemispherically weighted by the shading
    // normal so that an upward face is lit by the sky and a downward one is not. It is a
    // one-bounce approximation and the provenance says so: no global illumination pass ran.
    float sky = (dot(shadingNormal, float3(0.0, 1.0, 0.0)) * 0.5) + 0.5;
    // THE OCCLUSION IS SAMPLED BY THE FRAME TOO, and for the same reason the normal is:
    // `CySurface` has no occlusion term, so `cyResolveSurface` leaves `Surface::occlusion` at 1
    // whatever the material did. The green channel of the material's own packed data map is what the
    // generator wrote an ambient occlusion into, and reading it here is the difference between a
    // mortar joint that is a dark line and one that is a colour change. Declared as a gap beside the
    // normal: both belong in the closure vocabulary.
    float occlusion = surface.occlusion;
    uint dataSlot = asuint(push.surface.w);
    if (dataSlot != 0xFFFFFFFFu)
    {
        occlusion *= cyMaterialSampleTexture(dataSlot, input.uv).y;
    }
    // AMBIENT OCCLUSION DARKENS THE SKY TERM AND NOTHING ELSE: `lit` above is the sun, which
    // `rendering-post-processing` requires AO to leave alone by default.
    lit += surface.albedo * frame.ambient.xyz * sky * (occlusion * ambientVisibility);
    lit += surface.emission;
    return float4(lit, 1.0);
}

[shader("fragment")]
float4 sceneFragment(SceneVarying input) : SV_Target
{
    return throughFog(shadeScene(input, 1.0), input.relativePosition);
}

/// The same surface with the ambient occlusion term applied to its sky light. The target is the
/// frame's own extent, so the fragment's pixel is the term's texel.
[shader("fragment")]
float4 sceneFragmentOccluded(SceneVarying input) : SV_Target
{
    let visibility = ambientOcclusion.Load(int3(int2(input.position.xy), 0)).a;
    return throughFog(shadeScene(input, visibility), input.relativePosition);
}

/// SOFT AND CONTACT SHADOWS: the sun through `cy.shadow`'s percentage-closer soft filter, darkened
/// further by the contact term where it found an occluder — the darker of the two, because the
/// term is an addition to the map's answer and not a replacement for it — and the sky term through
/// the ambient occlusion term when that setting is on as well. Drawn against the prepass depth, as
/// `sceneFragmentOccluded` is.
[shader("fragment")]
float4 sceneFragmentSoft(SceneVarying input) : SV_Target
{
    let pixel = int2(input.position.xy);
    float3 geometricNormal = normalize(input.normal);
    float sun = softSunVisibility(input.relativePosition, geometricNormal, input.position.xy);
    if (frame.softControl.z != 0u)
    {
        sun = min(sun, contactShadows.Load(int3(pixel, 0)).r);
    }
    float ambient = 1.0;
    if (frame.softControl.y != 0u)
    {
        ambient = ambientOcclusion.Load(int3(pixel, 0)).a;
    }
    return throughFog(shadeSceneWithSun(input, ambient, sun), input.relativePosition);
}

/// THE DEPTH AND NORMAL PREPASS the ambient occlusion stage reads, drawn with `sceneVertex` so its
/// depth is the depth the scene pass then tests against. The GEOMETRIC normal, camera-relative and
/// octahedral in `.xy` — `rendering-forward-clustered`'s `DepthNormal` layout, which is what the
/// engine's own prepass writes; the normal map's detail is below the horizon search's scale.
[shader("fragment")]
float4 scenePrepassFragment(SceneVarying input) : SV_Target
{
    return float4(encodeOctahedral(normalize(input.normal)), 1.0, 1.0);
}

/// The same prepass with the frame's motion vectors, for motion blur: the normal as above, and in
/// the second target where this surface was LAST frame, in normalised screen space pointing at the
/// past — `temporal-rendering`'s convention and the one `cy/frame.slang`'s prepass writes. Drawn
/// with `sceneVertex`, like the prepass above, so the depth the scene pass tests is this pass's;
/// both positions are pushed through the interpolated baked-space position, which is exact per
/// pixel. No jitter to remove: this frame is supersampled and runs no temporal stage.
struct BeautyPrepassMotion
{
    float4 normalRoughness : SV_Target0;
    float2 velocity : SV_Target1;
};

[shader("fragment")]
BeautyPrepassMotion scenePrepassMotionFragment(SceneVarying input)
{
    let point = float4(input.relativePosition, 1.0);
    let current = transform(frame.viewProjection0, frame.viewProjection1, frame.viewProjection2,
                            frame.viewProjection3, point);
    let previous = transform(frame.previousViewProjection0, frame.previousViewProjection1,
                             frame.previousViewProjection2, frame.previousViewProjection3, point);
    let currentNdc = current.xy / current.w;
    let previousNdc = previous.xy / previous.w;
    BeautyPrepassMotion output;
    output.normalRoughness = float4(encodeOctahedral(normalize(input.normal)), 1.0, 1.0);
    output.velocity = float2((previousNdc.x - currentNdc.x) * 0.5,
                             (currentNdc.y - previousNdc.y) * 0.5);
    return output;
}

// --- The sky --------------------------------------------------------------------------------------
//
// A dome whose every vertex carries the radiance `rendering::sky::compose_sky()` answered for its
// direction — the engine's own atmosphere, its own multiple-scattering table and its own volumetric
// cloud march, evaluated on the processor once per dome vertex. Exactly what samples/10-world does,
// for the same reason: the sky in this picture is the engine's sky and not a gradient.

struct SkyVarying
{
    float4 position : SV_Position;
    float3 radiance : TEXCOORD0;
    /// The dome's point, camera-relative, for the fog in front of it: the lookup clamps it to the
    /// volume's far plane, so the sky is seen through the fog the volume holds and none beyond.
    float3 relativePosition : TEXCOORD1;
};

[shader("vertex")]
SkyVarying skyVertex(float3 position : POSITION, float3 radiance : COLOR0)
{
    SkyVarying output;
    output.position = transform(frame.viewProjection0, frame.viewProjection1, frame.viewProjection2,
                                frame.viewProjection3, float4(position, 1.0));
    output.radiance = radiance;
    output.relativePosition = position;
    return output;
}

[shader("fragment")]
float4 skyFragment(SkyVarying input) : SV_Target
{
    return throughFog(float4(input.radiance, 1.0), input.relativePosition);
}
