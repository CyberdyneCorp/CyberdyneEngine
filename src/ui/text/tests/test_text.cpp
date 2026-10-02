// SPDX-License-Identifier: MIT
// `unit.ui_text`: the built-in font, and a label's text measured and painted through a running text
// server into CyberUI's primitive stream.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/layout.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/text_painter.h>

#include <cmath>
#include <string_view>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

constexpr u16 kGlyphPage = 1;

/// A running server and a painter started on the built-in font.
struct Fixture {
    Fixture() noexcept {
        cy::text::TextServerConfig config;
        config.atlas.initial_extent = 256;
        config.atlas.maximum_extent = 256;
        started = server.start(config).has_value() &&
                  painter.start(server, builtin_font(), kGlyphPage).has_value();
    }

    cy::text::TextServer server;
    TextPainter painter{allocator()};
    bool started = false;
};

/// The coverage of a glyph's top-left `columns x rows` corner in the painter's atlas.
[[nodiscard]] u8 atlas_at(const TextPainter& painter, const Primitive& glyph, u32 x,
                          u32 y) noexcept {
    const u32 extent = painter.atlas_extent();
    const auto left = static_cast<u32>(std::lround(glyph.uv.x * static_cast<f32>(extent)));
    const auto top = static_cast<u32>(std::lround(glyph.uv.y * static_cast<f32>(extent)));
    return painter.atlas_pixels()[(static_cast<usize>(top + y) * extent) + left + x];
}

}  // namespace

CY_TEST_CASE("ui_text: the built-in font is printable ASCII in six by thirteen cells") {
    const cy::text::ImageGridFont& font = builtin_font();
    CY_REQUIRE(font.validate().has_value());
    CY_CHECK_EQ(font.cell_width, kBuiltinFontCellWidth);
    CY_CHECK_EQ(font.cell_height, kBuiltinFontCellHeight);
    CY_CHECK_EQ(font.first_codepoint, 32U);
    CY_CHECK_EQ(font.glyph_count, 95U);
    // 'A' is cell 33: its apex is one lit pixel in the middle of row two, and its crossbar is the
    // five lit pixels of row seven — the misc-fixed glyph, read back from the grid.
    const u32 cell = 'A' - 32U;
    const u32 left = (cell % font.columns) * font.cell_width;
    const u32 top = (cell / font.columns) * font.cell_height;
    const auto at = [&](u32 x, u32 y) {
        return font.pixels[(static_cast<usize>(top + y) * font.image_width) + left + x];
    };
    CY_CHECK_EQ(at(2, 2), 255U);
    CY_CHECK_EQ(at(1, 2), 0U);
    for (u32 x = 0; x < 5U; ++x) {
        CY_CHECK_EQ(at(x, 7), 255U);
    }
    CY_CHECK_EQ(at(5, 7), 0U);
    // The space is empty.
    for (u32 y = 0; y < font.cell_height; ++y) {
        for (u32 x = 0; x < font.cell_width; ++x) {
            CY_CHECK_EQ(font.pixels[(static_cast<usize>(y) * font.image_width) + x], 0U);
        }
    }
}

CY_TEST_CASE("ui_text: a label measures to its advance by the line height, times its scale") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    TextStyle style;
    const Vec2 one = fixture.painter.measure("Gold 1250", style);
    CY_CHECK_EQ(one.x, 9.0F * 6.0F);
    CY_CHECK_EQ(one.y, 13.0F);
    style.pixel_scale = 2;
    const Vec2 two = fixture.painter.measure("Gold 1250", style);
    CY_CHECK_EQ(two.x, 2.0F * 9.0F * 6.0F);
    CY_CHECK_EQ(two.y, 26.0F);

    // As a `ContentMeasurer`, it answers for the element the text is attached to, and layout uses
    // it for a label with no preferred size.
    ElementStore store(allocator());
    auto root = store.create(kNoElement, Name::intern("panel"));
    CY_REQUIRE(root.has_value());
    store.layout_input(*root)->model = LayoutModel::Absolute;
    auto label = store.create(*root, Name::intern("label"));
    CY_REQUIRE(label.has_value());
    CY_REQUIRE(fixture.painter.set_text(*label, "Wood 830", style).has_value());
    const Vec2 measured = fixture.painter.measure_content(*label, Vec2{-1.0F, -1.0F});
    CY_CHECK_EQ(measured.x, 2.0F * 8.0F * 6.0F);
    CY_CHECK_EQ(measured.y, 26.0F);
}

