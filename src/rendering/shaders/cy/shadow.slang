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
