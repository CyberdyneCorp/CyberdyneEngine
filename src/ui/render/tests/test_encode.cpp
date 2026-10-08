// SPDX-License-Identifier: MIT
// `unit.ui_render`: the host half of the interface renderer — the rows the shader reads, the
// scissors, one draw per batch, and the shader transcribed — with no device.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/paint.h>
#include <cy/ui/render/encode.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace cy;
using namespace cy::ui;
using namespace cy::ui::render;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

[[nodiscard]] ElementId panel(ElementStore& store, ElementId parent, ui::Rect rect, u32 colour,
                              ui::Rect clip = ui::Rect{0.0F, 0.0F, 64.0F, 48.0F}) noexcept {
    auto created = store.create(parent, Name::intern("panel"));
    if (!created) {
        return kNoElement;
    }
    store.layout_output(*created)->rect = rect;
    store.layout_output(*created)->clip = clip;
    store.paint(*created)->background = colour;
    return *created;
}

/// RGBA8, red in the low byte, from the interface's premultiplied 0xAARRGGBB.
[[nodiscard]] constexpr u32 texel_of(u32 argb) noexcept {
    return ((argb >> 16U) & 0xFFU) | (argb & 0xFF00U) | ((argb & 0xFFU) << 16U) |
           (argb & 0xFF000000U);
}

}  // namespace

CY_TEST_CASE("ui_render: a primitive round-trips through the row the shader reads") {
    // The layout cy/ui.slang declares: four 16-byte rows, and the words where the shader looks.
    static_assert(offsetof(GpuUiPrimitive, bounds) == 0);
    static_assert(offsetof(GpuUiPrimitive, uv) == 16);
    static_assert(offsetof(GpuUiPrimitive, shape) == 32);
    static_assert(offsetof(GpuUiPrimitive, colour) == 48);
    static_assert(offsetof(GpuUiPrimitive, border_colour) == 52);
    static_assert(offsetof(GpuUiPrimitive, material) == 56);
    static_assert(offsetof(GpuUiPrimitive, atlas) == 60);

    Primitive primitive;
    primitive.bounds = ui::Rect{12.5F, 7.25F, 100.0F, 40.0F};
    primitive.uv = ui::Rect{0.125F, 0.25F, 0.0625F, 0.5F};
    primitive.material = material_index(BuiltinMaterial::Image);
    primitive.atlas = 3;
    primitive.colour = 0xC0102030U;
    primitive.border_colour = 0xFF405060U;
    primitive.corner_radius = 6.0F;
    primitive.border_width = 1.5F;
    for (const f32 scale : {1.0F, 2.0F, 1.5F}) {
        const GpuUiPrimitive row = encode_primitive(primitive, scale);
        // Scaled to pixels on the way in.
        CY_CHECK_EQ(row.bounds[0], 12.5F * scale);
        CY_CHECK_EQ(row.bounds[3], 40.0F * scale);
        CY_CHECK_EQ(row.shape[0], 6.0F * scale);
        CY_CHECK_EQ(row.shape[1], 1.5F * scale);
        CY_CHECK_EQ(row.shape[2], 0.0F);
        // And everything it carries comes back.
        const Primitive back = decode_primitive(row, scale);
        CY_CHECK_EQ(back.bounds.x, primitive.bounds.x);
        CY_CHECK_EQ(back.bounds.y, primitive.bounds.y);
        CY_CHECK_EQ(back.bounds.width, primitive.bounds.width);
        CY_CHECK_EQ(back.bounds.height, primitive.bounds.height);
        CY_CHECK_EQ(back.uv.x, primitive.uv.x);
        CY_CHECK_EQ(back.uv.height, primitive.uv.height);
        CY_CHECK_EQ(back.material, primitive.material);
        CY_CHECK_EQ(back.atlas, primitive.atlas);
        CY_CHECK_EQ(back.colour, primitive.colour);
        CY_CHECK_EQ(back.border_colour, primitive.border_colour);
        CY_CHECK_EQ(back.corner_radius, primitive.corner_radius);
        CY_CHECK_EQ(back.border_width, primitive.border_width);
    }
}

