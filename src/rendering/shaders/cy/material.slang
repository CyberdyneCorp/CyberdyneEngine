// The material interface. Part of the engine's Slang standard library (`shader-system`).
//
// `shader-system` — "Material author writes a surface function": a custom material implements the
// surface interface, populating albedo, normal, roughness, metallic and emission, "and the engine
// SHALL supply vertex processing, light iteration, and output composition".
//
// THAT SENTENCE IS THIS FILE. `IMaterial` has exactly one method and it returns a `Surface`. A
// material cannot see a light, a cluster, a shadow atlas or a render target, so the engine keeps
// control of the light loop and the pass structure — which is what lets the pipeline change (a
// visibility buffer at M10, a different light assignment at M7) without every material changing
// with it.
//
// A SLANG INTERFACE, NOT A PREPROCESSOR HOOK. `shader-system` orders the variation mechanisms and
// puts generics and interfaces above preprocessor permutations; a material is a type argument, so
// two materials produce two specialisations of one shading function rather than two textual copies
// of it.
//
// M7's material compiler generates a `struct` implementing this interface and nothing else. That is
// the extension point the specification calls "a defined extension point", and it is deliberately
// this small.

module material;

import cy.brdf;
import cy.light;

/// What the engine's vertex processing hands a surface function.
///
/// Camera-relative (design.md §3): `positionRelativeToCamera` is never world space.
public struct SurfaceInput
{
    public float3 positionRelativeToCamera;
    public float3 normal;
    public float4 tangent;  // w carries the bitangent sign
    public float2 uv0;
    public float2 uv1;
    public float4 vertexColor;
};

/// What a surface function produces. The engine composes the output from this and nothing else.
public struct Surface
{
    public float3 albedo;
    /// Tangent space. The engine transforms it; a material that wants world space has to say so by
    /// producing a tangent-space normal of (0,0,1) and rotating in its own code, which is
    /// deliberately the awkward path.
    public float3 normal;
    public float roughness;
    public float metallic;
    public float3 emission;
    public float occlusion;
    public float opacity;
};

public Surface defaultSurface()
{
    Surface surface;
    surface.albedo = float3(0.5);
    surface.normal = float3(0.0, 0.0, 1.0);
    surface.roughness = 0.5;
    surface.metallic = 0.0;
    surface.emission = float3(0.0);
    surface.occlusion = 1.0;
    surface.opacity = 1.0;
    return surface;
}

/// The one thing a material implements.
public interface IMaterial
{
    Surface evaluateSurface(SurfaceInput input);
};

/// The engine's light loop, over one light. Materials never call this; the pass does.
public float3 shadeSurfaceWithLight(Surface surface, float3 worldNormal, float3 viewDirection,
                                    LightSample sample)
{
    let normalDotLight = saturate(dot(worldNormal, sample.direction));
    if (normalDotLight <= 0.0)
    {
        return float3(0.0);
    }
    let f0 = computeF0(surface.albedo, surface.metallic);
    let diffuse = diffuseLambert(surface.albedo * (1.0 - surface.metallic));
    let specular = specularGgx(worldNormal, viewDirection, sample.direction, surface.roughness,
                               f0);
    return (diffuse + specular) * sample.illuminance * normalDotLight;
}

/// The fallback material: the one a draw gets when its own material failed to compile.
/// `rendering-materials` requires a fallback rather than a missing draw, and it is deliberately
/// obvious rather than plausible.
public struct FallbackMaterial : IMaterial
{
    public Surface evaluateSurface(SurfaceInput input)
    {
        Surface surface = defaultSurface();
        surface.albedo = float3(1.0, 0.0, 1.0);
        surface.roughness = 1.0;
        return surface;
    }
};
