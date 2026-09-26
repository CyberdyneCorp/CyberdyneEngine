// Shadow sampling. Part of the engine's Slang standard library (`shader-system`).
//
// The comparison is GreaterEqual because the engine is reversed-Z (design.md §3). A shadow map
// written with the engine's projection stores near at 1 and far at 0, so "closer than" is ">".
//
// SAMPLE COUNT IS A SPECIALIZATION CONSTANT, not a preprocessor permutation: `shader-system`'s
// "Sample count as a specialization constant" scenario is exactly this case, and it means a quality
// change creates a new pipeline from the same SPIR-V rather than a new compilation.

module shadow;

import cy.sampling;

/// Declared with an explicit id so cy::shader::PermutationSet can name it. The id is the contract
/// between the shader and the engine's permutation table, and
/// `PermutationSet::validate_specialization_ids` checks that the two agree.
[SpecializationConstant]
public const int kShadowSampleCount = 4;

/// One hardware-compared tap.
public float sampleShadowTap(Texture2D<float> atlas, SamplerComparisonState comparison,
                             float2 uv, float referenceDepth)
{
    return atlas.SampleCmpLevelZero(comparison, uv, referenceDepth);
}

/// A Poisson-ish disc of taps around `uv`, rotated per pixel so the pattern does not band.
public float samplePercentageCloserFilter(Texture2D<float> atlas,
                                          SamplerComparisonState comparison, float2 uv,
                                          float referenceDepth, float radius, float rotation)
{
    let cosine = cos(rotation);
    let sine = sin(rotation);
    float total = 0.0;
    for (int index = 0; index < kShadowSampleCount; ++index)
    {
        let offset = hammersley(uint(index), uint(kShadowSampleCount)) * 2.0 - 1.0;
        let rotated = float2(offset.x * cosine - offset.y * sine,
                             offset.x * sine + offset.y * cosine);
        total += sampleShadowTap(atlas, comparison, uv + rotated * radius, referenceDepth);
    }
    return total / float(kShadowSampleCount);
}

/// A depth bias that grows with the angle between the surface and the light, which is the one that
/// removes acne without introducing peter-panning at grazing angles.
public float slopeScaledBias(float normalDotLight, float constantBias, float slopeBias)
{
    let tangent = sqrt(saturate(1.0 - normalDotLight * normalDotLight)) /
                  max(normalDotLight, 1e-3);
    return constantBias + slopeBias * min(tangent, 4.0);
}

// ================================================================================================
// PERCENTAGE-CLOSER SOFT SHADOWS
// ================================================================================================
//
// `rendering-lighting-and-shadows` — "Shadow filtering": "a blocker search estimating average
// occluder depth, from which a penumbra radius scales the filter kernel". `virtual-shadows` —
// "Filtering and softness": softness "driven by physical source shape — angular radius for
// directional lights".
//
// Three steps, and the second is the only one that knows about the light:
//
//   1. search a disc for texels NEARER THE LIGHT than the receiver and average their depths;
//   2. similar triangles: a directional light of angular radius a casts a penumbra whose radius on
//      the receiver is (receiver-to-blocker distance) * tan(a). In the map's units that is
//      (blocker - receiver) * `penumbraPerDepth`, where `penumbraPerDepth` is
//      depth range * tan(a) / map width — one number the caller derives from the light and the
//      projection, so there is no softness parameter to tune;
//   3. filter with a disc of that radius.
//
// The map is read through `IShadowDepthSource` because the engine binds shadow maps two ways: the
// frame reads a colour copy through the bindless material table, and a sample binds a depth texture
// directly. The arithmetic is one function either way, and `cy::rendering::pcss_reference` in
// src/rendering/lighting/ is its C++ twin, expression for expression.
//
// REVERSED-Z, as everywhere: the light's near plane is 1, so a blocker is GREATER than the receiver
// and a texel is lit when `receiver >= stored`.

/// A shadow map read without comparison: the depth a texel stores.
public interface IShadowDepthSource
{
    public float storedDepth(float2 uv);
}

/// What the blocker search found: the mean depth of the texels nearer the light than the receiver,
/// and how many of the taps those were. `count == 0` is a receiver nothing blocks.
public struct PcssBlockers
{
    public float averageDepth;
    public float count;
};

