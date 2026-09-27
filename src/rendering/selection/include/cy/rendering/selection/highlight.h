// SPDX-License-Identifier: MIT
#pragma once
// Selection outlines and unit highlights: what a game marks, how an outline looks, the constants
// the edge pass reads, and the host reference the suites compare the device against.
//
// ================================================================================================
// WHAT A GAME DOES
// ================================================================================================
//
//     HighlightSet highlights(allocator);
//     highlights.select(unit_a, kTeamBlue);          // solid, `selected_width` pixels
//     highlights.select(unit_b, kTeamBlue);
//     highlights.hover(unit_c, kWarmWhite);          // a softer glow, `hovered_width` pixels
//     outline_pass.set_highlights(&highlights, settings);
//
// A mark is keyed by the draw's STABLE IDENTITY — `render::DrawItem::stable_id`, which the extract
// stage fills with the entity id — never by a GPU slot or a draw index, both of which are
// allocation order and move when anything else is published. So a selection made on one frame is
// still the same units on the next, which is what `editor-viewport-and-gizmos` asks of selection in
// general.
//
// ================================================================================================
// WHAT THE PASS DRAWS
// ================================================================================================
//
// The marked objects are drawn again into a MASK — one word a pixel naming the style the nearest
// marked surface draws with, and that surface's own depth — tested only against each other, so the
// mask holds the whole silhouette of every marked object including the parts something else hides.
// An edge pass over the tonemapped colour then decides, per pixel:
//
//   * OUTSIDE every silhouette, within a style's width of one: an outline pixel, in the colour of
//     the NEAREST marked pixel (Euclidean, in whole pixels; ties go to the first in scan order).
//     Selected is solid; hovered falls off from `hovered_strength` at the silhouette.
//   * where that nearest marked pixel is HIDDEN — the scene's depth there is nearer than the marked
//     surface's by more than `depth_tolerance` of it — the outline is the occluded style instead:
//     dashed, and dimmed by `occluded_alpha`.
//   * INSIDE a silhouette: untouched where the marked surface is the one on screen, so an outline
//     never covers the material it surrounds; tinted by `occluded_fill` where it is hidden, the
//     "unit behind a building" read strategy games use.
//
// Nothing marked is nothing drawn: the frame is byte-identical to the frame without the stage.
//
// ================================================================================================
// WHY AFTER THE TONE CURVE
// ================================================================================================
//
// `editor-visual-language`'s "Selection appearance" requires an outline that does not bloom or glow
// and stays legible on bright and dark content without changing width or brightness. Before bloom
// the outline would bloom; before exposure its colour would be the scene's exposure applied to it.
// After the tone curve the colour a game asks for is the colour in the output, byte for byte, which
// is what `render.selection_outlines` checks. The hovered glow is drawn by this pass as a falloff,
// not by the bloom chain, for the same reason.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::selection {

/// A display-referred colour, the bytes an 8-bit output ends up holding. `a` scales the outline's
/// opacity; 255 is solid.
struct HighlightColour {
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
    u8 a = 255;

    /// Packed as an `Rgba8Unorm` texel reads back: red in the low byte.
    [[nodiscard]] constexpr u32 packed() const noexcept {
        return static_cast<u32>(r) | (static_cast<u32>(g) << 8U) | (static_cast<u32>(b) << 16U) |
               (static_cast<u32>(a) << 24U);
    }
    friend constexpr bool operator==(HighlightColour, HighlightColour) noexcept = default;
};

enum class HighlightKind : u8 {
    /// A crisp, solid outline: the unit is in the player's selection.
    Selected = 1,
    /// A softer glow that falls off away from the silhouette: the cursor is over the unit.
    Hovered = 2,
};

/// One marked object.
struct HighlightMark {
    u64 identity = 0;
    HighlightKind kind = HighlightKind::Selected;
    HighlightColour colour;
    /// The style slot the mask carries for it: 1-based into `HighlightSet::styles()`.
    u8 slot = 0;
};

