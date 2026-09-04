// Sampling patterns and importance sampling. Part of the engine's Slang standard library.

module sampling;

/// The radical inverse in base two, which is the second dimension of a Hammersley set.
public float radicalInverseBase2(uint index)
{
    uint bits = index;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return float(bits) * 2.3283064365386963e-10;
}

public float2 hammersley(uint index, uint count)
{
    return float2(float(index) / float(count), radicalInverseBase2(index));
}

/// A cosine-weighted direction in the hemisphere around +Z.
public float3 cosineHemisphere(float2 random)
{
    let radius = sqrt(random.x);
    let angle = 6.283185307179586 * random.y;
    return float3(radius * cos(angle), radius * sin(angle), sqrt(max(0.0, 1.0 - random.x)));
}

/// A GGX half-vector in the hemisphere around +Z, for a given roughness.
public float3 importanceSampleGgx(float2 random, float roughness)
{
    let alpha = roughness * roughness;
    let phi = 6.283185307179586 * random.x;
    let cosTheta = sqrt((1.0 - random.y) / (1.0 + (alpha * alpha - 1.0) * random.y));
    let sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

/// An orthonormal basis around `normal`, branchless (Duff et al.). Used to move a sample out of
/// tangent space without a second normalize.
public void orthonormalBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    let sign = normal.z >= 0.0 ? 1.0 : -1.0;
    let a = -1.0 / (sign + normal.z);
    let b = normal.x * normal.y * a;
    tangent = float3(1.0 + sign * normal.x * normal.x * a, sign * b, -sign * normal.x);
    bitangent = float3(b, sign + normal.y * normal.y * a, -normal.y);
}
