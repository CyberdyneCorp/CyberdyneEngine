// SPDX-License-Identifier: MIT
// The text server over a backend: runs by face, bidirectional lines, the three atlases, cooked
// fonts and the per-frame report. M11.e, issue #86.
//
// Over `FakeBackend` (fake_backend.h), so every case is the server's logic and nothing else, and
// the suite runs in a build with CY_TEXT off. The same paths over FreeType and HarfBuzz are
// `integration.text_complete`.

#include "fake_backend.h"
#include "grid_font.h"

#include <cy/servers/text/cooked_font.h>
#include <cy/servers/text/server.h>
#include <cy/test/test.h>

#include <string_view>

using namespace cy::text;
using cy::Array;
using cy::Error;
using cy::ErrorCode;
using cy::Expected;
using cy::f32;
using cy::Span;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

TextServerConfig small_config() {
    TextServerConfig config;
    config.atlas.initial_extent = 64;
    config.atlas.maximum_extent = 256;
    return config;
}

/// A server started over a fake backend, stopped before the backend goes.
struct Faked {
    test::FakeBackend backend;
    TextServer server;

    Faked() { CY_REQUIRE(server.start_with(small_config(), backend).has_value()); }
    ~Faked() { server.stop(); }
    Faked(const Faked&) = delete;
    Faked& operator=(const Faked&) = delete;
    Faked(Faked&&) = delete;
    Faked& operator=(Faked&&) = delete;

    FontHandle face(std::string_view family, RenderMode mode = RenderMode::Grayscale) {
        Expected<FontHandle, Error> made =
            server.create_face(test::fake_desc(family, mode), test::fake_source());
        CY_REQUIRE(made.has_value());
        return made.value();
    }
};

FallbackChain chain_of(FontHandle first, FontHandle second = {}) {
    FallbackChain chain;
    CY_REQUIRE(chain.push(first).has_value());
    if (!second.is_null()) {
        CY_REQUIRE(chain.push(second).has_value());
    }
    return chain;
}

}  // namespace

CY_TEST_CASE("backend: an outline face on the minimal backend is refused, naming the option") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    const auto refused = server.create_face(test::fake_desc("latin"), test::fake_source());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unsupported);
    CY_CHECK(std::string_view(refused.error().message).find("CY_TEXT") != std::string_view::npos);
    CY_CHECK_FALSE(server.capabilities().outline_fonts);
}

CY_TEST_CASE("backend: the server reports the backend's capabilities and name as its own") {
    Faked faked;
    CY_CHECK(faked.server.capabilities().complex_shaping);
    CY_CHECK(faked.server.capabilities().outline_fonts);
    CY_CHECK(std::string_view(faked.server.capabilities().backend) == "fake");
}

CY_TEST_CASE("backend: text is shaped a run per face, and the fallback face gets whole runs") {
    // "ab" in the Latin face, the Hebrew word in the Hebrew one, "c" back in Latin: three runs, so
    // three calls to the shaper rather than one per character.
    Faked faked;
    const FallbackChain chain = chain_of(faked.face("latin"), faked.face("hebrew"));
    ShapedRun run;
    CY_REQUIRE(faked.server
                   .shape("ab\xd7\xa9\xd7\x9c"
                          "c",
                          chain, Direction::LeftToRight, run)
                   .has_value());
    CY_CHECK_EQ(faked.backend.shapes, 3U);
    CY_REQUIRE_EQ(run.glyphs.size(), 5U);
    const u32 offsets[] = {0, 1, 2, 4, 6};
    for (usize index = 0; index < 5; ++index) {
        CY_CHECK_EQ(run.glyphs[index].source_offset, offsets[index]);
        CY_CHECK_EQ(run.glyphs[index].offset.x,
                    static_cast<f32>(index) * test::FakeBackend::kAdvance);
        CY_CHECK_FALSE(run.glyphs[index].missing);
    }
    CY_CHECK(run.glyphs[2].face == chain.faces[1]);
    CY_CHECK_EQ(run.width, 50.0f);
}