CY_TEST_CASE("ui_text: glyphs are painted at the label's corner, from the atlas, spaces skipped") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    ElementStore store(allocator());
    auto label = store.create(kNoElement, Name::intern("label"));
    CY_REQUIRE(label.has_value());
    TextStyle style;
    style.colour = 0xFFFFD040U;
    style.pixel_scale = 2;
    CY_REQUIRE(fixture.painter.set_text(*label, "A b", style).has_value());

    Array<Primitive> glyphs(allocator());
    CY_REQUIRE(fixture.painter.paint_content(*label, ui::Rect{100.0F, 50.0F, 200.0F, 30.0F}, glyphs)
                   .has_value());
    CY_REQUIRE_EQ(glyphs.size(), 2U);
    // Cell-aligned: the first glyph's cell starts at the label's corner, the second two advances
    // on (the space between them drew nothing), each a cell at twice the font's pixels.
    CY_CHECK_EQ(glyphs[0].bounds.x, 100.0F);
    CY_CHECK_EQ(glyphs[0].bounds.y, 50.0F);
    CY_CHECK_EQ(glyphs[0].bounds.width, 12.0F);
    CY_CHECK_EQ(glyphs[0].bounds.height, 26.0F);
    CY_CHECK_EQ(glyphs[1].bounds.x, 100.0F + (2.0F * 12.0F));
    for (const Primitive& glyph : glyphs) {
        CY_CHECK_EQ(glyph.material, material_index(BuiltinMaterial::Glyph));
        CY_CHECK_EQ(glyph.atlas, kGlyphPage);
        CY_CHECK_EQ(glyph.colour, 0xFFFFD040U);
        CY_CHECK_GT(glyph.uv.width, 0.0F);
        CY_CHECK_LE(glyph.uv.x + glyph.uv.width, 1.0F);
    }
    // The uv names the glyph's own pixels in the atlas: 'A''s apex and crossbar.
    CY_CHECK_EQ(atlas_at(fixture.painter, glyphs[0], 2, 2), 255U);
    CY_CHECK_EQ(atlas_at(fixture.painter, glyphs[0], 1, 2), 0U);
    CY_CHECK_EQ(atlas_at(fixture.painter, glyphs[0], 0, 7), 255U);
}

CY_TEST_CASE("ui_text: the atlas is warmed at start and painting never grows it") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const u32 revision = fixture.painter.atlas_revision();
    const usize resident = fixture.server.atlas().live_glyphs();
    // Every printable character and `.notdef`.
    CY_CHECK_EQ(resident, 96U);
    ElementStore store(allocator());
    auto label = store.create(kNoElement, Name::intern("label"));
    CY_REQUIRE(label.has_value());
    CY_REQUIRE(fixture.painter.set_text(*label, "The quick brown fox jumps over 13 lazy dogs!")
                   .has_value());
    Array<Primitive> glyphs(allocator());
    CY_REQUIRE(fixture.painter.paint_content(*label, ui::Rect{0.0F, 0.0F, 400.0F, 20.0F}, glyphs)
                   .has_value());
    CY_CHECK_EQ(fixture.server.atlas().live_glyphs(), resident);
    CY_CHECK_EQ(fixture.painter.atlas_revision(), revision);
}

CY_TEST_CASE("ui_text: replacing and clearing text keeps every other label's text") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    ElementStore store(allocator());
    ElementId labels[3];
    for (ElementId& label : labels) {
        auto created = store.create(kNoElement, Name::intern("label"));
        CY_REQUIRE(created.has_value());
        label = *created;
    }
    CY_REQUIRE(fixture.painter.set_text(labels[0], "first").has_value());
    CY_REQUIRE(fixture.painter.set_text(labels[1], "second").has_value());
    CY_REQUIRE(fixture.painter.set_text(labels[2], "third").has_value());
    // Growing a text many times leaves dead characters behind, which compaction reclaims without
    // disturbing the others.
    for (u32 round = 0; round < 40U; ++round) {
        CY_REQUIRE(fixture.painter
                       .set_text(labels[1], round % 2U == 0U ? "second, longer now"
                                                             : "second, and longer still")
                       .has_value());
    }
    CY_REQUIRE(fixture.painter.set_text(labels[0], "1st").has_value());
    CY_CHECK(fixture.painter.text_of(labels[0]) == std::string_view("1st"));
    CY_CHECK(fixture.painter.text_of(labels[1]) == std::string_view("second, and longer still"));
    CY_CHECK(fixture.painter.text_of(labels[2]) == std::string_view("third"));
    fixture.painter.clear_text(labels[2]);
    CY_CHECK(fixture.painter.text_of(labels[2]).empty());
    Array<Primitive> glyphs(allocator());
    CY_REQUIRE(fixture.painter.paint_content(labels[2], ui::Rect{0.0F, 0.0F, 100.0F, 20.0F}, glyphs)
                   .has_value());
    CY_CHECK(glyphs.empty());
}
