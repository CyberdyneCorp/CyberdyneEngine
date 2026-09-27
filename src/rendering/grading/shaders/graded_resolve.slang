// SPDX-License-Identifier: MIT
// The graded resolve: exposure, the tone curve and the grading lookup, straight into the frame's
// output. `rendering-post-processing` — "Exposure", "Tonemapping", "Colour grading", and steps 10,
// 11 and 12 of "Chain order and colour space".
//
// ================================================================================================
// WHAT IT IS, AND WHAT IT IS NOT
// ================================================================================================
//
// It is `cy/fullscreen.slang`'s `fullscreenResolve` with two things added, and with everything that
// function does done by the SAME library calls in the same order — `applyExposure`, then the curve
// the specialization constant selects — so that with the metered exposure off and no table bound it
// draws the frame's resolve byte for byte, which render.grading asserts:
//
//   * THE EXPOSURE CAN BE METERED. With `exposure.y` set, the stops come from the state the metering
//     chain wrote (exposure_adapt.slang): EV100 x, turned into stops by `exposure_stops_for_ev100` —
//     `-EV100 - log2(1.2)`, the saturation-based multiplier `1 / (1.2 * 2^EV100)`. Otherwise the stops
//     are pushed, which is how a manual EV100 or a camera's aperture, shutter and ISO arrive.
//   * THE GRADE IS ONE LOOKUP, after the curve, on display-referred colour — step 12. The table is
//     indexed and stored in `display_log_encode` (src/rendering/post/include/cy/rendering/post/lut.h):
//     `log2(1 + 4096 x) / log2(4097)`, a base-two log with a linear toe, so the shadows get the
//     resolution the specification's log-space scenario asks for and black is a lattice point.
//
// The vertex stage is the library's own `fullscreenVertex`, compiled from cy/fullscreen.slang; this
// file's input struct matches its output (position, then TEXCOORD0) rather than importing the module,
// whose own parameter blocks would claim this pipeline's set 0.
//
// See exposure_histogram.slang for how this file is compiled.

import cy.tonemap;

/// `cy::rendering::grading::GradedResolvePush`, 32 bytes.
struct CyGradedResolvePush
{
    /// x: the stops to expose by when not metered. y: 1 to read the metered EV100 instead.
    /// z: log2(1.2), the saturation constant's stops. w: unused.
    float4 exposure;
    /// x: 1 when a grading table is applied. y: the table's edge. zw: unused.
    float4 lut;
};

struct CyGradedResolveSet
{
    Texture2D<float4> sceneColor;
    SamplerState linearClamp;
    /// The grade, `display_log_encode`d in and out. A 2³ identity when no grade is bound, so the
    /// descriptor is always valid; `lut.x` is what decides whether it is read.
    Texture3D<float4> lut;
    /// exposure_adapt.slang's state: x is the EV100 in force.
    StructuredBuffer<float4> exposureState;
};

[[vk::binding(0, 0)]] ParameterBlock<CyGradedResolveSet> cyGradedSet;
[[vk::push_constant]] ConstantBuffer<CyGradedResolvePush> cyGradedPush;

/// The curve, as `fullscreenResolve`'s `kTonemapOperator`: 0 none, 1 Reinhard, 2 the ACES fit.
[SpecializationConstant]
const int kGradedTonemapOperator = 1;

static const float kCyDisplayToe = 4096.0;
/// log2(4097): display-linear 1 encodes to exactly 1.
static const float kCyDisplaySpan = 12.000352177480302;

struct CyGradedFragment
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

/// `sample_display_lut`: encode, one trilinear fetch at the texel centres' lattice, decode.
float3 cyGradedLookup(float3 displayLinear, float edge)
{
    let encoded = log2(1.0 + saturate(displayLinear) * kCyDisplayToe) / kCyDisplaySpan;
    let uvw = encoded * ((edge - 1.0) / edge) + 0.5 / edge;
    let graded = cyGradedSet.lut.SampleLevel(cyGradedSet.linearClamp, uvw, 0.0).rgb;
    return (exp2(saturate(graded) * kCyDisplaySpan) - 1.0) / kCyDisplayToe;
}

[shader("fragment")]
float4 cyGradedResolve(CyGradedFragment input) : SV_Target
{
    let scene = cyGradedSet.sceneColor.Sample(cyGradedSet.linearClamp, input.uv).rgb;
    float stops = cyGradedPush.exposure.x;
    if (cyGradedPush.exposure.y > 0.5)
    {
        stops = -cyGradedSet.exposureState[0].x - cyGradedPush.exposure.z;
    }
    let exposed = applyExposure(scene, stops);

    float3 mapped = exposed;
    if (kGradedTonemapOperator == 1)
    {
        mapped = tonemapReinhard(exposed, 4.0);
    }
    else if (kGradedTonemapOperator == 2)
    {
        mapped = tonemapAcesApproximate(exposed);
    }
    if (cyGradedPush.lut.x > 0.5)
    {
        mapped = cyGradedLookup(mapped, cyGradedPush.lut.y);
    }
    return float4(mapped, 1.0);
}