CY_TEST_CASE("backend: a combining mark stays in its base character's face") {
    // U+0301 is in the Latin primary and not in the Hebrew fallback, U+05B8 the other way round.
    // Each follows a base from the other face, and each is shaped with its base, in one run.
    Faked faked;
    const FallbackChain chain = chain_of(faked.face("latin"), faked.face("hebrew"));
    ShapedRun run;
    CY_REQUIRE(
        faked.server.shape("\xd7\xa9\xcc\x81", chain, Direction::LeftToRight, run).has_value());
    CY_CHECK_EQ(faked.backend.shapes, 1U);
    CY_REQUIRE_EQ(run.glyphs.size(), 2U);
    CY_CHECK(run.glyphs[0].face == chain.faces[1]);
    CY_CHECK(run.glyphs[1].face == chain.faces[1]);

    ShapedRun latin;
    CY_REQUIRE(faked.server.shape("a\xd6\xb8", chain, Direction::LeftToRight, latin).has_value());
    CY_CHECK_EQ(faked.backend.shapes, 2U);
    CY_REQUIRE_EQ(latin.glyphs.size(), 2U);
    CY_CHECK(latin.glyphs[1].face == chain.faces[0]);
}

CY_TEST_CASE("backend: a newline ends a run and draws nothing") {
    Faked faked;
    ShapedRun run;
    CY_REQUIRE(
        faked.server.shape("a\nb", chain_of(faked.face("latin")), Direction::LeftToRight, run)
            .has_value());
    CY_REQUIRE_EQ(run.glyphs.size(), 2U);
    CY_CHECK_EQ(run.glyphs[1].source_offset, 2U);
    CY_CHECK_EQ(faked.backend.shapes, 2U);
}

CY_TEST_CASE("backend: a right-to-left run lists its face runs right to left") {
    // Two face runs in a right-to-left run: the LAST in logical order is the LEFTMOST on the page.
    Faked faked;
    const FallbackChain chain = chain_of(faked.face("hebrew"), faked.face("latin"));
    ShapedRun run;
    CY_REQUIRE(faked.server
                   .shape("\xd7\xa9\xd7\x9c"
                          "ab",
                          chain, Direction::RightToLeft, run)
                   .has_value());
    CY_REQUIRE_EQ(run.glyphs.size(), 4U);
    // "ab" (bytes 4 and 5) reversed by the fake shaper as a real one would not — the fake reverses
    // every right-to-left run — then the Hebrew (bytes 2 and 0) reversed within itself.
    CY_CHECK_EQ(run.glyphs[0].source_offset, 5U);
    CY_CHECK_EQ(run.glyphs[1].source_offset, 4U);
    CY_CHECK_EQ(run.glyphs[2].source_offset, 2U);
    CY_CHECK_EQ(run.glyphs[3].source_offset, 0U);
}

CY_TEST_CASE("backend: a mixed line is placed in visual order with src/text's levels") {
    // The fake backend has no bidirectional algorithm, so this is the in-tree one deciding the
    // levels and the server placing the runs: "ab שלום cd" draws the Hebrew word last letter first.
    Faked faked;
    const FallbackChain chain = chain_of(faked.face("latin"), faked.face("hebrew"));
    TextLine line;
    CY_REQUIRE(faked.server.layout_line("ab \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d cd", chain, line)
                   .has_value());
    const u32 visual[] = {0, 1, 2, 9, 7, 5, 3, 11, 12, 13};
    CY_REQUIRE_EQ(line.run().glyphs.size(), std::size(visual));
    for (usize index = 0; index < std::size(visual); ++index) {
        CY_CHECK_EQ(line.run().glyphs[index].source_offset, visual[index]);
        CY_CHECK_EQ(line.run().glyphs[index].offset.x,
                    static_cast<f32>(index) * test::FakeBackend::kAdvance);
    }
    CY_CHECK_EQ(line.width(), 100.0f);
}

