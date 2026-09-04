// Packing and encoding helpers. Part of the engine's Slang standard library (`shader-system`).
//
// A module, not a header: `shader-system` requires shared shader code to "live in a Slang module
// imported by each, with no textual preprocessor inclusion". `import cy.packing` is how it is used.

module packing;

/// Octahedral encoding of a unit vector into two components in [0,1].
///
/// Two 16-bit channels of this carry a normal to within about a tenth of a degree, which is why the
/// GPU scene stores normals this way rather than as three floats.
public float2 encodeOctahedral(float3 normal)
{
    let projected = normal / (abs(normal.x) + abs(normal.y) + abs(normal.z));
    float2 encoded = projected.xy;
    if (projected.z < 0.0)
    {
        encoded = (1.0 - abs(projected.yx)) * select(projected.xy >= 0.0, 1.0, -1.0);
    }
    return encoded * 0.5 + 0.5;
}

public float3 decodeOctahedral(float2 encoded)
{
    let centred = encoded * 2.0 - 1.0;
    float3 normal = float3(centred.x, centred.y, 1.0 - abs(centred.x) - abs(centred.y));
    let fold = saturate(-normal.z);
    normal.xy += select(normal.xy >= 0.0, -fold, fold);
    return normalize(normal);
}

// UNORM PACKING IS SLANG'S, NOT THE ENGINE'S. `packUnorm4x8`, `unpackUnorm4x8`, `packUnorm2x16`
// and `unpackUnorm2x16` are in Slang's own standard library and lower to the SPIR-V instructions
// for them. Redeclaring them here made every call site ambiguous, which is the compiler saying the
// same thing this codebase says about dependencies: do not reimplement what you are integrating.

/// The inverses. Slang's own standard library provides `packUnorm4x8` and `packUnorm2x16` but not
/// their inverses, so these two are the engine's — and the pack half is deliberately absent above
/// rather than duplicated, because redeclaring it made every call site ambiguous.
public float4 unpackUnorm4x8(uint packed)
{
    let quantised = uint4(packed, packed >> 8, packed >> 16, packed >> 24) & 0xFFu;
    return float4(quantised) / 255.0;
}

public float2 unpackUnorm2x16(uint packed)
{
    return float2(uint2(packed, packed >> 16) & 0xFFFFu) / 65535.0;
}
