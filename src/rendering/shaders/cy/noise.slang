// Hashes and noise. Part of the engine's Slang standard library.
//
// The hashes are integer hashes, not sin-based ones: a sin-based hash produces different values on
// different drivers, and `rendering-architecture` requires the frame to be reproducible.

module noise;

public uint hashPcg(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    return (word >> 22) ^ word;
}

public uint3 hashPcg3d(uint3 value)
{
    uint3 v = value * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

/// A unit-range float from a hash. The 24-bit mantissa is what a float can represent exactly, so
/// the low eight bits are dropped rather than folded in.
public float randomUnit(uint seed)
{
    return float(hashPcg(seed) >> 8) * (1.0 / 16777216.0);
}

/// Value noise over a 3D lattice, smoothstep-interpolated.
public float valueNoise(float3 position)
{
    let cell = floor(position);
    let fraction = position - cell;
    let weight = fraction * fraction * (3.0 - 2.0 * fraction);
    let base = uint3(int3(cell) + 1024);

    float result = 0.0;
    for (uint corner = 0; corner < 8; ++corner)
    {
        let offset = uint3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        let sample = float(hashPcg3d(base + offset).x >> 8) * (1.0 / 16777216.0);
        let blend = lerp(1.0 - weight, weight, float3(offset));
        result += sample * blend.x * blend.y * blend.z;
    }
    return result;
}
