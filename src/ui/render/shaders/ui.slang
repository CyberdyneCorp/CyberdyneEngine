// SPDX-License-Identifier: MIT
// CyberUI's one shader: every flattened primitive — a panel, a rounded and bordered box, an image,
// a glyph — is a quad drawn by this pair, and the primitive's material index picks the behaviour.
// `ui-system`: "Text glyphs, rounded rectangles, borders, gradients, shadows, and images SHALL share
// the primitive representation and, where practical, one shader with material-indexed behaviour."
//
// `cy::ui::render::shade_reference` is `cyUiFragment` on the host, expression for expression, and
// `render.ui` compares the two pixel by pixel. A change here that is not made there is a red suite.
//
// ================================================================================================
// ONE PRIMITIVE, ONE QUAD, ONE SIGNED DISTANCE
// ================================================================================================
//
// The quad is the primitive's bounds grown by one pixel on every side, so the half-pixel of
// anti-aliasing outside an edge that does not fall on a pixel boundary is rasterised at all. What
// a pixel receives is decided by a signed distance to the rounded box, in PIXELS: a pixel centre
// inside the box by half a pixel or more is covered entirely, one outside by half a pixel or more
// not at all. An edge on a pixel boundary — every edge of a whole-pixel rectangle — therefore
// covers whole pixels and nothing else, and its colour lands in the output byte for byte. A border
// is the difference between the box and the box inset by the border's width.
//
// ================================================================================================
// PREMULTIPLIED, DISPLAY-REFERRED, AND NOTHING WRITTEN WHERE NOTHING IS DRAWN
// ================================================================================================
//
// Colours arrive premultiplied, with the element's opacity already in them (`cy::ui::flatten`), as
// 0xAARRGGBB words, and leave premultiplied for a `One, OneMinusSourceAlpha` blend. The interface
// is drawn after the tone curve into the display-referred output, so a colour is an encoding, not a
// radiance: on a UNORM target it is written as it is; on an sRGB target (`kCyUiOutputLinear`) it is
// decoded first so that the hardware's encode gives the same bytes back. A pixel no primitive
// covers is DISCARDED, so the frame under an interface is untouched by construction.
//
// ================================================================================================
// NO FRAME SETS
// ================================================================================================
//
// This file imports nothing. Its one set is its own — the primitives, the atlas page of the batch
// being drawn, and two samplers — at set 0, and the push block names the batch's first primitive.
// Regenerate the committed SPIR-V and MSL with src/ui/render/shaders/regenerate.py.

module ui;

/// `BuiltinMaterial` in cy/ui/paint.h.
static const uint kCyUiShape = 0u;
static const uint kCyUiImage = 1u;
static const uint kCyUiGlyph = 2u;

/// `kUiOutputLinear` in cy/ui/render/encode.h: the target encodes sRGB itself.
static const uint kCyUiOutputLinear = 1u;

/// `cy::ui::render::GpuUiPrimitive`. 64 bytes.
public struct CyUiPrimitive
{
    /// x, y, width, height, in pixels from the top-left of the target.
    public float4 bounds;
    /// u0, v0, width, height, normalised to the atlas page.
    public float4 uv;
    /// Corner radius and border width, in pixels; the rest is zero.
    public float4 shape;
    /// Colour, border colour (both premultiplied 0xAARRGGBB), material, atlas page.
    public uint4 words;
};

/// `cy::ui::render::UiPush`. 16 bytes.
public struct CyUiPush
{
    /// Two over the target's width and height, which is what maps a pixel to clip space.
    public float2 inverseExtent;
    /// The batch's first primitive: `SV_InstanceID` counts from zero on every target.
    public uint first;
    public uint flags;
};

public struct CyUiSet
{
    public StructuredBuffer<CyUiPrimitive> primitives;
    /// The batch's atlas page: glyph coverage in red, or a premultiplied image.
    public Texture2D<float4> atlas;
    /// Glyphs: a bitmap font's pixels stay whole at a whole-number scale.
    public SamplerState pointClamp;
    /// Images.
    public SamplerState linearClamp;
};

