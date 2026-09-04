// The full-screen triangle, and a tonemapping resolve over it. Part of the engine's shader library.
//
// ONE TRIANGLE, NOT TWO. A single oversized triangle covers the viewport with no diagonal seam,
// which matters because the quad's diagonal is a place where derivatives are wrong in exactly one
// pixel-wide band.
//
// The vertex stage reads no vertex buffer: the position is derived from SV_VertexID. That is why
// this shader reflects with no vertex inputs, which is worth knowing when reading its reflection.

module fullscreen;

import cy.globals;
import cy.tonemap;
import cy.color;

public struct FullscreenVertex
{
    public float4 position : SV_Position;
    public float2 uv : TEXCOORD0;
};

[shader("vertex")]
public FullscreenVertex fullscreenVertex(uint vertexId: SV_VertexID)
{
    FullscreenVertex output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    // Reversed-Z (design.md §3): the depth buffer is cleared to 0 and compared GreaterEqual, so a
    // full-screen pass that must survive the test writes 1, the near value.
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    return output;
}

/// Pass-specific resources are set 2 by the descriptor set convention.
[[vk::binding(0, 2)]]
Texture2D<float4> sceneColor;
[[vk::binding(1, 2)]]
SamplerState linearClamp;

/// Which curve to apply. A specialization constant, not a preprocessor permutation: changing the
/// tonemapper is a quality setting, and `shader-system` requires a quality change to produce a new
/// pipeline from the same SPIR-V rather than a new compilation.
[SpecializationConstant]
const int kTonemapOperator = 1;

static const int kTonemapNone = 0;
static const int kTonemapReinhard = 1;
static const int kTonemapAces = 2;

[shader("fragment")]
public float4 fullscreenResolve(FullscreenVertex input) : SV_Target
{
    let scene = sceneColor.Sample(linearClamp, input.uv).rgb;
    let exposed = applyExposure(scene, cyGlobals.exposureStops);

    float3 mapped = exposed;
    if (kTonemapOperator == kTonemapReinhard)
    {
        mapped = tonemapReinhard(exposed, 4.0);
    }
    else if (kTonemapOperator == kTonemapAces)
    {
        mapped = tonemapAcesApproximate(exposed);
    }
    // The swapchain is an sRGB format, so the hardware applies the transfer function on write and
    // this shader must not apply it a second time. `linearToSrgb` exists for the targets that are
    // not — a readback, a UNORM capture — and is deliberately not called here.
    return float4(mapped, 1.0);
}
