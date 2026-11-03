// SPDX-License-Identifier: MIT
// present.slang — samples/13-rts-api's `--play`: the frame, drawn at its own extent, scaled into a
// window of any size and letterboxed where the two aspects differ. host/present_pass.h records it.
//
// One full-screen triangle over the swapchain image. A pixel inside the letterboxed rectangle reads
// the frame bilinearly — four texel loads, so the pass needs no sampler — and a pixel outside it is
// black. The frame's bytes are display-referred already (its tone curve ran), so they are written
// as they are to a UNORM image and decoded first to an sRGB one, whose store encodes them again.
//
// REGENERATING: shaders/regenerate.py beside this file compiles both entry points for SPIR-V and
// MSL and rewrites host/present_spirv.h and host/present_msl.h. `just quality-shader-headers`
// fails when they drift from this source.

module rts_present;

/// Decode the frame's bytes before an sRGB store re-encodes them.
static const uint kCyRtsPresentSrgbTarget = 1u;

public struct CyRtsPresentConstants
{
    /// The frame's extent, in texels.
    public uint2 source;
    /// The letterboxed rectangle in the window's image: its top-left corner and its size, in
    /// pixels.
    public float2 origin;
    public float2 size;
    public uint flags;
    public uint pad;
};

public struct CyRtsPresentSet
{
    public Texture2D<float4> frame;
};

[[vk::binding(0, 0)]]
public ParameterBlock<CyRtsPresentSet> cyRtsPresent;

[[vk::push_constant]]
public ConstantBuffer<CyRtsPresentConstants> cyRtsPresentConstants;

public struct CyRtsPresentVertex
{
    public float4 position : SV_Position;
};

[shader("vertex")]
CyRtsPresentVertex cyRtsPresentVertex(uint vertex : SV_VertexID)
{
    CyRtsPresentVertex output;
    const float2 corner = float2(float((vertex << 1u) & 2u), float(vertex & 2u));
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

float4 load(int2 texel)
{
    const int2 last = int2(cyRtsPresentConstants.source) - 1;
    return cyRtsPresent.frame.Load(int3(clamp(texel, int2(0, 0), last), 0));
}

float3 decode(float3 encoded)
{
    const float3 low = encoded / 12.92;
    const float3 high = pow((encoded + 0.055) / 1.055, float3(2.4, 2.4, 2.4));
    return select(encoded <= 0.04045, low, high);
}

[shader("fragment")]
float4 cyRtsPresentFragment(CyRtsPresentVertex input) : SV_Target
{
    const float2 local = input.position.xy - cyRtsPresentConstants.origin;
    if (any(local < 0.0) || any(local >= cyRtsPresentConstants.size))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    // The texel centre this pixel's centre falls on, and its neighbours' weights.
    const float2 at =
        local / cyRtsPresentConstants.size * float2(cyRtsPresentConstants.source) - 0.5;
    const int2 base = int2(floor(at));
    const float2 t = at - floor(at);
    const float4 top = lerp(load(base), load(base + int2(1, 0)), t.x);
    const float4 bottom = lerp(load(base + int2(0, 1)), load(base + int2(1, 1)), t.x);
    float3 colour = lerp(top, bottom, t.y).rgb;
    if ((cyRtsPresentConstants.flags & kCyRtsPresentSrgbTarget) != 0u)
    {
        colour = decode(colour);
    }
    return float4(colour, 1.0);
}
