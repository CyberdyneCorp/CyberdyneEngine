// The full-screen triangle, and a tonemapping resolve over it. Part of the engine's shader library.
//
// ONE TRIANGLE, NOT TWO. A single oversized triangle covers the viewport with no diagonal seam,
// which matters because the quad's diagonal is a place where derivatives are wrong in exactly one
// pixel-wide band.
//
// The vertex stage reads no vertex buffer: the position is derived from SV_VertexID. That is why
// this shader reflects with no vertex inputs, which is worth knowing when reading its reflection.

module fullscreen;

import cy.tonemap;
import cy.color;

struct FullscreenGlobalsData
{
    float timeSeconds;
    float deltaSeconds;
    float exposureStops;
    float windStrength;
    float4 windDirectionAndSpeed;
};

struct FullscreenGlobalSet
{
    ConstantBuffer<FullscreenGlobalsData> data;
};

[[vk::binding(0, 0)]] ParameterBlock<FullscreenGlobalSet> globalSet;

// Set 1 is retained in this entry point so Metal's argument-buffer indices stay equal to the
// engine's set indices. The extent is already present at view binding 0; the impossible branch
// makes it a real read without changing a legal frame.
struct FullscreenViewData
{
    float4 rows[14];
    uint4 words[4];
    float4 temporalFeedback;
    float4 temporalJitter;
};

struct FullscreenViewSet
{
    ConstantBuffer<FullscreenViewData> frame;
};

[[vk::binding(0, 1)]] ParameterBlock<FullscreenViewSet> viewSet;

public struct FullscreenVertex
{
    public float4 position : SV_Position;
    public float2 uv : TEXCOORD0;
};

/// `SV_VulkanVertexID` rather than `SV_VertexID`, and the difference is a DEVICE FEATURE.
///
/// Slang lowers `SV_VertexID` to `gl_VertexIndex - gl_BaseVertex`, to match Direct3D's semantics —
/// which declares the SPIR-V `DrawParameters` capability, which a Vulkan device only accepts with
/// `VkPhysicalDeviceVulkan11Features::shaderDrawParameters` enabled. `src/backends/rhi/vulkan/` does
/// not request that feature, so a pipeline built from such a module is refused by validation.
/// `SV_VulkanVertexID` is the raw `VertexIndex`, which is what a draw with no vertex buffer and no
/// base vertex means anyway. Recorded here because the alternative fix is in the backend.
[shader("vertex")]
public FullscreenVertex fullscreenVertex(uint vertexId: SV_VulkanVertexID)
{
    FullscreenVertex output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    // Reversed-Z (design.md §3): the depth buffer is cleared to 0 and compared GreaterEqual, so a
    // full-screen pass that must survive the test writes 1, the near value.
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    return output;
}

/// Pass-specific resources are set 2 by the descriptor set convention.
struct FullscreenPassSet
{
    Texture2D<float4> sceneColor;
    SamplerState linearClamp;
    Texture2D<float4> historyColor;
    Texture2D<float2> velocity;
    Texture2D<float> depth;
};

[[vk::binding(0, 2)]] ParameterBlock<FullscreenPassSet> passSet;

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
    let scene = passSet.sceneColor.Sample(passSet.linearClamp, input.uv).rgb;
    let exposed = applyExposure(scene, globalSet.data.exposureStops);

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
    if (viewSet.frame.rows[13].x < 0.0)
        mapped = -mapped;
    return float4(mapped, 1.0);
}

/// Temporal resolve in scene-linear space. History is clamped to the current 3x3 neighbourhood
/// before blending, which bounds stale highlights and turns disocclusion into a local current-frame
/// reconstruction instead of a trail. `rows[10]` and `rows[11]` are the two temporal float4s at
/// the end of `CyFrameData`.
[shader("fragment")]
public float4 temporalResolve(FullscreenVertex input) : SV_Target
{
    let inverseExtent = viewSet.frame.rows[13].zw;
    let feedback = viewSet.frame.temporalFeedback;
    let current = passSet.sceneColor.SampleLevel(passSet.linearClamp, input.uv, 0.0);

    float3 minimum = current.rgb;
    float3 maximum = current.rgb;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            let sampleUv = input.uv + float2(x, y) * inverseExtent;
            let neighbour = passSet.sceneColor.SampleLevel(passSet.linearClamp, sampleUv, 0.0).rgb;
            minimum = min(minimum, neighbour);
            maximum = max(maximum, neighbour);
        }
    }

    // WHERE THIS PIXEL WAS LAST FRAME, and nothing else: the prepass removes the jitter from the
    // motion vector (`temporal-rendering`'s convention), so a still camera reads its history at
    // exactly this texel. The previous offset appears nowhere — last frame's jitter is already
    // averaged into its history.
    //
    // THIS USED TO ADD `previous - current` jitter to a motion vector that still carried the
    // current one, so a still camera re-sampled its history at a different sub-pixel offset every
    // frame and the resolved picture changed from frame to frame as much as an unresolved one:
    // `render.pipeline`'s temporal case measured 1.057/255 against 0.997/255 with the history cut.
    let motion = passSet.velocity.SampleLevel(passSet.linearClamp, input.uv, 0.0);
    let surfaceDepth = passSet.depth.SampleLevel(passSet.linearClamp, input.uv, 0.0);
    let historyUv = input.uv + motion;
    let inBounds = all(historyUv >= float2(0.0)) && all(historyUv <= float2(1.0));
    let previous = passSet.historyColor.SampleLevel(passSet.linearClamp, historyUv, 0.0).rgb;
    let clampedHistory = clamp(previous, minimum, maximum);
    let historyWeight = (feedback.x > 0.5 && inBounds && surfaceDepth > 0.0) ? feedback.y : 0.0;
    return float4(lerp(current.rgb, clampedHistory, historyWeight), current.a);
}
