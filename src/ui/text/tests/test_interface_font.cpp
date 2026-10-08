// SPDX-License-Identifier: MIT
// `integration.ui_interface_font`: CyberUI's text in the interface font, through the complete text
// backend — distance-field glyphs, one batch a line, the effects, and the atlas staying put while a
// frame paints. Issue #86. Built only with CY_TEXT; `unit.ui_text` covers the built-in font.

#include <cy/backends/text/complete_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/layout.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/interface_font.h>
#include <cy/ui/text/text_painter.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

using namespace cy;
using namespace cy::ui;
using cy::text::PixelFormat;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

constexpr u16 kFirstPage = 1;

/// A backend, a server over it, and a painter started on the interface font.
struct Interface {
    cy::text::CompleteTextBackend backend;
    cy::text::TextServer server;
    TextPainter painter{allocator()};
    ElementStore store{allocator()};

    explicit Interface(cy::text::FontSource source = {ui::interface_font_bytes(), 0},
                       cy::text::FontDesc desc = ui::interface_font_desc()) {
        CY_REQUIRE(backend.start().has_value());
        cy::text::TextServerConfig config;
        config.atlas.initial_extent = 512;
        config.atlas.maximum_extent = 2048;
        CY_REQUIRE(server.start_with(config, backend).has_value());
        const Status started = painter.start(server, source, desc, kFirstPage);
        CY_REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().message));
    }
    ~Interface() {
        server.stop();
        backend.stop();
    }
    Interface(const Interface&) = delete;
    Interface& operator=(const Interface&) = delete;
    Interface(Interface&&) = delete;
    Interface& operator=(Interface&&) = delete;

    [[nodiscard]] ElementId label(std::string_view text, const TextStyle& style) {
        auto made = store.create(kNoElement, Name::intern("label"));
        CY_REQUIRE(made.has_value());
        CY_REQUIRE(painter.set_text(*made, text, style).has_value());
        return *made;
    }

    [[nodiscard]] Array<Primitive> paint(ElementId element, f32 x = 10.0F, f32 y = 20.0F) {
        Array<Primitive> out(allocator());
        CY_REQUIRE(painter.paint_content(element, ui::Rect{x, y, 400.0F, 40.0F}, out).has_value());
        return out;
    }
};

[[nodiscard]] Array<u8> read_font(const char* name) {
    const std::string path = std::string(CY_TEXT_FONTS_DIR) + "/" + name;
    Array<u8> bytes;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    CY_REQUIRE(file != nullptr);
    u8 chunk[4096];
    usize read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        CY_REQUIRE(bytes.append(Span<const u8>(chunk, read)).has_value());
    }
    std::fclose(file);
    return bytes;
}

[[nodiscard]] u32 channel(u32 colour, u32 shift) {
    return (colour >> shift) & 0xFFU;
}

}  // namespace

CY_TEST_CASE("interface font: compiled in with the complete backend, as a distance field") {
    CY_CHECK(has_interface_font());
    CY_CHECK_GT(interface_font_bytes().size(), 1000U);
    CY_CHECK(interface_font_desc().mode == cy::text::RenderMode::SignedDistanceField);
}

CY_TEST_CASE(
    "interface font: a label is distance-field glyphs from one page, so a line is one batch") {
    Interface ui;
    TextStyle style;
    style.colour = 0xFFFFD040U;
    const ElementId label = ui.label("Selected: 4 units (1 more)", style);
    const Array<Primitive> glyphs = ui.paint(label);
    CY_REQUIRE_EQ(glyphs.size(), 22U);  // 26 characters, four of them spaces
    for (const Primitive& glyph : glyphs) {
        CY_CHECK_EQ(glyph.material, material_index(BuiltinMaterial::GlyphField));
        CY_CHECK_EQ(glyph.atlas, ui.painter.atlas_page(PixelFormat::DistanceField));
        CY_CHECK_EQ(glyph.colour, 0xFFFFD040U);
        CY_CHECK_GT(glyph.distance_range, 0.0F);
    }
    CY_CHECK_EQ(ui.painter.atlas_page(PixelFormat::DistanceField), kFirstPage + 1);

    // flatten() breaks batches on material and page; a line of one face in one format is one.
    ui.store.layout_input(label)->model = LayoutModel::Absolute;
    ui.store.layout_input(label)->offset_max = Vec2{300.0F, 20.0F};
    ScaleSettings settings;
    settings.mode = ScaleMode::FixedPixel;
    LayoutReport laid{};
    CY_REQUIRE(layout(ui.store, settings, Vec2{400.0F, 300.0F}, &ui.painter, laid).has_value());
    PrimitiveBuffer buffer(allocator());
    FlattenReport flattened{};
    CY_REQUIRE(
        flatten(ui.store, ui::Rect{0.0F, 0.0F, 400.0F, 300.0F}, buffer, flattened, &ui.painter)
            .has_value());
    CY_CHECK_EQ(buffer.primitives().size(), 22U);
    CY_CHECK_EQ(flattened.batches, 1U);
}

