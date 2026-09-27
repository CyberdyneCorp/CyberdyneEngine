// SPDX-License-Identifier: MIT
// The marks, the palette and the edge pass's host reference. `unit.rendering_selection`.
//
// No device: every case here is about what the API records and what the reference decides, over
// small synthetic masks whose answers can be written down. `render.selection_outlines` holds the
// device to this reference over a real frame.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/selection/highlight.h>
#include <cy/test/test.h>

#include <cmath>
#include <vector>

using namespace cy;
using namespace cy::rendering::selection;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

constexpr HighlightColour kBlue{40, 120, 255, 255};
constexpr HighlightColour kRed{230, 50, 40, 255};
constexpr HighlightColour kWhite{255, 250, 235, 255};

/// A synthetic frame for the reference: a mask, the marked depth and the scene's depth.
struct Picture {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> mask;
    std::vector<f32> mask_depth;
    std::vector<f32> scene_depth;

    Picture(u32 w, u32 h)
        : width(w),
          height(h),
          mask(static_cast<usize>(w) * h, 0U),
          mask_depth(static_cast<usize>(w) * h, 0.0F),
          scene_depth(static_cast<usize>(w) * h, 0.05F) {}

    /// Mark a rectangle with a slot, at a depth; the scene shows the marked surface there.
    void mark(u32 x0, u32 y0, u32 x1, u32 y1, u32 slot, f32 depth) {
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                const usize index = (static_cast<usize>(y) * width) + x;
                mask[index] = slot;
                mask_depth[index] = depth;
                scene_depth[index] = depth;
            }
        }
    }
    /// Put something nearer than the marked surface over a rectangle.
    void occlude(u32 x0, u32 y0, u32 x1, u32 y1, f32 depth) {
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                scene_depth[(static_cast<usize>(y) * width) + x] = depth;
            }
        }
    }
    [[nodiscard]] OutlineInputs inputs() const {
        return OutlineInputs{width, height, Span<const u32>(mask.data(), mask.size()),
                             Span<const f32>(mask_depth.data(), mask_depth.size()),
                             Span<const f32>(scene_depth.data(), scene_depth.size())};
    }
};

/// How far a coordinate lies outside the half-open span `[low, high)`, in whole pixels.
[[nodiscard]] u32 outside(u32 value, u32 low, u32 high) {
    if (value < low) {
        return low - value;
    }
    if (value >= high) {
        return value - high + 1U;
    }
    return 0;
}

/// The Euclidean distance, squared, from a pixel to the nearest pixel of a rectangle.
[[nodiscard]] u32 distance_squared_to(u32 x, u32 y, u32 x0, u32 y0, u32 x1, u32 y1) {
    const u32 dx = outside(x, x0, x1);
    const u32 dy = outside(y, y0, y1);
    return (dx * dx) + (dy * dy);
}

[[nodiscard]] std::vector<OutlineDecision> decide(const Picture& picture, const HighlightSet& set,
                                                  const OutlineSettings& settings) {
    std::vector<GpuOutlineStyle> styles(set.style_count());
    set.write_styles(settings, Span<GpuOutlineStyle>(styles.data(), styles.size()));
    const OutlineConstants constants =
        make_outline_constants(set, settings, picture.width, picture.height);
    std::vector<OutlineDecision> out(static_cast<usize>(picture.width) * picture.height);
    const Status made = outline_reference(picture.inputs(),
                                          Span<const GpuOutlineStyle>(styles.data(), styles.size()),
                                          constants, Span<OutlineDecision>(out.data(), out.size()));
    CY_REQUIRE(made.has_value());
    return out;
}

}  // namespace

CY_TEST_CASE("a mark is kept by identity, replaced by a second mark and removed by unmark") {
    HighlightSet set(allocator());
    CY_REQUIRE(set.select(907, kBlue).has_value());
    CY_REQUIRE(set.select(903, kBlue).has_value());
    CY_REQUIRE(set.hover(905, kWhite).has_value());
    CY_REQUIRE_EQ(set.marks().size(), 3U);
    // Sorted by identity, whatever order the game marked in.
    CY_CHECK_EQ(set.marks()[0].identity, 903U);
    CY_CHECK_EQ(set.marks()[1].identity, 905U);
    CY_CHECK_EQ(set.marks()[2].identity, 907U);
    // One style per distinct kind and colour, shared by the marks that use it.
    CY_CHECK_EQ(set.style_count(), 2U);
    CY_CHECK_EQ(set.slot_of(903), set.slot_of(907));
    CY_CHECK_NE(set.slot_of(903), set.slot_of(905));
    CY_CHECK_EQ(set.slot_of(904), 0U);

    // A second mark replaces the first: the unit under the cursor is also selected.
    CY_REQUIRE(set.select(905, kRed).has_value());
    CY_REQUIRE_EQ(set.marks().size(), 3U);
    CY_CHECK(set.find(905)->kind == HighlightKind::Selected);
    CY_CHECK(set.find(905)->colour == kRed);

    CY_CHECK(set.unmark(903));
    CY_CHECK_FALSE(set.unmark(903));
    CY_CHECK_EQ(set.find(903), nullptr);
    CY_CHECK_EQ(set.marks().size(), 2U);

    CY_REQUIRE(set.hover(911, kWhite).has_value());
    set.clear(HighlightKind::Hovered);
    CY_CHECK_EQ(set.find(911), nullptr);
    CY_CHECK_NE(set.find(905), nullptr);
    set.clear();
    CY_CHECK(set.empty());
}

