// Light evaluation. Part of the engine's Slang standard library (`shader-system`).
//
// PHYSICAL UNITS, which is what `rendering-lights-and-shadows` requires: a punctual light carries
// luminous intensity in candela and an area light luminance in nits. The inverse-square falloff is
// the physical one, with a smooth window rather than a hard cut so a light does not pop as it
// leaves its range.

module light;

public static const uint kLightDirectional = 0;
public static const uint kLightPoint = 1;
public static const uint kLightSpot = 2;

/// One light, as the GPU scene publishes it.
///
/// CAMERA-RELATIVE (design.md §3): `position` is relative to the camera, never world space. That is
/// a decision this struct records rather than a comment somewhere else, because a world-space
/// position here is precisely the field that would work at the origin and jitter a million units
/// out.
public struct Light
{
    public float3 positionRelativeToCamera;
    public float range;
    public float3 direction;
    public float intensity;      // candela for punctual lights
    public float3 color;         // linear, unit luminance
    public uint kind;
    public float2 spotScaleBias; // smooth cone attenuation, precomputed on the CPU
    public float2 padding;
};

/// The sample one light contributes at a shading point.
public struct LightSample
{
    public float3 direction;    // towards the light, normalised
    public float3 illuminance;  // colour times attenuated intensity
    public float attenuation;
};

/// Inverse-square falloff windowed to `range`, so the light reaches zero at its bound rather than
/// being clipped there.
float distanceAttenuation(float distanceSquared, float range)
{
    let factor = distanceSquared / max(range * range, 1e-6);
    let smoothWindow = saturate(1.0 - factor * factor);
    return (smoothWindow * smoothWindow) / max(distanceSquared, 1e-4);
}

public LightSample evaluateLight(Light light, float3 surfaceRelativeToCamera)
{
    LightSample result;
    if (light.kind == kLightDirectional)
    {
        result.direction = -light.direction;
        result.attenuation = 1.0;
        result.illuminance = light.color * light.intensity;
        return result;
    }

    let toLight = light.positionRelativeToCamera - surfaceRelativeToCamera;
    let distanceSquared = dot(toLight, toLight);
    result.direction = toLight * rsqrt(max(distanceSquared, 1e-8));
    result.attenuation = distanceAttenuation(distanceSquared, light.range);

    if (light.kind == kLightSpot)
    {
        let cosine = dot(-result.direction, light.direction);
        let cone = saturate(cosine * light.spotScaleBias.x + light.spotScaleBias.y);
        result.attenuation *= cone * cone;
    }
    result.illuminance = light.color * (light.intensity * result.attenuation);
    return result;
}