CY_TEST_CASE(
    "interface font: a line is the built-in font's height, so its layouts keep their rows") {
    Interface ui;
    TextStyle style;
    CY_CHECK_NEAR(ui.painter.measure("Rifleman", style).y, 13.0F, 1e-4F);
    style.pixel_scale = 2;
    CY_CHECK_NEAR(ui.painter.measure("Rifleman", style).y, 26.0F, 1e-4F);
    // A proportional face: "iiii" is far narrower than "MMMM", which a grid font cannot say.
    style.pixel_scale = 1;
    CY_CHECK_LT(ui.painter.measure("iiii", style).x * 2.0F, ui.painter.measure("MMMM", style).x);
}

CY_TEST_CASE("interface font: a size scales the quads and the distance range together") {
    Interface ui;
    TextStyle small;
    small.size = 16.0F;
    TextStyle large = small;
    large.size = 32.0F;
    const Array<Primitive> a = ui.paint(ui.label("Ag", small), 0.0F, 0.0F);
    const Array<Primitive> b = ui.paint(ui.label("Ag", large), 0.0F, 0.0F);
    CY_REQUIRE_EQ(a.size(), 2U);
    CY_REQUIRE_EQ(b.size(), 2U);
    for (usize index = 0; index < 2; ++index) {
        CY_CHECK_NEAR(b[index].bounds.width, 2.0F * a[index].bounds.width, 1e-3F);
        CY_CHECK_NEAR(b[index].bounds.x, 2.0F * a[index].bounds.x, 1e-3F);
        CY_CHECK_NEAR(b[index].distance_range, 2.0F * a[index].distance_range, 1e-3F);
        // The SAME atlas entry: magnifying a distance field never rasterises again.
        CY_CHECK_EQ(b[index].uv.x, a[index].uv.x);
        CY_CHECK_EQ(b[index].uv.y, a[index].uv.y);
    }
    // At the face's own 32 pixels, the range is twice the face's four atlas pixels.
    CY_CHECK_NEAR(b[0].distance_range, 8.0F, 1e-4F);
}

CY_TEST_CASE("interface font: an outline and a shadow come from the same entry, in one batch") {
    Interface ui;
    TextStyle style;
    style.colour = 0xFFFFFFFFU;
    style.outline_width = 1.5F;
    style.outline_colour = 0xFF000000U;
    style.shadow_colour = 0x80000000U;
    style.shadow_offset = Vec2{2.0F, 3.0F};
    const Array<Primitive> glyphs = ui.paint(ui.label("HUD", style));
    CY_REQUIRE_EQ(glyphs.size(), 6U);
    for (usize index = 0; index < 3; ++index) {
        const Primitive& shadow = glyphs[index];
        const Primitive& glyph = glyphs[index + 3];
        // Every shadow first, so no shadow falls over a glyph drawn before it.
        CY_CHECK_EQ(shadow.colour, 0x80000000U);
        CY_CHECK_EQ(shadow.border_width, 0.0F);
        CY_CHECK_NEAR(shadow.bounds.x, glyph.bounds.x + 2.0F, 1e-4F);
        CY_CHECK_NEAR(shadow.bounds.y, glyph.bounds.y + 3.0F, 1e-4F);
        CY_CHECK_EQ(shadow.uv.x, glyph.uv.x);
        CY_CHECK_EQ(shadow.material, glyph.material);
        CY_CHECK_EQ(shadow.atlas, glyph.atlas);
        // The outline is the glyph's own primitive's band, not eight copies of the glyph.
        CY_CHECK_EQ(glyph.border_width, 1.5F);
        CY_CHECK_EQ(glyph.border_colour, 0xFF000000U);
    }
}