CY_TEST_CASE("the palette holds 255 styles, compacts the unused ones and refuses the 256th") {
    HighlightSet set(allocator());
    for (u32 index = 0; index < kMaxHighlightStyles; ++index) {
        const HighlightColour colour{static_cast<u8>(index), 7, 9, 255};
        CY_REQUIRE(set.select(1000U + index, colour).has_value());
    }
    CY_CHECK_EQ(set.style_count(), kMaxHighlightStyles);
    // Every slot is in use, so a new colour has nowhere to go.
    const Status refused = set.select(5000, HighlightColour{1, 2, 3, 255});
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::OutOfRange);
    // Free one by unmarking its only user: the next new colour compacts and fits, and every mark
    // still names its own colour afterwards.
    CY_CHECK(set.unmark(1000U));
    CY_REQUIRE(set.select(5000, HighlightColour{1, 2, 3, 255}).has_value());
    CY_CHECK_EQ(set.style_count(), kMaxHighlightStyles);
    const OutlineSettings settings;
    for (const HighlightMark& mark : set.marks()) {
        CY_CHECK_EQ(set.style(mark.slot, settings).colour, mark.colour.packed());
    }
}

CY_TEST_CASE("settings outside their ranges are refused") {
    OutlineSettings settings;
    CY_CHECK(validate(settings).has_value());
    settings.selected_width = 0;
    CY_CHECK_FALSE(validate(settings).has_value());
    settings.selected_width = kMaxOutlineWidth + 1U;
    CY_CHECK_FALSE(validate(settings).has_value());
    settings = OutlineSettings{};
    settings.occluded_alpha = 1.5F;
    CY_CHECK_FALSE(validate(settings).has_value());
    settings = OutlineSettings{};
    settings.depth_tolerance = -1.0F;
    CY_CHECK_FALSE(validate(settings).has_value());
}

CY_TEST_CASE("a selected outline is every pixel within its width outside the silhouette, solid") {
    Picture picture(24, 20);
    HighlightSet set(allocator());
    CY_REQUIRE(set.select(1, kBlue).has_value());
    picture.mark(8, 6, 14, 12, set.slot_of(1), 0.2F);
    OutlineSettings settings;
    settings.selected_width = 3;
    const std::vector<OutlineDecision> out = decide(picture, set, settings);
    for (u32 y = 0; y < picture.height; ++y) {
        for (u32 x = 0; x < picture.width; ++x) {
            const OutlineDecision& decision = out[(static_cast<usize>(y) * picture.width) + x];
            const u32 d2 = distance_squared_to(x, y, 8, 6, 14, 12);
            const bool ring = d2 > 0 && d2 <= 9U;
            CY_CHECK_EQ(decision.texel == OutlineTexel::Outline, ring);
            if (ring) {
                CY_CHECK_EQ(decision.alpha, 1.0F);
                CY_CHECK_EQ(decision.slot, set.slot_of(1));
            } else {
                CY_CHECK(decision.texel == OutlineTexel::None);
            }
        }
    }
    // Alpha 1 puts the colour's bytes into the output exactly, whatever was there.
    CY_CHECK_EQ(blend_outline(0xFF102030U, kBlue.packed(), 1.0F), kBlue.packed());
}

