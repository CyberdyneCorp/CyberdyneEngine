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

#include CY_MATERIAL_MODULE

// --- What the frame hands every stage -------------------------------------------------------------
//
// FOUR EXPLICIT ROWS INSTEAD OF float4x4, the same decision samples/03-first-light and
// tests/render/shaders/conventions.slang make and for the same reason: a matrix in a constant block
// has a layout that is a property of the compiler's flags, and `mul(M, v)` against `mul(v, M)`
// silently transposes the result.

struct BeautyPush
{
    float4 viewProjection0;
    float4 viewProjection1;
    float4 viewProjection2;
    float4 viewProjection3;
    float4 sunToClip0;
    float4 sunToClip1;
    float4 sunToClip2;
    float4 sunToClip3;
    /// xyz: the sun's direction TOWARDS the light, camera-relative. w: the shadow depth bias.
    float4 sunDirection;
    /// xyz: the sun's illuminance in linear units. w: the shadow map's texel size.
    float4 sunColor;
    /// xyz: the sky's ambient irradiance, from the assembly's own sky table. w: unused.
    float4 ambient;
    /// x: the bindless slot of this material's tangent-space normal map, or 0xFFFFFFFF for none.
    /// y: the UV scale this surface tiles its material at. z: normal strength. w: unused.
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
    output.position = transform(push.sunToClip0, push.sunToClip1, push.sunToClip2,
                                push.sunToClip3, float4(position, 1.0));
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
    output.position = transform(push.viewProjection0, push.viewProjection1, push.viewProjection2,
                                push.viewProjection3, float4(position, 1.0));
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
float sunVisibility(float3 relativePosition)
{
    float4 lightClip = transform(push.sunToClip0, push.sunToClip1, push.sunToClip2,
                                 push.sunToClip3, float4(relativePosition, 1.0));
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
    float reference = projected.z + push.sunDirection.w;
    float total = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * push.sunColor.w;
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
        float3 tangentNormal = cyMaterialSampleTexture(normalSlot, input.uv).xyz;
        tangentNormal = (tangentNormal * 2.0) - 1.0;
        tangentNormal.xy *= push.surface.z;
        float3 tangent = normalize(input.tangent.xyz);
        float3 bitangent = cross(geometricNormal, tangent) * input.tangent.w;
        shadingNormal = normalize((tangent * tangentNormal.x) + (bitangent * tangentNormal.y) +
                                  (geometricNormal * tangentNormal.z));
    }

    float3 view = normalize(-input.relativePosition);
    float3 light = normalize(push.sunDirection.xyz);
    float normalDotLight = saturate(dot(shadingNormal, light));
    float visibility = sunVisibility(input.relativePosition);

    float3 f0 = computeF0(surface.albedo, surface.metallic);
    float3 diffuse = diffuseLambert(surface.albedo * (1.0 - surface.metallic));
    float3 specular = specularGgx(shadingNormal, view, light, surface.roughness, f0);

    float3 lit = (diffuse + specular) * push.sunColor.xyz * normalDotLight * visibility;

    // The ambient term is the assembly's own sky table, hemispherically weighted by the shading
    // normal so that an upward face is lit by the sky and a downward one is not. It is a
    // one-bounce approximation and the provenance says so: no global illumination pass ran.
    float sky = (dot(shadingNormal, float3(0.0, 1.0, 0.0)) * 0.5) + 0.5;
    lit += surface.albedo * push.ambient.xyz * sky * surface.occlusion;
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
    output.position = transform(push.viewProjection0, push.viewProjection1, push.viewProjection2,
                                push.viewProjection3, float4(position, 1.0));
    output.radiance = radiance;
    return output;
}

[shader("fragment")]
float4 skyFragment(SkyVarying input) : SV_Target
{
    return float4(input.radiance, 1.0);
}
