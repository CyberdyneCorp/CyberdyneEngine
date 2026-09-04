// Colour space conversion. Part of the engine's Slang standard library (`shader-system`).
//
// THE ENGINE RENDERS IN LINEAR sRGB PRIMARIES and converts on the way out. These functions are the
// exact transfer function, not the 2.2 power approximation: the approximation is visibly wrong in
// the darkest few percent, which is exactly where a lighting bug hides.

module color;

public float3 srgbToLinear(float3 encoded)
{
    let low = encoded / 12.92;
    let high = pow((encoded + 0.055) / 1.055, 2.4);
    return select(encoded <= 0.04045, low, high);
}

public float3 linearToSrgb(float3 linear)
{
    let low = linear * 12.92;
    let high = 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
    return select(linear <= 0.0031308, low, high);
}

/// Rec. 709 luminance, which is the one that matches the primaries the engine renders in.
public float luminance(float3 linear)
{
    return dot(linear, float3(0.2126, 0.7152, 0.0722));
}
