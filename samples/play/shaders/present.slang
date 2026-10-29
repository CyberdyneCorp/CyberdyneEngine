// SPDX-License-Identifier: MIT
// The play host's present: the game's picture scaled into the window.
//
// The game draws at a fixed extent — its frame's targets, its upscaler and its GI are sized once —
// and the window is whatever the player made it. One full-screen triangle, drawn into the
// letterbox the pass sets as its viewport, samples the picture with a linear filter: the picture
// keeps its aspect, and the bars either side are the cleared black of the swapchain image.
//
// THE PICTURE IS DISPLAY-REFERRED BYTES — the frame's tone curve has run, and its output is an
// eight-bit UNORM image holding encoded values. On a UNORM swapchain they are written as they are;
// on an sRGB one (`kCyPresentDecode`) they are decoded first, so the hardware's encode gives the
// same bytes back rather than brightening the picture a second time. The interface the presenter
// draws afterwards keeps the same rule (`kUiOutputLinear`).
//
// Its own set at 0 and its push block, no frame sets: this file imports nothing. Regenerate the
// committed SPIR-V and MSL with samples/play/shaders/regenerate.py.

module present;

/// The swapchain image encodes sRGB itself: decode the picture's bytes first.
static const uint kCyPresentDecode = 1u;

struct CyPresentPush
{
    uint flags;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct CyPresentSet
{
    Texture2D<float4> picture;
    SamplerState linearClamp;
};

[[vk::binding(0, 0)]]
ParameterBlock<CyPresentSet> cyPresent;

[[vk::push_constant]]
ConstantBuffer<CyPresentPush> cyPresentPush;

struct CyPresentVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

/// One oversized triangle from `SV_VertexID`, `cy/fullscreen.slang`'s: every draw is
/// `draw(3, 1, 0, 0)`, which is what makes the index agree on every target.
[shader("vertex")]
CyPresentVertex cyPresentVertex(uint vertexId : SV_VertexID)
{
    CyPresentVertex output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

/// The sRGB transfer function's inverse, per channel.
float3 decodeSrgb(float3 encoded)
{
    let low = encoded / 12.92;
    let high = pow((encoded + 0.055) / 1.055, float3(2.4, 2.4, 2.4));
    return select(encoded <= float3(0.04045, 0.04045, 0.04045), low, high);
}

[shader("fragment")]
float4 cyPresentFragment(CyPresentVertex input) : SV_Target0
{
    var colour = cyPresent.picture.Sample(cyPresent.linearClamp, input.uv).rgb;
    if ((cyPresentPush.flags & kCyPresentDecode) != 0u)
    {
        colour = decodeSrgb(colour);
    }
    return float4(colour, 1.0);
}