CY_TEST_CASE(
    "backend: a number in right-to-left text keeps its digits in order and its runs in place") {
    // "של 12 אב": the digits are a level-2 run inside level-1 Hebrew, so the three level runs are
    // drawn last first — "אב", then "12" left to right, then "של" — which is L2's visual order and
    // not the runs' logical one. A line that placed its runs in logical order would draw "של"
    // first.
    Faked faked;
    const FallbackChain chain = chain_of(faked.face("hebrew"), faked.face("latin"));
    TextLine line;
    CY_REQUIRE(
        faked.server.layout_line("\xd7\xa9\xd7\x9c 12 \xd7\x90\xd7\x91", chain, line).has_value());
    const u32 visual[] = {10, 8, 7, 5, 6, 4, 2, 0};
    CY_REQUIRE_EQ(line.run().glyphs.size(), std::size(visual));
    for (usize index = 0; index < std::size(visual); ++index) {
        CY_CHECK_EQ(line.run().glyphs[index].source_offset, visual[index]);
    }
}

CY_TEST_CASE("backend: a right-to-left paragraph starts at the right") {
    Faked faked;
    const FallbackChain chain = chain_of(faked.face("hebrew"));
    ParagraphOptions options;
    options.width = 100.0f;
    options.direction = Direction::RightToLeft;
    TextParagraph paragraph;
    CY_REQUIRE(
        faked.server.layout_paragraph("\xd7\xa9\xd7\x9c", chain, options, paragraph).has_value());
    CY_REQUIRE_EQ(paragraph.line_count(), 1U);
    CY_CHECK_EQ(paragraph.line_origin(0).x, 80.0f);
}

CY_TEST_CASE("backend: each raster lands in the atlas of its format") {
    Faked faked;
    const FontHandle plain = faked.face("latin");
    const FontHandle field = faked.face("latin", RenderMode::SignedDistanceField);
    CY_CHECK_FALSE(faked.server.atlas(PixelFormat::DistanceField).is_running());

    const GlyphSlot* coverage = faked.server.glyph_slot(plain, 'a').value();
    const GlyphSlot* distance = faked.server.glyph_slot(field, 'a').value();
    const GlyphSlot* colour =
        faked.server.glyph_slot(plain, test::FakeBackend::kColourGlyph).value();
    CY_CHECK_EQ(coverage->page, static_cast<u32>(PixelFormat::Coverage));
    CY_CHECK_EQ(distance->page, static_cast<u32>(PixelFormat::DistanceField));
    CY_CHECK_EQ(colour->page, static_cast<u32>(PixelFormat::Colour));
    CY_CHECK(colour->colour);
    CY_CHECK_FALSE(coverage->colour);
    CY_CHECK_EQ(faked.server.atlas(PixelFormat::DistanceField).bytes_per_pixel(), 4U);
    CY_CHECK_EQ(faked.server.atlas(PixelFormat::Colour).live_glyphs(), 1U);

    // A second request for each finds it, whichever atlas it is in, and rasterises nothing.
    CY_CHECK(faked.server.glyph_slot(plain, test::FakeBackend::kColourGlyph).has_value());
    CY_CHECK(faked.server.glyph_slot(field, 'a').has_value());
    CY_CHECK_EQ(faked.backend.rasterisations, 3U);
    CY_CHECK_EQ(faked.server.diagnostics().glyphs_rasterised, 3U);
}

CY_TEST_CASE("backend: destroying a face, and stopping the server, closes it in the backend") {
    Faked faked;
    const FontHandle face = faked.face("latin");
    (void)faked.face("hebrew");
    CY_CHECK_EQ(faked.backend.open_faces, 2);
    faked.server.destroy_face(face);
    CY_CHECK_EQ(faked.backend.open_faces, 1);
    faked.server.stop();
    CY_CHECK_EQ(faked.backend.open_faces, 0);
}

