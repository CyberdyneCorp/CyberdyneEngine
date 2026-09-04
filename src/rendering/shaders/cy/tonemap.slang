// Tonemapping. Part of the engine's Slang standard library (`shader-system`).

module tonemap;

import cy.color;

/// Reinhard, extended with a white point. Cheap, and the reference the others are compared against.
public float3 tonemapReinhard(float3 linear, float whitePoint)
{
    let numerator = linear * (1.0 + (linear / (whitePoint * whitePoint)));
    return numerator / (1.0 + linear);
}

/// The ACES filmic approximation (Narkowicz). One polynomial, no matrices: it is the curve, not the
/// full ACES pipeline, and it is named as such so nobody mistakes it for a colour-managed path.
public float3 tonemapAcesApproximate(float3 linear)
{
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return saturate((linear * (a * linear + b)) / (linear * (c * linear + d) + e));
}

/// Exposure in stops, applied before any curve. Separated so the curve is a choice and the exposure
/// is not.
public float3 applyExposure(float3 linear, float exposureStops)
{
    return linear * exp2(exposureStops);
}
