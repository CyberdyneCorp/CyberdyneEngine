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

[shader("fragment")]
float4 sceneFragment(SceneVarying input) : SV_Target
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

    float3 view = normalize(frame.eye.xyz - input.relativePosition);
    float3 light = normalize(frame.sunDirection.xyz);
    float normalDotLight = saturate(dot(shadingNormal, light));
    float visibility = sunVisibility(input.relativePosition, geometricNormal);

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
    lit += surface.albedo * frame.ambient.xyz * sky * occlusion;
    lit += surface.emission;
    return float4(lit, 1.0);
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
};

[shader("vertex")]
SkyVarying skyVertex(float3 position : POSITION, float3 radiance : COLOR0)
{
    SkyVarying output;
    output.position = transform(frame.viewProjection0, frame.viewProjection1, frame.viewProjection2,
                                frame.viewProjection3, float4(position, 1.0));
    output.radiance = radiance;
    return output;
}

[shader("fragment")]
float4 skyFragment(SkyVarying input) : SV_Target
{
    return float4(input.radiance, 1.0);
}