/// How outlines look. One per view; the marks choose only a kind and a colour.
struct OutlineSettings {
    /// A selected outline's width in pixels, 1 to `kMaxOutlineWidth`.
    u32 selected_width = 2;
    /// How far a hovered glow reaches, in pixels, 1 to `kMaxOutlineWidth`.
    u32 hovered_width = 5;
    /// A hovered glow's opacity at the silhouette. It falls off quadratically to the width.
    f32 hovered_strength = 0.85F;
    /// Draw the occluded style where a marked object is hidden. Off, hidden parts get no outline
    /// and no fill — the outline follows only what is on screen.
    bool show_occluded = true;
    /// The occluded outline's opacity, relative to the visible one.
    f32 occluded_alpha = 0.6F;
    /// Dash length and gap, in pixels, along the occluded outline. Zero draws it solid.
    u32 occluded_dash = 3;
    /// The tint over the hidden part of a marked object's silhouette. Zero draws none.
    f32 occluded_fill = 0.25F;
    /// How much nearer, as a fraction of the marked surface's reversed-Z depth, the scene must be
    /// before the surface counts as hidden. It absorbs the two rasterisations of one surface
    /// disagreeing in the last bits; 1e-3 is a centimetre at ten metres.
    f32 depth_tolerance = 1.0e-3F;
};

inline constexpr u32 kMaxOutlineWidth = 8;
/// The mask carries a style in eight bits and zero means "nothing marked".
inline constexpr u32 kMaxHighlightStyles = 255;

/// Refuses a width of zero or above `kMaxOutlineWidth`, a strength or an alpha outside [0, 1] and a
/// negative tolerance.
[[nodiscard]] Status validate(const OutlineSettings& settings) noexcept;

/// `CyOutlineStyle` in selection_outline.slang: one palette entry. 16 bytes.
struct GpuOutlineStyle {
    u32 colour = 0;
    u32 kind = 0;
    /// In pixels.
    f32 width = 0.0F;
    /// Opacity at the silhouette: 1 for a selected outline, `hovered_strength` for a glow.
    f32 strength = 0.0F;
};
static_assert(sizeof(GpuOutlineStyle) == 16);

/// `CyOutlineConstants` in selection_outline.slang, the composite's push constants.
struct alignas(16) OutlineConstants {
    u32 extent[2] = {0, 0};
    /// How far the edge pass searches: the widest style in use.
    u32 radius = 0;
    u32 dash = 0;
    f32 occluded_alpha = 0.0F;
    f32 occluded_fill = 0.0F;
    f32 depth_tolerance = 0.0F;
    /// `kOutlineShowOccluded`.
    u32 flags = 0;
};
static_assert(sizeof(OutlineConstants) == 32);

inline constexpr u32 kOutlineShowOccluded = 1U;

/// The marks one view draws, and the palette of styles they share.
///
/// Sorted by identity, so a draw finds its mark in O(log n). The palette holds one entry per
/// distinct (kind, colour) pair; up to `kMaxHighlightStyles` of them can be in use at once, which
/// is the eight bits the mask spends on it. NOT THREAD-SAFE.
class HighlightSet {
public:
    explicit HighlightSet(Allocator& allocator) noexcept;

    /// Mark an object, or re-mark one: the kind and colour replace what it had. Refuses a palette
    /// with no room for a 256th distinct style, after compacting away styles no mark uses.
    [[nodiscard]] Status mark(u64 identity, HighlightKind kind, HighlightColour colour) noexcept;
    [[nodiscard]] Status select(u64 identity, HighlightColour colour) noexcept {
        return mark(identity, HighlightKind::Selected, colour);
    }
    [[nodiscard]] Status hover(u64 identity, HighlightColour colour) noexcept {
        return mark(identity, HighlightKind::Hovered, colour);
    }
    /// False when the object was not marked.
    bool unmark(u64 identity) noexcept;
    /// Unmark every object of one kind: the cursor left, or the selection was replaced.
    void clear(HighlightKind kind) noexcept;
    void clear() noexcept;