/// The per-pixel rotation of the tap disc: interleaved gradient noise (Jimenez 2014), which puts
/// its error at the highest spatial frequency so a small filter or TAA removes it. Fixed in screen
/// space, so a still view gives the same picture every frame.
public float shadowDiscRotation(float2 pixel)
{
    let noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    return noise * 6.28318530718;
}

/// One tap of the rotated disc: `hammersley` spread over the unit disc by its square root, so the
/// taps cover the area evenly rather than bunching at the centre.
public float2 shadowDiscTap(int index, int count, float cosine, float sine)
{
    let point = hammersley(uint(index), uint(count));
    let radius = sqrt(point.x);
    let angle = point.y * 6.28318530718;
    let tap = float2(cos(angle), sin(angle)) * radius;
    return float2(tap.x * cosine - tap.y * sine, tap.x * sine + tap.y * cosine);
}

/// Step 1. `searchRadius` is in UV.
public PcssBlockers pcssBlockerSearch<S : IShadowDepthSource>(S source, float2 uv, float receiver,
                                                              float searchRadius, float rotation,
                                                              int taps)
{
    let cosine = cos(rotation);
    let sine = sin(rotation);
    PcssBlockers result;
    result.averageDepth = 0.0;
    result.count = 0.0;
    for (int index = 0; index < taps; ++index)
    {
        let stored = source.storedDepth(uv + shadowDiscTap(index, taps, cosine, sine) * searchRadius);
        if (stored > receiver)
        {
            result.averageDepth += stored;
            result.count += 1.0;
        }
    }
    if (result.count > 0.0)
    {
        result.averageDepth /= result.count;
    }
    return result;
}

/// Step 2, for a directional light. Zero when nothing blocks — the fully lit case must not produce
/// a kernel.
public float pcssPenumbraRadius(float receiver, float averageBlocker, float penumbraPerDepth)
{
    return max(averageBlocker - receiver, 0.0) * penumbraPerDepth;
}

/// Step 3: the fraction of `taps` disc taps of radius `radius` (UV) that see the light.
public float pcssFilter<S : IShadowDepthSource>(S source, float2 uv, float receiver, float radius,
                                                float rotation, int taps)
{
    let cosine = cos(rotation);
    let sine = sin(rotation);
    float lit = 0.0;
    for (int index = 0; index < taps; ++index)
    {
        let stored = source.storedDepth(uv + shadowDiscTap(index, taps, cosine, sine) * radius);
        lit += receiver >= stored ? 1.0 : 0.0;
    }
    return lit / float(taps);
}

/// The light's shape and the filter's quality, as one frame's shader reads them.
public struct PcssShape
{
    /// UV of penumbra radius per unit of light-space depth between blocker and receiver:
    /// depth range * tan(angular radius) / map width.
    public float penumbraPerDepth;
    /// The smallest kernel, in UV — about a texel, so a contact edge is filtered like PCF rather
    /// than aliased.
    public float minRadius;
    /// The largest, in UV. Bounds the search too: a receiver far below the light's near plane would
    /// otherwise search the whole map.
    public float maxRadius;
    public int blockerTaps;
    public int filterTaps;
};

/// All three steps. A receiver the search finds no blocker for is lit, exactly: an open plane has
/// no penumbra and no noise.
public float pcssVisibility<S : IShadowDepthSource>(S source, float2 uv, float receiver,
                                                    PcssShape shape, float rotation)
{
    // The widest penumbra any blocker between this receiver and the light's near plane could cast.
    let searchRadius = clamp((1.0 - receiver) * shape.penumbraPerDepth, shape.minRadius,
                             shape.maxRadius);
    let blockers = pcssBlockerSearch(source, uv, receiver, searchRadius, rotation,
                                     shape.blockerTaps);
    if (blockers.count == 0.0)
    {
        return 1.0;
    }
    let radius = clamp(pcssPenumbraRadius(receiver, blockers.averageDepth, shape.penumbraPerDepth),
                       shape.minRadius, shape.maxRadius);
    return pcssFilter(source, uv, receiver, radius, rotation, shape.filterTaps);
}
