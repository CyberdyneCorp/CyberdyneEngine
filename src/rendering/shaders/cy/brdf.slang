// The core BRDF. Part of the engine's Slang standard library (`shader-system`).
//
// Cook-Torrance with GGX/Trowbridge-Reitz, Smith height-correlated visibility and Schlick's
// Fresnel. The visibility term is the *height-correlated* one and it returns G / (4 NoL NoV), so
// the specular lobe is `D * V * F` with no stray denominator — which is the arrangement that stops
// a `4 * NoL * NoV` appearing in one shader and not another.

module brdf;

/// GGX normal distribution.
public float distributionGgx(float normalDotHalf, float roughness)
{
    let alpha = roughness * roughness;
    let alphaSquared = alpha * alpha;
    let denominator = normalDotHalf * normalDotHalf * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(3.14159265 * denominator * denominator, 1e-7);
}

/// Smith height-correlated visibility, already divided by 4·NoL·NoV.
public float visibilitySmithGgxCorrelated(float normalDotView, float normalDotLight,
                                          float roughness)
{
    let alpha = roughness * roughness;
    let alphaSquared = alpha * alpha;
    let view = normalDotLight * sqrt(normalDotView * normalDotView * (1.0 - alphaSquared) +
                                     alphaSquared);
    let light = normalDotView * sqrt(normalDotLight * normalDotLight * (1.0 - alphaSquared) +
                                     alphaSquared);
    return 0.5 / max(view + light, 1e-7);
}

public float3 fresnelSchlick(float3 f0, float viewDotHalf)
{
    let factor = pow(saturate(1.0 - viewDotHalf), 5.0);
    return f0 + (1.0 - f0) * factor;
}

/// Lambert, already divided by pi.
public float3 diffuseLambert(float3 albedo)
{
    return albedo * (1.0 / 3.14159265);
}

/// The full specular lobe for one light direction.
public float3 specularGgx(float3 normal, float3 view, float3 light, float roughness, float3 f0)
{
    let half = normalize(view + light);
    let normalDotView = saturate(dot(normal, view)) + 1e-5;
    let normalDotLight = saturate(dot(normal, light));
    let normalDotHalf = saturate(dot(normal, half));
    let viewDotHalf = saturate(dot(view, half));

    let d = distributionGgx(normalDotHalf, roughness);
    let v = visibilitySmithGgxCorrelated(normalDotView, normalDotLight, roughness);
    let f = fresnelSchlick(f0, viewDotHalf);
    return d * v * f;
}

/// The reflectance at normal incidence for a metallic-roughness surface. Dielectrics reflect four
/// percent; a metal's reflectance is its albedo.
public float3 computeF0(float3 albedo, float metallic)
{
    return lerp(float3(0.04), albedo, metallic);
}