[[vk::binding(0, 0)]]
public ParameterBlock<CyUiSet> cyUi;

[[vk::push_constant]]
public ConstantBuffer<CyUiPush> cyUiPush;

public struct CyUiVertex
{
    public float4 position : SV_Position;
    public nointerpolation uint primitive : CY_UI_PRIMITIVE;
};

/// Corner `vertexId` of the primitive's grown quad: the index buffer is 0 1 2, 2 1 3.
[shader("vertex")]
public CyUiVertex cyUiVertex(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    let index = cyUiPush.first + instanceId;
    let bounds = cyUi.primitives[index].bounds;
    let corner = float2(float(vertexId & 1u), float(vertexId >> 1u));
    let pixel = bounds.xy - 1.0 + corner * (bounds.zw + 2.0);
    CyUiVertex output;
    output.position = float4(pixel.x * cyUiPush.inverseExtent.x - 1.0,
                             1.0 - pixel.y * cyUiPush.inverseExtent.y, 0.0, 1.0);
    output.primitive = index;
    return output;
}

/// The signed distance from `p` to a box of half-size `halfSize` centred on the origin, its corners
/// rounded by `radius` (clamped to the box). Negative inside.
float roundedBoxDistance(float2 p, float2 halfSize, float radius)
{
    let r = min(radius, min(halfSize.x, halfSize.y));
    let q = abs(p) - halfSize + r;
    return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - r;
}

/// How much of the pixel at `p` the box covers: whole at half a pixel inside, none at half outside.
float coverage(float2 p, float2 halfSize, float radius)
{
    if (halfSize.x <= 0.0 || halfSize.y <= 0.0)
    {
        return 0.0;
    }
    return saturate(0.5 - roundedBoxDistance(p, halfSize, radius));
}

/// 0xAARRGGBB to (r, g, b, a), still premultiplied.
float4 unpackColour(uint colour)
{
    return float4(float((colour >> 16u) & 0xFFu), float((colour >> 8u) & 0xFFu),
                  float(colour & 0xFFu), float(colour >> 24u)) / 255.0;
}

float decodeSrgb(float value)
{
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

/// A premultiplied display encoding to the premultiplied linear value an sRGB target re-encodes.
float4 toLinear(float4 colour)
{
    if (colour.a <= 0.0)
    {
        return colour;
    }
    let straight = colour.rgb / colour.a;
    return float4(float3(decodeSrgb(straight.r), decodeSrgb(straight.g), decodeSrgb(straight.b)) *
                      colour.a,
                  colour.a);
}

[shader("fragment")]
public float4 cyUiFragment(CyUiVertex input) : SV_Target
{
    let primitive = cyUi.primitives[input.primitive];
    let pixel = input.position.xy;
    let halfSize = primitive.bounds.zw * 0.5;
    let local = pixel - (primitive.bounds.xy + halfSize);
    let radius = primitive.shape.x;
    let border = primitive.shape.y;

    let outer = coverage(local, halfSize, radius);
    let inner = border > 0.0 ? coverage(local, halfSize - border, max(radius - border, 0.0))
                             : outer;

    var fill = unpackColour(primitive.words.x);
    let material = primitive.words.z;
    if (material == kCyUiImage || material == kCyUiGlyph)
    {
        let along = saturate((pixel - primitive.bounds.xy) / primitive.bounds.zw);
        let uv = primitive.uv.xy + along * primitive.uv.zw;
        if (material == kCyUiGlyph)
        {
            fill *= cyUi.atlas.SampleLevel(cyUi.pointClamp, uv, 0.0).r;
        }
        else
        {
            fill *= cyUi.atlas.SampleLevel(cyUi.linearClamp, uv, 0.0);
        }
    }

    var result = fill * inner + unpackColour(primitive.words.y) * (outer - inner);
    if (all(result == float4(0.0, 0.0, 0.0, 0.0)))
    {
        discard;
    }
    if ((cyUiPush.flags & kCyUiOutputLinear) != 0u)
    {
        result = toLinear(result);
    }
    return result;
}
