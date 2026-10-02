// SPDX-License-Identifier: MIT
#pragma once
// The host half of the interface renderer: what a flattened primitive becomes on the device, how a
// batch becomes a draw, and the shader transcribed for the suites. No device anywhere in this file.
//
// ================================================================================================
// REFERENCE UNITS IN, PIXELS OUT
// ================================================================================================
//
// `cy::ui` lays out and flattens in the document's reference units; the renderer draws in the
// target's pixels. `encode_primitive` applies the one scale `ui::resolve_scale` computed for the
// document, so the shader never sees a reference unit and a 1080p and a 4K target run the same
// shader on the same stream. The clip rectangle becomes a scissor by the same rule a rasteriser
// covers pixels by: a pixel is inside when its centre is.
//
// ================================================================================================
// ONE BATCH, ONE DRAW
// ================================================================================================
//
// `flatten()`'s batches already break on material, atlas, clip and transform, so a batch is exactly
// what one draw can cover: one atlas page bound, one scissor set, one instanced quad per primitive.
// `build_draws` turns each into a `UiDraw` and drops the ones whose scissor is empty — a scroll
// view scrolled entirely out of its parent draws nothing rather than drawing with a zero scissor.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ui/paint.h>

namespace cy::ui::render {

/// cy/ui.slang's `CyUiPrimitive`, 64 bytes, std430: four 16-byte rows.
struct alignas(16) GpuUiPrimitive {
    /// x, y, width, height, in pixels.
    f32 bounds[4] = {};
    /// u0, v0, width, height, normalised to the atlas page.
    f32 uv[4] = {};
    /// Corner radius and border width in pixels, then two zeros.
    f32 shape[4] = {};
    /// Colour, border colour, material, atlas page.
    u32 colour = 0;
    u32 border_colour = 0;
    u32 material = 0;
    u32 atlas = 0;
};
static_assert(sizeof(GpuUiPrimitive) == 64, "CyUiPrimitive is four 16-byte rows");

/// `CyUiPush::flags`: the target encodes sRGB itself, so the shader decodes the colour first.
inline constexpr u32 kUiOutputLinear = 1U;

/// cy/ui.slang's `CyUiPush`, 16 bytes.
struct UiPush {
    f32 inverse_extent[2] = {};
    u32 first = 0;
    u32 flags = 0;
};
static_assert(sizeof(UiPush) == 16, "CyUiPush is one 16-byte row");

/// A scissor, in whole pixels from the top-left of the target.
struct PixelRect {
    u32 x = 0;
    u32 y = 0;
    u32 width = 0;
    u32 height = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] constexpr bool contains(u32 px, u32 py) const noexcept {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

/// One draw: a batch of primitives sharing a material, an atlas page and a scissor.
struct UiDraw {
    u32 first = 0;
    u32 count = 0;
    u16 material = 0;
    u16 atlas = 0;
    PixelRect scissor;
};

/// A primitive in reference units to the device's row, in pixels at `scale`.
[[nodiscard]] GpuUiPrimitive encode_primitive(const Primitive& primitive, f32 scale) noexcept;

/// The inverse of `encode_primitive` for every field it carries. Clip, transform and source are
/// the draw's and the inspector's, not the row's, and come back zero.
[[nodiscard]] Primitive decode_primitive(const GpuUiPrimitive& row, f32 scale) noexcept;

/// The pixels a clip rectangle in reference units covers at `scale`, clamped to the target: a
/// pixel is inside when its centre is.
[[nodiscard]] PixelRect scissor_for(const Rect& clip, f32 scale, u32 width, u32 height) noexcept;

/// What `build_draws` produced from one flattened buffer.
struct UiDrawList {
    explicit UiDrawList(Allocator& allocator) noexcept : rows(allocator), draws(allocator) {}

    Array<GpuUiPrimitive> rows;
    Array<UiDraw> draws;
    /// Batches whose scissor was empty and that therefore draw nothing.
    u32 clipped_batches = 0;

    void clear() noexcept {
        rows.clear();
        draws.clear();
        clipped_batches = 0;
    }
};

/// Encode every primitive of `buffer` and turn each of its batches into a draw, in order.
[[nodiscard]] Status build_draws(const PrimitiveBuffer& buffer, f32 scale, u32 width, u32 height,
                                 UiDrawList& out) noexcept;

// --- The shader, on the host ---------------------------------------------------------------------

/// An atlas page as the reference reads it: RGBA8 texels, red in the low byte, or one coverage byte
/// a texel (`channels == 1`), which reads as (r, 0, 0, 1) as an `R8Unorm` view does.
struct ReferenceAtlas {
    Span<const u8> texels;
    u32 width = 0;
    u32 height = 0;
    u32 channels = 4;
};

/// `cyUiFragment` at one pixel, before blending: the premultiplied (r, g, b, a) in [0, 1] the
/// shader returns, and false where it discards. `atlas` may be null for a shape.
[[nodiscard]] bool shade_reference(const GpuUiPrimitive& row, u32 px, u32 py,
                                   const ReferenceAtlas* atlas, f32 (&out)[4]) noexcept;

/// Draw `list` over `target` (`width * height` RGBA8 texels, red in the low byte) as the device
/// does: each draw in order, scissored, every primitive blended `One, OneMinusSourceAlpha` and
/// rounded to the nearest 8-bit step. `atlases[page]` is the page a draw samples, or empty.
[[nodiscard]] Status draw_reference(const UiDrawList& list, Span<const ReferenceAtlas> atlases,
                                    u32 width, u32 height, Span<u32> target) noexcept;

}  // namespace cy::ui::render
