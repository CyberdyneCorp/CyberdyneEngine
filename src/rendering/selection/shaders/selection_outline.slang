// SPDX-License-Identifier: MIT
// The selection outline composite: a full-screen edge pass over the tonemapped colour that reads
// selection_mask.slang's mask and depth and the frame's prepass depth.
//
// `cy::rendering::selection::outline_reference_at` is this file's `cyOutlineComposite` on the host,
// expression for expression, and `render.selection_outlines` compares the two pixel by pixel. A
// change here that is not made there is a red suite, which is the point of having both.
//
// ================================================================================================
// WHAT ONE PIXEL DECIDES
// ================================================================================================
//
// Inside a silhouette (the mask names a style here): nothing, unless the marked surface is HIDDEN —
// the scene's depth is nearer than the mask's by more than `depthTolerance` of it — in which case the
// occluded fill tints it. An outline never covers the material it surrounds.
//
// Outside every silhouette: the nearest marked pixel within the search radius whose style is wide
// enough to reach here, scanned row by row and taking only a strictly nearer one, so a tie goes to
// the first in scan order — the same pixel the host picks. Selected is solid; hovered falls off
// quadratically from its strength at the silhouette to nothing past its width. Where that nearest
// marked pixel is hidden, the occluded style: dashed along the diagonal `(x + y) / dash`, which
// breaks an outline running in any direction, and dimmed by `occludedAlpha`.
//
// PREMULTIPLIED OUT, blended `One, OneMinusSourceAlpha` by the pipeline, and a pixel with nothing to
// draw is DISCARDED rather than written with zero alpha: the untouched output is then untouched by
// construction rather than by the blend unit's rounding.
//
// ================================================================================================
// NO FRAME SETS
// ================================================================================================
//
// This file does not import cy.frame. The composite's one set is its own — the mask, the two depths
// and the style palette — at set 0, and a module that imported the frame would bring the frame's
// set 0 declarations into the same program. See selection_mask.slang for regenerating.

module selection_outline;

static const uint kCyOutlineShowOccluded = 1u;
static const uint kCyOutlineHovered = 2u;

/// `cy::rendering::selection::GpuOutlineStyle`. 16 bytes.
public struct CyOutlineStyle
{
    /// Display-referred RGBA8, red in the low byte.
    public uint colour;
    /// 1 selected, 2 hovered.
    public uint kind;
    /// In pixels.
    public float width;
    /// Opacity at the silhouette.
    public float strength;
};

/// `cy::rendering::selection::OutlineConstants`. 32 bytes.
public struct CyOutlineConstants
{
    public uint2 extent;
    public uint radius;
    public uint dash;
    public float occludedAlpha;
    public float occludedFill;
    public float depthTolerance;
    public uint flags;
};

public struct CyOutlineSet
{
    public Texture2D<uint> mask;
    public Texture2D<float> maskDepth;
    public Texture2D<float> sceneDepth;
    public StructuredBuffer<CyOutlineStyle> styles;
};

[[vk::binding(0, 0)]]
public ParameterBlock<CyOutlineSet> cyOutline;

[[vk::push_constant]]
public ConstantBuffer<CyOutlineConstants> cyOutlineConstants;

public struct CyOutlineVertex
{
    public float4 position : SV_Position;
};

/// One oversized triangle from `SV_VertexID`, as `cy/fullscreen.slang`'s `fullscreenVertex`: the one
/// draw of this stage is `draw(3, 1, 0, 0)`, the zero base every target agrees on.
[shader("vertex")]
public CyOutlineVertex cyOutlineVertex(uint vertexId : SV_VertexID)
{
    let uv = float2((vertexId << 1) & 2, vertexId & 2);
    CyOutlineVertex output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    return output;
}

float4 unpackColour(uint colour)
{
    return float4(float(colour & 0xFFu), float((colour >> 8) & 0xFFu),
                  float((colour >> 16) & 0xFFu), float(colour >> 24)) / 255.0;
}

bool hidden(int2 pixel)
{
    let marked = cyOutline.maskDepth.Load(int3(pixel, 0));
    let scene = cyOutline.sceneDepth.Load(int3(pixel, 0));
    return scene > marked + marked * cyOutlineConstants.depthTolerance;
}

float4 premultiplied(uint colour, float alpha)
{
    return float4(unpackColour(colour).rgb * alpha, alpha);
}

[shader("fragment")]
public float4 cyOutlineComposite(CyOutlineVertex input) : SV_Target
{
    let pixel = int2(input.position.xy);
    let showOccluded = (cyOutlineConstants.flags & kCyOutlineShowOccluded) != 0;
    let here = cyOutline.mask.Load(int3(pixel, 0));
    if (here != 0)
    {
        if (!showOccluded || cyOutlineConstants.occludedFill <= 0.0 || !hidden(pixel))
        {
            discard;
        }
        let colour = cyOutline.styles[here - 1].colour;
        let fill = cyOutlineConstants.occludedFill * unpackColour(colour).a;
        if (fill <= 0.0)
        {
            discard;
        }
        return premultiplied(colour, fill);
    }

    let radius = int(cyOutlineConstants.radius);
    let extent = int2(cyOutlineConstants.extent);
    int bestDistance = radius * radius + 1;
    uint bestSlot = 0;
    int2 bestPixel = pixel;
    for (int dy = -radius; dy <= radius; ++dy)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            let distance = dx * dx + dy * dy;
            let other = pixel + int2(dx, dy);
            if (distance >= bestDistance || other.x < 0 || other.y < 0 || other.x >= extent.x ||
                other.y >= extent.y)
            {
                continue;
            }
            let slot = cyOutline.mask.Load(int3(other, 0));
            if (slot == 0)
            {
                continue;
            }
            let width = cyOutline.styles[slot - 1].width;
            if (float(distance) > width * width)
            {
                continue;
            }
            bestDistance = distance;
            bestSlot = slot;
            bestPixel = other;
        }
    }
    if (bestSlot == 0)
    {
        discard;
    }

    let style = cyOutline.styles[bestSlot - 1];
    float alpha = style.strength;
    if (style.kind == kCyOutlineHovered)
    {
        let t = saturate((style.width + 1.0 - sqrt(float(bestDistance))) / style.width);
        alpha *= t * t;
    }
    alpha *= unpackColour(style.colour).a;
    if (hidden(bestPixel))
    {
        let dash = cyOutlineConstants.dash;
        if (!showOccluded || (dash != 0 && ((uint(pixel.x + pixel.y) / dash) & 1u) != 0))
        {
            discard;
        }
        alpha *= cyOutlineConstants.occludedAlpha;
    }
    if (alpha <= 0.0)
    {
        discard;
    }
    return premultiplied(style.colour, alpha);
}
