// SPDX-License-Identifier: MIT
// The selection mask: the objects a game marked, drawn again with the frame's own geometry into one
// word a pixel and a depth of their own. `src/rendering/selection/include/cy/rendering/selection/
// outline_pass.h` records it; selection_outline.slang reads what it wrote.
//
// ================================================================================================
// THE FRAME'S VERTEX PATH, NOT A COPY OF IT
// ================================================================================================
//
// `import cy.frame` is the point, as it is for cy/particle.slang: the placement is the frame's own
// `cyDrawInstances` and `cyInstances`, read through the frame's own set 1, and the position goes
// through the same `transformToRelative` and `transformToClip` the depth prepass uses — so a marked
// surface that is on screen lands on the prepass's depth to the last few bits, and the edge pass
// can tell "hidden" from "visible" by comparing the two.
//
// ONE PUSH WORD, TWO FIELDS. The frame's layout carries one 32-bit push constant, and a layout
// whose push range differs is not compatible with the frame's sets (Vulkan 14.2.2 — see
// src/rendering/particles/src/transparent_draw.cpp). So the draw index takes the low 24 bits and
// the mark's style slot the high 8: sixteen million draws, 255 styles, and zero for "unmarked",
// which the mask is cleared to.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
// src/rendering/selection/shaders/regenerate.py compiles this file and selection_outline.slang and
// rewrites src/selection_spirv.h and src/selection_msl.h. For Metal it defines CY_FRAME_METAL and
// CY_MATERIAL_METAL_ARGUMENT_BUFFER, as cy/frame.slang's header asks of every module importing it.

module selection_mask;

import cy.frame;

static const uint kCyOutlineDrawMask = 0x00FFFFFFu;
static const uint kCyOutlineSlotShift = 24u;

public struct CyOutlineMaskVertex
{
    public float4 position : SV_Position;
};

[shader("vertex")]
public CyOutlineMaskVertex cyOutlineMaskVertex(float3 modelPosition : POSITION)
{
    let draw = cyFrameView.drawInstances[cyDraw.drawIndex & kCyOutlineDrawMask];
    let instance = cyFrameView.instances[draw.instanceSlot];
    CyOutlineMaskVertex output;
    output.position = transformToClip(transformToRelative(instance, modelPosition));
    return output;
}

/// The style slot, 1 to 255. The depth test — GreaterOrEqual, written, against a mask depth
/// cleared to the far plane — keeps the nearest marked surface, so where two marked objects
/// overlap the mask names the one in front.
[shader("fragment")]
public uint cyOutlineMaskFragment(CyOutlineMaskVertex input) : SV_Target
{
    return cyDraw.drawIndex >> kCyOutlineSlotShift;
}
