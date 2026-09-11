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

// ================================================================================================
// THE COMPILED-MATERIAL VOCABULARY — M8.c task 1b.3
// ================================================================================================
//
// M7's material compiler emits Slang naming `CyMaterialContext`, `CySurface`, `CyClosure` and ten
// `cy_closure_*` functions, and `src/rendering/material/emit.h` says in as many words that
// "the text names an interface the engine's shader standard library supplies". It did not. Until
// this section, compiling one of those programs failed on the first identifier:
//
//     error[E30015]: undefined identifier 'CyMaterialContext'
//     error[E30015]: undefined identifier 'CySurface'
//     error[E30015]: undefined identifier 'CyClosure'
//
// The half that is MATERIAL-INDEPENDENT is here: the closure algebra and the surface it lands in.
// The half that is per material — the parameter struct, the attribute struct, the context that
// holds them and the texture slot constants — cannot be, because it is different for every
// material; `cy::rendering::material::emit_prelude` generates it, and this file is what that
// prelude imports.
//
// THE CLOSURE SET IS THE IR'S, OP FOR OP. `ir.h` documents each one's operands beside its
// enumerator — `Specular  // (colour: Vec3, roughness: Float)` — and every signature below is that
// comment. A signature that drifted from it would be a compile error in the first material that
// used the op, which is the right place for it to be caught.

/// The closure set a material's surface root evaluates to.
///
/// A LOWERED, BOUNDED REPRESENTATION rather than a tree: `material-compiler` requires the closure
/// set to be resolved at compile time, and the engine's light loop shades a `Surface`. So the
/// algebra below accumulates into five terms and `cyResolveSurface` projects them onto the
/// `Surface` the engine already knows how to light. A material that wants a lobe this struct has no
/// term for is a material that wants a new shading model, which `rendering-materials-and-shading`
/// makes a declared model rather than a graph node.
public struct CyClosure
{
    public float3 diffuse;
    public float3 specular;
    public float3 emission;
    /// Weighted mean roughness of the specular terms; `specularWeight` is its denominator.
    public float roughness;
    public float specularWeight;
    /// How much of the surface the closure covers. `cy_closure_scale` multiplies it.
    public float weight;
};

public CyClosure cyClosureZero()
{
    CyClosure closure;
    closure.diffuse = float3(0.0);
    closure.specular = float3(0.0);
    closure.emission = float3(0.0);
    closure.roughness = 0.0;
    closure.specularWeight = 0.0;
    closure.weight = 1.0;
    return closure;
}

public CyClosure cy_closure_diffuse(float3 colour)
{
    CyClosure closure = cyClosureZero();
    closure.diffuse = colour;
    return closure;
}

public CyClosure cy_closure_specular(float3 colour, float roughness)
{
    CyClosure closure = cyClosureZero();
    closure.specular = colour;
    closure.roughness = roughness;
    closure.specularWeight = 1.0;
    return closure;
}

/// `Coat // (roughness: Float)` — a white dielectric lobe over whatever it is layered onto.
public CyClosure cy_closure_coat(float roughness)
{
    CyClosure closure = cyClosureZero();
    closure.specular = float3(1.0);
    closure.roughness = roughness;
    closure.specularWeight = 1.0;
    return closure;
}

/// Transmission and subsurface both reach the engine's `Surface` as diffuse energy: neither has a
/// term of its own in a forward `Surface`, and losing the distinction here is better than inventing
/// a field the light loop does not read. `vfx-system`-style volumetrics are not this file's.
public CyClosure cy_closure_transmission(float3 colour)
{
    return cy_closure_diffuse(colour);
}

public CyClosure cy_closure_subsurface(float3 colour)
{
    return cy_closure_diffuse(colour);
}

/// Sheen is a wide, rough specular lobe.
public CyClosure cy_closure_sheen(float3 colour)
{
    return cy_closure_specular(colour, 0.9);
}

public CyClosure cy_closure_emission(float3 colour)
{
    CyClosure closure = cyClosureZero();
    closure.emission = colour;
    return closure;
}

/// `ClosureScale // (closure, weight: Float)`.
public CyClosure cy_closure_scale(CyClosure closure, float weight)
{
    CyClosure scaled = closure;
    scaled.diffuse *= weight;
    scaled.specular *= weight;
    scaled.emission *= weight;
    scaled.specularWeight *= weight;
    scaled.weight *= weight;
    return scaled;
}