    [[nodiscard]] const HighlightMark* find(u64 identity) const noexcept;
    /// The mask word an object draws with, or 0 when it is not marked.
    [[nodiscard]] u32 slot_of(u64 identity) const noexcept;
    [[nodiscard]] Span<const HighlightMark> marks() const noexcept { return marks_.span(); }
    [[nodiscard]] bool empty() const noexcept { return marks_.empty(); }
    [[nodiscard]] u32 style_count() const noexcept { return static_cast<u32>(styles_.size()); }
    /// Palette entry `slot - 1`, as the device reads it, with the widths `settings` gives.
    [[nodiscard]] GpuOutlineStyle style(u32 slot, const OutlineSettings& settings) const noexcept;
    /// Every palette entry, in slot order. `out` holds at least `style_count()`.
    void write_styles(const OutlineSettings& settings, Span<GpuOutlineStyle> out) const noexcept;
    /// The widest style any mark uses, which is how far the edge pass has to search.
    [[nodiscard]] u32 radius(const OutlineSettings& settings) const noexcept;

private:
    struct Style {
        HighlightKind kind = HighlightKind::Selected;
        HighlightColour colour;
    };

    [[nodiscard]] usize lower_bound(u64 identity) const noexcept;
    [[nodiscard]] u32 find_style(HighlightKind kind, HighlightColour colour) const noexcept;
    [[nodiscard]] Expected<u32, Error> acquire_style(HighlightKind kind,
                                                     HighlightColour colour) noexcept;
    /// Drop the styles no mark uses and renumber the marks' slots.
    [[nodiscard]] Status compact() noexcept;

    Array<HighlightMark> marks_;
    Array<Style> styles_;
};

/// The constants the composite reads for one view.
[[nodiscard]] OutlineConstants make_outline_constants(const HighlightSet& highlights,
                                                      const OutlineSettings& settings, u32 width,
                                                      u32 height) noexcept;

// --- The host reference ------------------------------------------------------------------------

/// What the edge pass reads, `width * height`, row-major from the top-left.
struct OutlineInputs {
    u32 width = 0;
    u32 height = 0;
    /// The mask: 0, or a 1-based style slot.
    Span<const u32> mask;
    /// The marked surfaces' reversed-Z depth, and the scene's.
    Span<const f32> mask_depth;
    Span<const f32> scene_depth;
};

/// What the edge pass decides at one pixel.
enum class OutlineTexel : u8 {
    /// Untouched.
    None = 0,
    /// An outline beside a part of the marked object that is on screen.
    Outline,
    /// An outline beside a part that something hides: the occluded style.
    OccludedOutline,
    /// Inside a silhouette, where the marked surface is hidden.
    OccludedFill,
};

struct OutlineDecision {
    OutlineTexel texel = OutlineTexel::None;
    u8 slot = 0;
    /// The premultiplied source alpha the composite blends with, in (0, 1].
    f32 alpha = 0.0F;
};

/// selection_outline.slang's `cyOutlineComposite` at one pixel, on the host, expression for
/// expression — the same scan order, the same tie-break, the same falloff.
[[nodiscard]] OutlineDecision outline_reference_at(const OutlineInputs& inputs,
                                                   Span<const GpuOutlineStyle> styles,
                                                   const OutlineConstants& constants, u32 x,
                                                   u32 y) noexcept;

/// The whole image. `out` is `width * height`.
[[nodiscard]] Status outline_reference(const OutlineInputs& inputs,
                                       Span<const GpuOutlineStyle> styles,
                                       const OutlineConstants& constants,
                                       Span<OutlineDecision> out) noexcept;

/// The composite's blend, `One, OneMinusSourceAlpha` over an 8-bit texel, rounded to nearest the
/// way a UNORM attachment stores it. Alpha 1 returns the colour's bytes exactly.
[[nodiscard]] u32 blend_outline(u32 destination, u32 colour, f32 alpha) noexcept;

}  // namespace cy::rendering::selection