CY_TEST_CASE("ui_render: a clip becomes the scissor of the pixels whose centres it holds") {
    // Whole-pixel edges: exactly those pixels.
    PixelRect rect = scissor_for(ui::Rect{10.0F, 4.0F, 20.0F, 8.0F}, 1.0F, 64, 48);
    CY_CHECK_EQ(rect.x, 10U);
    CY_CHECK_EQ(rect.y, 4U);
    CY_CHECK_EQ(rect.width, 20U);
    CY_CHECK_EQ(rect.height, 8U);
    // A fractional edge keeps a pixel when its centre is inside: 10.4 holds 10.5, 10.6 does not.
    rect = scissor_for(ui::Rect{10.4F, 0.0F, 5.0F, 1.0F}, 1.0F, 64, 48);
    CY_CHECK_EQ(rect.x, 10U);
    CY_CHECK_EQ(rect.width, 5U);
    rect = scissor_for(ui::Rect{10.6F, 0.0F, 5.0F, 1.0F}, 1.0F, 64, 48);
    CY_CHECK_EQ(rect.x, 11U);
    CY_CHECK_EQ(rect.width, 5U);
    // Scaled to pixels, then clamped to the target.
    rect = scissor_for(ui::Rect{-5.0F, 20.0F, 100.0F, 100.0F}, 2.0F, 64, 48);
    CY_CHECK_EQ(rect.x, 0U);
    CY_CHECK_EQ(rect.y, 40U);
    CY_CHECK_EQ(rect.width, 64U);
    CY_CHECK_EQ(rect.height, 8U);
    CY_CHECK(scissor_for(ui::Rect{70.0F, 0.0F, 5.0F, 5.0F}, 1.0F, 64, 48).empty());
}

CY_TEST_CASE("ui_render: every batch is one draw with its clip, and a clipped-away batch none") {
    ElementStore store(allocator());
    const ElementId root =
        panel(store, kNoElement, ui::Rect{0.0F, 0.0F, 64.0F, 48.0F}, 0xFF000000U);
    // Two panels under one clip share a batch; a third, under a clip that is a sliver between two
    // pixel centres, is a batch whose scissor is empty and which therefore draws nothing.
    (void)panel(store, root, ui::Rect{2.0F, 2.0F, 10.0F, 10.0F}, 0xFFFF0000U,
                ui::Rect{0.0F, 0.0F, 32.0F, 32.0F});
    (void)panel(store, root, ui::Rect{14.0F, 2.0F, 10.0F, 10.0F}, 0xFF00FF00U,
                ui::Rect{0.0F, 0.0F, 32.0F, 32.0F});
    (void)panel(store, root, ui::Rect{40.0F, 2.0F, 10.0F, 10.0F}, 0xFF0000FFU,
                ui::Rect{40.1F, 2.0F, 0.3F, 10.0F});

    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(store, ui::Rect{0.0F, 0.0F, 64.0F, 48.0F}, buffer, report).has_value());
    CY_REQUIRE_EQ(report.batches, 3U);

    UiDrawList list(allocator());
    CY_REQUIRE(build_draws(buffer, 1.0F, 64, 48, list).has_value());
    CY_CHECK_EQ(list.rows.size(), 4U);
    CY_REQUIRE_EQ(list.draws.size(), 2U);
    CY_CHECK_EQ(list.clipped_batches, 1U);
    CY_CHECK_EQ(list.draws[0].first, 0U);
    CY_CHECK_EQ(list.draws[0].count, 1U);
    CY_CHECK_EQ(list.draws[0].scissor.width, 64U);
    CY_CHECK_EQ(list.draws[1].first, 1U);
    CY_CHECK_EQ(list.draws[1].count, 2U);
    CY_CHECK_EQ(list.draws[1].scissor.width, 32U);
    CY_CHECK_EQ(list.draws[1].scissor.height, 32U);
}