/// `ClosureAdd` is variadic, 2 to 4 operands (`op_arity` returns `kVariadic`), so there are three
/// overloads and not one: Slang has no variadic user function and a fold written at the call site
/// would be the emitter's business rather than this file's.
public CyClosure cy_closure_add(CyClosure a, CyClosure b)
{
    CyClosure sum;
    sum.diffuse = a.diffuse + b.diffuse;
    sum.specular = a.specular + b.specular;
    sum.emission = a.emission + b.emission;
    sum.specularWeight = a.specularWeight + b.specularWeight;
    sum.roughness = (a.roughness * a.specularWeight) + (b.roughness * b.specularWeight);
    sum.roughness = sum.specularWeight > 0.0 ? sum.roughness / sum.specularWeight : 0.0;
    sum.weight = max(a.weight, b.weight);
    return sum;
}

public CyClosure cy_closure_add(CyClosure a, CyClosure b, CyClosure c)
{
    return cy_closure_add(cy_closure_add(a, b), c);
}

public CyClosure cy_closure_add(CyClosure a, CyClosure b, CyClosure c, CyClosure d)
{
    return cy_closure_add(cy_closure_add(a, b), cy_closure_add(c, d));
}

/// `ClosureLayer // (top, base): the top attenuates the base. NOT COMMUTATIVE — that is the whole
/// content`, which is `ir.h`'s own sentence, and it is why this is a distinct op rather than an add.
public CyClosure cy_closure_layer(CyClosure top, CyClosure base)
{
    let attenuation = saturate(1.0 - top.weight);
    return cy_closure_add(top, cy_closure_scale(base, attenuation));
}

/// What a generated program writes into. `surface.preview` is the node-preview path: an interior
/// value of a graph, which is a colour rather than a closure.
public struct CySurface
{
    public CyClosure closures;
    public float opacity;
    public float3 preview;
};

public CySurface cyDefaultSurface()
{
    CySurface surface;
    surface.closures = cyClosureZero();
    surface.opacity = 1.0;
    surface.preview = float3(0.0);
    return surface;
}

/// Project a compiled material's closure set onto the `Surface` the engine's light loop shades.
///
/// ONE DIRECTION, DELIBERATELY. The closure set is what a material AUTHORS and `Surface` is what the
/// frame LIGHTS; keeping the projection in one function is what lets the shading model change
/// without every material changing with it, which is the same argument `IMaterial` above is written
/// from.
public Surface cyResolveSurface(CySurface compiled)
{
    Surface surface = defaultSurface();
    let specularLuminance = dot(compiled.closures.specular, float3(0.2126, 0.7152, 0.0722));
    surface.albedo = compiled.closures.diffuse + compiled.closures.specular;
    surface.metallic = saturate(specularLuminance);
    surface.roughness = clamp(compiled.closures.roughness, 0.02, 1.0);
    surface.emission = compiled.closures.emission;
    surface.opacity = compiled.opacity;
    return surface;
}

// --- The material texture table -----------------------------------------------------------------
//
// SET 0, because `cy/backends/shader/reflection.h`'s convention says a bindless global table appears
// only in set 0. A material's own parameters are set 3 (per draw) and are declared by the generated
// prelude; what is global is the TABLE the parameters index into.

[[vk::binding(1, 0)]]
public Texture2D<float4> cyMaterialTextures[];
[[vk::binding(2, 0)]]
public SamplerState cyMaterialSampler;

/// Sample one slot of the global table with the IMPLICIT level of detail. Pixel stages only:
/// implicit derivatives do not exist in a vertex or compute stage, and Slang refuses the call there
/// rather than producing a shader that reads the wrong mip.
public float4 cyMaterialSampleTexture(uint bindlessIndex, float2 uv)
{
    return cyMaterialTextures[bindlessIndex].Sample(cyMaterialSampler, uv);
}

/// The same sample at an EXPLICIT level, which every stage can make.
///
/// The generated prelude calls this one, and the reason is stated where it is generated: a material
/// program is compiled and reflected long before it is placed in a stage, and the probe entry point
/// `assemble_translation_unit` appends is a compute shader. A lowering that knows it is producing a
/// pixel stage calls the implicit form above; nothing does yet, and that is recorded in
/// `slang_program.h` rather than papered over.
public float4 cyMaterialSampleTextureLevel(uint bindlessIndex, float2 uv, float level)
{
    return cyMaterialTextures[bindlessIndex].SampleLevel(cyMaterialSampler, uv, level);
}