CY_TEST_CASE("cooked: a cooked font round-trips, and its pre-rendered glyphs are not rasterised") {
    // What the importer writes, read back by the server: the face is made, every pre-rendered glyph
    // goes into the atlas as PRELOADED, and laying out the cooked range rasterises nothing.
    u8 page[16 * 16] = {};
    for (usize index = 0; index < sizeof(page); ++index) {
        page[index] = static_cast<u8>(index);
    }
    CookedGlyph glyphs[2];
    glyphs[0].glyph = 'a';
    glyphs[0].metrics = GlyphMetrics{10.0f, 1.0f, -8.0f, 6, 8};
    glyphs[1] = glyphs[0];
    glyphs[1].glyph = 'b';
    glyphs[1].x = 7;
    const CodepointRange ranges[] = {{'a', 'b'}};
    const std::string_view fallbacks[] = {"hebrew"};
    CookedFontContent content;
    content.desc = test::fake_desc("latin");
    content.source = test::fake_source();
    content.ranges = ranges;
    content.fallbacks = fallbacks;
    content.glyphs = glyphs;
    content.pages[0] = CookedPage{16, Span<const u8>(page, sizeof(page))};
    Array<u8> bytes;
    CY_REQUIRE(write_cooked_font(content, bytes).has_value());

    CookedFont cooked;
    CY_REQUIRE(cooked.parse(Span<const u8>(bytes.data(), bytes.size())).has_value());
    CY_CHECK(cooked.desc().family == "latin");
    CY_CHECK_EQ(cooked.source().bytes.size(), sizeof(test::kFakeFontBytes));
    CY_REQUIRE_EQ(cooked.glyphs().size(), 2U);
    CY_CHECK_EQ(cooked.glyphs()[1].x, 7U);
    CY_REQUIRE_EQ(cooked.fallbacks().size(), 1U);
    CY_CHECK(cooked.fallbacks()[0] == "hebrew");
    CY_CHECK(cooked.covers('b'));
    CY_CHECK_FALSE(cooked.covers('c'));

    Faked faked;
    const Expected<FontHandle, Error> face = faked.server.create_face(cooked);
    CY_REQUIRE(face.has_value());
    CY_CHECK_EQ(faked.server.diagnostics().glyphs_preloaded, 2U);
    TextLine line;
    CY_REQUIRE(faked.server.layout_line("abab", chain_of(face.value()), line).has_value());
    for (const ShapedGlyph& glyph : line.run().glyphs) {
        const GlyphSlot* slot = faked.server.glyph_slot(glyph.face, glyph.glyph).value();
        // The cooked page's pixels, copied: row 0 of 'b' starts at page byte 7.
        if (glyph.glyph == 'b') {
            const auto& atlas = faked.server.atlas();
            CY_CHECK_EQ(
                atlas.pixels()[(static_cast<usize>(slot->rect.position.y) * atlas.extent()) +
                               static_cast<usize>(slot->rect.position.x)],
                7U);
        }
    }
    CY_CHECK_EQ(faked.backend.rasterisations, 0U);
    CY_CHECK_EQ(faked.server.diagnostics().glyphs_rasterised, 0U);
    // A glyph outside the cooked range is rasterised as usual, and counted as such.
    CY_CHECK(faked.server.glyph_slot(face.value(), 'z').has_value());
    CY_CHECK_EQ(faked.server.diagnostics().glyphs_rasterised, 1U);
}