CY_TEST_CASE("ui_render: a whole-pixel box lands byte for byte and nothing outside it is touched") {
    GpuUiPrimitive row;
    row.bounds[0] = 4.0F;
    row.bounds[1] = 3.0F;
    row.bounds[2] = 6.0F;
    row.bounds[3] = 5.0F;
    row.colour = 0xFF336699U;
    f32 out[4] = {};
    // Every pixel whose centre is inside is exactly the colour; the first outside discards.
    for (u32 y = 3; y < 8U; ++y) {
        for (u32 x = 4; x < 10U; ++x) {
            CY_REQUIRE(shade_reference(row, x, y, nullptr, out));
            CY_CHECK_EQ(out[0], static_cast<f32>(0x33) / 255.0F);
            CY_CHECK_EQ(out[2], static_cast<f32>(0x99) / 255.0F);
            CY_CHECK_EQ(out[3], 1.0F);
        }
    }
    CY_CHECK_FALSE(shade_reference(row, 3, 4, nullptr, out));
    CY_CHECK_FALSE(shade_reference(row, 10, 4, nullptr, out));
    CY_CHECK_FALSE(shade_reference(row, 5, 8, nullptr, out));

    // A border is the ring inside the bounds, in its own colour; the inside keeps the fill.
    row.shape[1] = 1.0F;
    row.border_colour = 0xFFFFFFFFU;
    CY_REQUIRE(shade_reference(row, 4, 3, nullptr, out));
    CY_CHECK_EQ(out[0], 1.0F);
    CY_REQUIRE(shade_reference(row, 6, 5, nullptr, out));
    CY_CHECK_EQ(out[0], static_cast<f32>(0x33) / 255.0F);

    // A rounded corner is partly covered at the corner pixel and whole at the centre.
    row.shape[0] = 2.5F;
    row.shape[1] = 0.0F;
    CY_REQUIRE(shade_reference(row, 4, 3, nullptr, out));
    CY_CHECK_GT(out[3], 0.0F);
    CY_CHECK_LT(out[3], 1.0F);
    CY_REQUIRE(shade_reference(row, 6, 5, nullptr, out));
    CY_CHECK_EQ(out[3], 1.0F);
}

CY_TEST_CASE(
    "ui_render: the reference draws in order, blends premultiplied, and keeps the scissor") {
    ElementStore store(allocator());
    const ElementId root = panel(store, kNoElement, ui::Rect{0.0F, 0.0F, 0.0F, 0.0F}, 0);
    // Later siblings over earlier ones; a half-transparent one blends; a nested pair of scroll
    // views clips to the intersection of both.
    (void)panel(store, root, ui::Rect{0.0F, 0.0F, 20.0F, 20.0F}, 0xFFFF0000U);
    (void)panel(store, root, ui::Rect{10.0F, 10.0F, 20.0F, 20.0F}, 0xFF0000FFU);
    const ElementId glass = panel(store, root, ui::Rect{40.0F, 0.0F, 10.0F, 10.0F}, 0xFFFFFFFFU);
    store.paint(glass)->opacity = 0.5F;
    (void)panel(store, root, ui::Rect{30.0F, 30.0F, 30.0F, 30.0F}, 0xFF00FF00U,
                ui::Rect{32.0F, 32.0F, 10.0F, 8.0F});

    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(store, ui::Rect{0.0F, 0.0F, 64.0F, 48.0F}, buffer, report).has_value());
    UiDrawList list(allocator());
    CY_REQUIRE(build_draws(buffer, 1.0F, 64, 48, list).has_value());

    const u32 below = 0xFF202020U;
    std::vector<u32> target(static_cast<usize>(64U) * 48U, below);
    CY_REQUIRE(
        draw_reference(list, {}, 64, 48, Span<u32>(target.data(), target.size())).has_value());
    const auto at = [&](u32 x, u32 y) { return target[(static_cast<usize>(y) * 64U) + x]; };
    CY_CHECK_EQ(at(5, 5), texel_of(0xFFFF0000U));
    // The overlap is the later sibling's.
    CY_CHECK_EQ(at(15, 15), texel_of(0xFF0000FFU));
    // Half white over 0x20: 128 + 32 * 0.498 = 143.9, the nearest step 144.
    CY_CHECK_EQ(at(45, 5) & 0xFFU, 144U);
    // Inside the scissor, green; outside it but inside the panel, untouched.
    CY_CHECK_EQ(at(35, 35), texel_of(0xFF00FF00U));
    CY_CHECK_EQ(at(31, 35), below);
    CY_CHECK_EQ(at(35, 40), below);
    CY_CHECK_EQ(at(63, 47), below);
}