CY_TEST_CASE("interface font: a gradient runs across the line and a colour span overrides it") {
    Interface ui;
    TextStyle style;
    style.colour = 0xFFFF0000U;
    style.gradient_colour = 0xFF0000FFU;
    const ElementId label = ui.label("MMMMMMMM", style);
    Array<Primitive> glyphs = ui.paint(label);
    CY_REQUIRE_EQ(glyphs.size(), 8U);
    // Red at the start, blue at the end, every step between bluer than the one before.
    CY_CHECK_GT(channel(glyphs[0].colour, 16), 200U);
    CY_CHECK_LT(channel(glyphs[0].colour, 0), 55U);
    CY_CHECK_GT(channel(glyphs[7].colour, 0), 200U);
    for (usize index = 1; index < glyphs.size(); ++index) {
        CY_CHECK_GT(channel(glyphs[index].colour, 0), channel(glyphs[index - 1].colour, 0));
    }
    // Per-character colour: bytes 2 and 3 green, over the gradient.
    const ColourSpan spans[] = {{2, 4, 0xFF00FF00U}};
    CY_REQUIRE(ui.painter.set_colours(label, spans).has_value());
    glyphs = ui.paint(label);
    CY_CHECK_EQ(glyphs[2].colour, 0xFF00FF00U);
    CY_CHECK_EQ(glyphs[3].colour, 0xFF00FF00U);
    CY_CHECK_NE(glyphs[4].colour, 0xFF00FF00U);
}

CY_TEST_CASE(
    "interface font: setting text makes its glyphs resident, so painting never grows a page") {
    Interface ui;
    const u32 before = ui.painter.atlas_revision();
    const usize warmed = ui.server.atlas(PixelFormat::DistanceField).live_glyphs();
    TextStyle style;
    // Not ASCII, so not warmed at start: setting it is what rasterises it.
    const ElementId label = ui.label(
        "Na\xc3\xafve \xc3\xb1"
        "and\xc3\xba",
        style);
    CY_CHECK_GT(ui.server.atlas(PixelFormat::DistanceField).live_glyphs(), warmed);
    const u32 after_set = ui.painter.atlas_revision();
    CY_CHECK_NE(after_set, before);
    const usize resident = ui.server.atlas(PixelFormat::DistanceField).live_glyphs();
    (void)ui.paint(label);
    CY_CHECK_EQ(ui.server.atlas(PixelFormat::DistanceField).live_glyphs(), resident);
    CY_CHECK_EQ(ui.painter.atlas_revision(), after_set);
}

CY_TEST_CASE("interface font: a colour glyph draws as an image from the colour page") {
    const Array<u8> font = read_font("CyberColourTest.ttf");
    cy::text::FontDesc desc;
    desc.family = "colour";
    desc.size_pixels = 32.0F;
    Interface ui(cy::text::FontSource{Span<const u8>(font.data(), font.size()), 0}, desc);
    TextStyle style;
    style.colour = 0x80FFD040U;  // half transparent: only the alpha reaches a colour glyph
    const Array<Primitive> glyphs = ui.paint(ui.label("\xe2\x96\xa0", style));
    CY_REQUIRE_EQ(glyphs.size(), 1U);
    CY_CHECK_EQ(glyphs[0].material, material_index(BuiltinMaterial::Image));
    CY_CHECK_EQ(glyphs[0].atlas, kFirstPage + 2);
    CY_CHECK_EQ(channel(glyphs[0].colour, 24), 0x80U);
    CY_CHECK_EQ(channel(glyphs[0].colour, 16), 0x80U);  // premultiplied white
    CY_CHECK_GT(ui.painter.atlas_extent(PixelFormat::Colour), 0U);
}

CY_TEST_CASE(
    "interface font: a server with no backend is refused, and the built-in font still starts") {
    cy::text::TextServer server;
    CY_REQUIRE(server.start(cy::text::TextServerConfig{}).has_value());
    TextPainter painter(allocator());
    CY_CHECK_FALSE(painter
                       .start(server, cy::text::FontSource{interface_font_bytes(), 0},
                              interface_font_desc(), kFirstPage)
                       .has_value());
    CY_CHECK_FALSE(painter.is_running());
    CY_REQUIRE(painter.start(server, builtin_font(), kFirstPage).has_value());
    CY_CHECK_FALSE(painter.outline());
}