CY_TEST_CASE("cooked: every truncation of a cooked font is refused rather than read past its end") {
    CookedFontContent content;
    content.desc = test::fake_desc("latin");
    content.source = test::fake_source();
    u8 page[4 * 4] = {};
    CookedGlyph glyph;
    glyph.glyph = 'a';
    glyph.metrics = GlyphMetrics{4.0f, 0.0f, -4.0f, 4, 4};
    content.glyphs = Span<const CookedGlyph>(&glyph, 1);
    content.pages[0] = CookedPage{4, Span<const u8>(page, sizeof(page))};
    Array<u8> bytes;
    CY_REQUIRE(write_cooked_font(content, bytes).has_value());
    for (usize length = 0; length < bytes.size(); ++length) {
        CookedFont cooked;
        CY_CHECK_FALSE(cooked.parse(Span<const u8>(bytes.data(), length)).has_value());
    }
    // A glyph whose rectangle leaves its page is refused by name, so the server never copies past
    // it.
    glyph.x = 2;
    CY_REQUIRE(write_cooked_font(content, bytes).has_value());
    CookedFont cooked;
    const auto refused = cooked.parse(Span<const u8>(bytes.data(), bytes.size()));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(std::string_view(refused.error().message).find("outside its page") !=
             std::string_view::npos);
}

CY_TEST_CASE("cooked: another version is refused with the instruction to re-cook") {
    CookedFontContent content;
    content.desc = test::fake_desc("latin");
    content.source = test::fake_source();
    Array<u8> bytes;
    CY_REQUIRE(write_cooked_font(content, bytes).has_value());
    bytes[8] = static_cast<u8>(kCookedFontVersion + 1);
    CookedFont cooked;
    const auto refused = cooked.parse(Span<const u8>(bytes.data(), bytes.size()));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unsupported);
}

CY_TEST_CASE("diagnostics: a frame that rasterises past the threshold is a spike") {
    TextServerConfig config = small_config();
    config.rasterisation_spike = 3;
    TextServer server;
    CY_REQUIRE(server.start(config).has_value());
    test::GridFont source(8, 8, 16);
    const FontHandle face = server.create_face(FontDesc{}, source.font()).value();

    // Frame one: two glyphs. Frame two: five. Frame three: none — everything is resident.
    for (u32 index = 0; index < 2; ++index) {
        CY_REQUIRE(server.glyph_slot(face, index + 1).has_value());
    }
    server.end_frame();
    for (u32 index = 2; index < 7; ++index) {
        CY_REQUIRE(server.glyph_slot(face, index + 1).has_value());
    }
    server.end_frame();
    CY_CHECK_EQ(server.diagnostics().rasterised_last_frame, 5U);
    for (u32 index = 0; index < 7; ++index) {
        CY_REQUIRE(server.glyph_slot(face, index + 1).has_value());
    }
    server.end_frame();

    const TextDiagnostics diagnostics = server.diagnostics();
    CY_CHECK_EQ(diagnostics.frames, 3U);
    CY_CHECK_EQ(diagnostics.rasterised_last_frame, 0U);
    CY_CHECK_EQ(diagnostics.peak_frame_rasterisations, 5U);
    CY_CHECK_EQ(diagnostics.rasterisation_spikes, 1U);
    CY_CHECK_EQ(diagnostics.glyphs_rasterised, 7U);
}

CY_TEST_CASE("diagnostics: the fallback report counts against the primary that needed it") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont low(8, 8, 16, 'a');   // a to p
    test::GridFont high(8, 8, 16, 'q');  // q onwards
    const FontHandle primary = server.create_face(FontDesc{}, low.font()).value();
    const FontHandle fallback = server.create_face(FontDesc{}, high.font()).value();
    ShapedRun run;
    CY_REQUIRE(
        server.shape("abqr", chain_of(primary, fallback), Direction::LeftToRight, run).has_value());
    Array<FallbackReport> report;
    CY_REQUIRE(server.fallback_report(report).has_value());
    CY_REQUIRE_EQ(report.size(), 1U);
    CY_CHECK_EQ(report[0].face, primary.bits());
    CY_CHECK_EQ(report[0].codepoints, 4U);
    CY_CHECK_EQ(report[0].fallbacks, 2U);
}