CY_TEST_CASE("ui_render: a glyph samples its page's coverage by point, an image by its texels") {
    // A 4x4 coverage page with one lit texel at (1, 2).
    std::vector<u8> coverage(16, 0);
    coverage[(2U * 4U) + 1U] = 255;
    const ReferenceAtlas glyphs{Span<const u8>(coverage.data(), coverage.size()), 4, 4, 1};
    GpuUiPrimitive glyph;
    glyph.bounds[0] = 0.0F;
    glyph.bounds[1] = 0.0F;
    glyph.bounds[2] = 8.0F;
    glyph.bounds[3] = 8.0F;
    glyph.uv[2] = 1.0F;
    glyph.uv[3] = 1.0F;
    glyph.material = material_index(BuiltinMaterial::Glyph);
    glyph.colour = 0xFFFFC000U;
    f32 out[4] = {};
    // At twice the page's size, texel (1, 2) covers pixels 2..3 by 4..5 and nothing else.
    CY_REQUIRE(shade_reference(glyph, 2, 4, &glyphs, out));
    CY_CHECK_EQ(out[0], 1.0F);
    CY_CHECK_EQ(out[1], static_cast<f32>(0xC0) / 255.0F);
    CY_REQUIRE(shade_reference(glyph, 3, 5, &glyphs, out));
    CY_CHECK_FALSE(shade_reference(glyph, 4, 4, &glyphs, out));
    CY_CHECK_FALSE(shade_reference(glyph, 2, 6, &glyphs, out));

    // An image's texel, premultiplied, multiplied by the colour.
    const u8 texels[4] = {255, 128, 0, 255};
    const ReferenceAtlas image{Span<const u8>(texels, 4), 1, 1, 4};
    GpuUiPrimitive picture = glyph;
    picture.material = material_index(BuiltinMaterial::Image);
    picture.colour = 0xFFFFFFFFU;
    CY_REQUIRE(shade_reference(picture, 3, 3, &image, out));
    CY_CHECK_EQ(out[0], 1.0F);
    CY_CHECK_EQ(out[1], 128.0F / 255.0F);
    CY_CHECK_EQ(out[2], 0.0F);
}

CY_TEST_CASE("ui_render: a distance-field glyph is its colour inside, the outline's in the band") {
    // A field that falls by one pixel per texel across a 16-texel page: the outline is at x = 6.5.
    // Drawn one texel a pixel with a range of four atlas pixels (shape[2] = 2 x 4 = 8 pixels for
    // the field's whole [0, 1]), pixels up to 6 are the glyph, 7 and 8 are a two-pixel outline,
    // and 9 on are nothing — the distance thresholding `text-and-fonts` asks outlined text to be
    // drawn by, rather than by drawing the text eight times.
    std::vector<u8> field(16U * 4U, 0);
    for (u32 texel = 0; texel < 16U; ++texel) {
        const f32 value = 0.5F + ((6.5F - static_cast<f32>(texel)) / 8.0F);
        const auto byte = static_cast<u8>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        for (u32 channel = 0; channel < 4U; ++channel) {
            field[(texel * 4U) + channel] = byte;
        }
    }
    const ReferenceAtlas page{Span<const u8>(field.data(), field.size()), 16, 1, 4};
    Primitive primitive;
    primitive.bounds = ui::Rect{0.0F, 0.0F, 16.0F, 1.0F};
    primitive.uv = ui::Rect{0.0F, 0.0F, 1.0F, 1.0F};
    primitive.material = material_index(BuiltinMaterial::GlyphField);
    primitive.colour = 0xFFFFFFFFU;
    primitive.border_width = 2.0F;
    primitive.border_colour = 0xFF000000U;
    primitive.distance_range = 8.0F;
    const GpuUiPrimitive row = encode_primitive(primitive, 1.0F);
    CY_CHECK_EQ(row.shape[2], 8.0F);
    CY_CHECK_EQ(decode_primitive(row, 2.0F).distance_range, 4.0F);

    f32 out[4] = {};
    CY_REQUIRE(shade_reference(row, 3, 0, &page, out));
    CY_CHECK_NEAR(out[0], 1.0F, 0.03F);  // white: the glyph
    CY_CHECK_NEAR(out[3], 1.0F, 0.03F);
    for (const u32 band : {7U, 8U}) {
        CY_REQUIRE(shade_reference(row, band, 0, &page, out));
        CY_CHECK_NEAR(out[0], 0.0F, 0.03F);  // black: the outline
        CY_CHECK_NEAR(out[3], 1.0F, 0.03F);
    }
    CY_CHECK_FALSE(shade_reference(row, 10, 0, &page, out));

    // Without an outline the band is empty: the glyph stops at its edge.
    GpuUiPrimitive plain = row;
    plain.shape[1] = 0.0F;
    CY_CHECK_FALSE(shade_reference(plain, 8, 0, &page, out));
    // And magnified twice, the same page draws the same edge twice as far out: one atlas entry,
    // any size.
    primitive.bounds.width = 32.0F;
    primitive.distance_range = 16.0F;
    const GpuUiPrimitive large = encode_primitive(primitive, 1.0F);
    CY_REQUIRE(shade_reference(large, 12, 0, &page, out));
    CY_CHECK_NEAR(out[0], 1.0F, 0.03F);
    CY_REQUIRE(shade_reference(large, 15, 0, &page, out));
    CY_CHECK_NEAR(out[0], 0.0F, 0.03F);
}