CY_TEST_CASE("a hovered glow falls off from its strength and reaches its own width") {
    Picture picture(30, 20);
    HighlightSet set(allocator());
    CY_REQUIRE(set.hover(1, kWhite).has_value());
    picture.mark(10, 8, 14, 12, set.slot_of(1), 0.2F);
    OutlineSettings settings;
    settings.hovered_width = 5;
    settings.hovered_strength = 0.8F;
    const std::vector<OutlineDecision> out = decide(picture, set, settings);
    // Along the row through the middle, to the right of the silhouette: strictly decreasing, the
    // strength at the edge, and nothing past the width.
    const u32 row = 10;
    f32 previous = 2.0F;
    for (u32 step = 1; step <= 5U; ++step) {
        const OutlineDecision& decision = out[(static_cast<usize>(row) * 30U) + 13U + step];
        CY_CHECK(decision.texel == OutlineTexel::Outline);
        CY_CHECK_LT(decision.alpha, previous);
        previous = decision.alpha;
    }
    CY_CHECK_NEAR(out[(static_cast<usize>(row) * 30U) + 14U].alpha, 0.8F, 1e-6F);
    CY_CHECK(out[(static_cast<usize>(row) * 30U) + 19U].texel == OutlineTexel::None);
}

CY_TEST_CASE("the occluded style is drawn only beside the hidden part, and the fill only on it") {
    Picture picture(40, 20);
    HighlightSet set(allocator());
    CY_REQUIRE(set.select(1, kBlue).has_value());
    picture.mark(10, 5, 30, 15, set.slot_of(1), 0.2F);
    // A wall over the right half of the unit and beyond it.
    picture.occlude(20, 0, 40, 20, 0.4F);
    OutlineSettings settings;
    settings.selected_width = 2;
    settings.occluded_dash = 3;
    const std::vector<OutlineDecision> out = decide(picture, set, settings);
    bool saw_gap = false;
    bool saw_dash = false;
    for (u32 y = 0; y < picture.height; ++y) {
        for (u32 x = 0; x < picture.width; ++x) {
            const OutlineDecision& decision = out[(static_cast<usize>(y) * picture.width) + x];
            const bool inside = x >= 10 && x < 30 && y >= 5 && y < 15;
            if (inside) {
                // Tinted exactly where the unit is hidden, untouched where it is on screen.
                CY_CHECK_EQ(decision.texel == OutlineTexel::OccludedFill, x >= 20U);
                continue;
            }
            if (decision.texel == OutlineTexel::OccludedOutline) {
                // Beside the hidden part only — the nearest marked pixel is under the wall — and
                // dimmed.
                CY_CHECK_GE(x, 19U);
                CY_CHECK_NEAR(decision.alpha, settings.occluded_alpha, 1e-6F);
                saw_dash = true;
            } else if (decision.texel == OutlineTexel::Outline) {
                CY_CHECK_LT(x, 20U);
            } else if (distance_squared_to(x, y, 10, 5, 30, 15) <= 4U && x >= 21U) {
                saw_gap = true;
            }
        }
    }
    CY_CHECK(saw_dash);
    // Dashed: some of the ring beside the hidden part is left out.
    CY_CHECK(saw_gap);

    // Off, the hidden part draws nothing at all.
    settings.show_occluded = false;
    const std::vector<OutlineDecision> hidden_off = decide(picture, set, settings);
    for (const OutlineDecision& decision : hidden_off) {
        CY_CHECK(decision.texel != OutlineTexel::OccludedOutline);
        CY_CHECK(decision.texel != OutlineTexel::OccludedFill);
    }
}

CY_TEST_CASE("between two marked objects the nearer silhouette's style wins, and a tie the first") {
    Picture picture(32, 12);
    HighlightSet set(allocator());
    CY_REQUIRE(set.select(1, kBlue).has_value());
    CY_REQUIRE(set.select(2, kRed).has_value());
    // Two squares with a three-pixel gap: columns 10, 11 and 12 are between them.
    picture.mark(4, 4, 10, 8, set.slot_of(1), 0.2F);
    picture.mark(13, 4, 19, 8, set.slot_of(2), 0.2F);
    OutlineSettings settings;
    settings.selected_width = 3;
    const std::vector<OutlineDecision> out = decide(picture, set, settings);
    const auto at = [&](u32 x, u32 y) { return out[(static_cast<usize>(y) * 32U) + x]; };
    CY_CHECK_EQ(at(10, 5).slot, set.slot_of(1));
    CY_CHECK_EQ(at(12, 5).slot, set.slot_of(2));
    // Column 11 is two pixels from both. The scan runs row by row, left to right, and takes only a
    // strictly nearer pixel, so the tie goes to the left square — as it does on the device.
    CY_CHECK_EQ(at(11, 5).slot, set.slot_of(1));
}

CY_TEST_CASE("nothing marked decides nothing anywhere") {
    Picture picture(16, 16);
    HighlightSet set(allocator());
    const std::vector<OutlineDecision> out = decide(picture, set, OutlineSettings{});
    for (const OutlineDecision& decision : out) {
        CY_CHECK(decision.texel == OutlineTexel::None);
    }
    CY_CHECK_EQ(make_outline_constants(set, OutlineSettings{}, 16, 16).radius, 0U);
}
