// SPDX-License-Identifier: MIT
// `integration.import_font`: the font importer, and text laid out from what it cooks. Issue #86.
//
// The case `text-and-fonts` names — "WHEN a font is imported with the Latin range pre-rendered THEN
// those glyphs SHALL be present in the cooked atlas and require no runtime rasterisation" — is the
// first one, and it is asserted from the RUNTIME's side: a text server reads the cooked font and
// lays Latin out, and its own rasterisation counter is what must stay at zero.

#include <cy/import/font.h>
#include <cy/import/pipeline.h>
#include <cy/test/test.h>
#include <cy_features.h>

#if defined(CY_TEXT)
#    include <cy/backends/text/complete_backend.h>
#    include <cy/servers/text/server.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

using namespace cy;
using namespace cy::import;

namespace {

Array<u8> read_font(const char* name) {
    const std::string path = std::string(CY_TEXT_FONTS_DIR) + "/" + name;
    Array<u8> bytes;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    CY_REQUIRE_MESSAGE(file != nullptr, "missing test font " << path);
    u8 chunk[4096];
    usize read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        CY_REQUIRE(bytes.append(Span<const u8>(chunk, read)).has_value());
    }
    std::fclose(file);
    return bytes;
}

[[nodiscard]] ImportResult import_font(const Array<u8>& bytes, const ImportOptions* options,
                                       std::string_view path = "fonts/NotoSans.ttf") {
    FontImporter importer;
    ImportRequest request;
    request.source = assets::VirtualPath::normalise(path).value();
    request.bytes = Span<const u8>(bytes.data(), bytes.size());
    request.options = options;
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    return result;
}

[[nodiscard]] bool reported(const ImportResult& result, std::string_view code) {
    for (const ImportDiagnostic& diagnostic : result.diagnostics()) {
        if (code == diagnostic.code) {
            return true;
        }
    }
    return false;
}

}  // namespace

#if defined(CY_TEXT)

namespace {

/// A started backend and a server over it, in the order they must stop.
struct Runtime {
    text::CompleteTextBackend backend;
    text::TextServer server;

    Runtime() {
        CY_REQUIRE(backend.start().has_value());
        text::TextServerConfig config;
        config.atlas.initial_extent = 256;
        config.atlas.maximum_extent = 4096;
        CY_REQUIRE(server.start_with(config, backend).has_value());
    }
    ~Runtime() {
        server.stop();
        backend.stop();
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
};

[[nodiscard]] ImportOptions grayscale_latin() {
    ImportOptions options;
    const OptionsSchema schema = font_options();
    CY_REQUIRE(options.set(schema, "mode", OptionValue::of_enumeration("grayscale")).has_value());
    CY_REQUIRE(options.set(schema, "size-pixels", OptionValue::of_float(20.0)).has_value());
    CY_REQUIRE(options.set(schema, "prerender", OptionValue::of_text("latin")).has_value());
    return options;
}

}  // namespace

CY_TEST_CASE("font: a cooked Latin range lays Latin out with no runtime rasterisation") {
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    const ImportOptions options = grayscale_latin();
    const ImportResult result = import_font(font, &options);
    CY_REQUIRE_FALSE(result.has_errors());
    CY_REQUIRE(result.primary() != nullptr);
    CY_CHECK(result.primary()->kind == assets::AssetKind::Font);
    const Span<const u8> payload(result.primary()->payload.data(),
                                 result.primary()->payload.size());

    text::CookedFont cooked;
    CY_REQUIRE(cooked.parse(payload).has_value());
    CY_CHECK(cooked.desc().family == "NotoSans");
    CY_CHECK(cooked.covers('A'));
    CY_CHECK(cooked.covers(0x00E9));  // é
    CY_CHECK_FALSE(cooked.covers(0x0100));

    Runtime runtime;
    const auto face = runtime.server.create_face(cooked);
    CY_REQUIRE(face.has_value());
    // Every Latin codepoint's glyph is in the cooked atlas...
    for (text::Codepoint codepoint = 0x20; codepoint <= 0xFF; ++codepoint) {
        if (codepoint > 0x7E && codepoint < 0xA0) {
            continue;
        }
        const text::GlyphIndex glyph = runtime.server.glyph_for(face.value(), codepoint);
        const bool present =
            std::any_of(cooked.glyphs().begin(), cooked.glyphs().end(),
                        [glyph](const text::CookedGlyph& entry) { return entry.glyph == glyph; });
        CY_TEST_INFO("U+" << static_cast<u32>(codepoint));
        CY_CHECK(present);
    }
    // ...the ligature shaping substitutes for "ffi" is too, though no codepoint maps to it...
    CY_CHECK(std::any_of(cooked.glyphs().begin(), cooked.glyphs().end(),
                         [](const text::CookedGlyph& entry) { return entry.glyph == 220; }));
    const u64 preloaded = runtime.server.diagnostics().glyphs_preloaded;
    CY_CHECK_EQ(preloaded, static_cast<u64>(cooked.glyphs().size()));

    // ...and laying Latin out, ligatures and accents included, rasterises NOTHING.
    text::FallbackChain chain;
    CY_REQUIRE(chain.push(face.value()).has_value());
    text::TextParagraph paragraph;
    text::ParagraphOptions layout;
    layout.width = 300.0f;
    CY_REQUIRE(runtime.server
                   .layout_paragraph("The quick brown fox jumps over the lazy dog. Caf\xc3\xa9 "
                                     "office, \xc2\xbf\xc3\x91"
                                     "and\xc3\xba? 0123456789 \xc2\xa9",
                                     chain, layout, paragraph)
                   .has_value());
    for (usize line = 0; line < paragraph.line_count(); ++line) {
        for (const text::ShapedGlyph& glyph : paragraph.line(line).run().glyphs) {
            CY_REQUIRE(runtime.server.glyph_slot(glyph.face, glyph.glyph).has_value());
        }
    }
    runtime.server.end_frame();
    const text::TextDiagnostics diagnostics = runtime.server.diagnostics();
    CY_CHECK_EQ(diagnostics.glyphs_rasterised, 0U);
    CY_CHECK_EQ(diagnostics.rasterised_last_frame, 0U);
    CY_CHECK_EQ(diagnostics.rasterisation_spikes, 0U);
}

CY_TEST_CASE("font: text outside the cooked range is rasterised, and the frame says so") {
    // The control for the case above: the same font with only the capitals cooked. Lower case was
    // not pre-rendered, so the same kind of sentence rasterises, and the per-frame count shows it.
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    ImportOptions options = grayscale_latin();
    CY_REQUIRE(options.set(font_options(), "prerender", OptionValue::of_text("U+0041-U+005A"))
                   .has_value());
    const ImportResult result = import_font(font, &options);
    CY_REQUIRE_FALSE(result.has_errors());
    text::CookedFont cooked;
    CY_REQUIRE(cooked
                   .parse(Span<const u8>(result.primary()->payload.data(),
                                         result.primary()->payload.size()))
                   .has_value());
    Runtime runtime;
    const auto face = runtime.server.create_face(cooked);
    CY_REQUIRE(face.has_value());
    text::FallbackChain chain;
    CY_REQUIRE(chain.push(face.value()).has_value());
    text::TextLine line;
    CY_REQUIRE(runtime.server.layout_line("ABC quick", chain, line).has_value());
    for (const text::ShapedGlyph& glyph : line.run().glyphs) {
        CY_REQUIRE(runtime.server.glyph_slot(glyph.face, glyph.glyph).has_value());
    }
    runtime.server.end_frame();
    // "quick" and the space: six glyphs nobody cooked.
    CY_CHECK_EQ(runtime.server.diagnostics().rasterised_last_frame, 6U);
}

CY_TEST_CASE("font: the cook records the mode, the instance, the features and the fallbacks") {
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    ImportOptions options;
    const OptionsSchema schema = font_options();
    CY_REQUIRE(options.set(schema, "prerender", OptionValue::of_text("U+0030-0039")).has_value());
    CY_REQUIRE(options.set(schema, "size-pixels", OptionValue::of_float(24.0)).has_value());
    CY_REQUIRE(options.set(schema, "axes", OptionValue::of_text("wght=700")).has_value());
    CY_REQUIRE(options.set(schema, "features", OptionValue::of_text("liga=0, onum")).has_value());
    CY_REQUIRE(
        options.set(schema, "fallbacks", OptionValue::of_text("Noto Sans Arabic, Noto Sans Hebrew"))
            .has_value());
    CY_REQUIRE(options.set(schema, "family", OptionValue::of_text("Interface Bold")).has_value());
    const ImportResult result = import_font(font, &options);
    CY_REQUIRE_FALSE(result.has_errors());
    text::CookedFont cooked;
    CY_REQUIRE(cooked
                   .parse(Span<const u8>(result.primary()->payload.data(),
                                         result.primary()->payload.size()))
                   .has_value());
    const text::FontDesc& desc = cooked.desc();
    CY_CHECK(desc.family == "Interface Bold");
    CY_CHECK(desc.mode == text::RenderMode::SignedDistanceField);
    CY_CHECK_EQ(desc.size_pixels, 24.0f);
    CY_REQUIRE_EQ(desc.axis_count, 1U);
    CY_CHECK(std::string_view(desc.axes[0].tag, 4) == "wght");
    CY_CHECK_EQ(desc.axes[0].value, 700.0f);
    CY_REQUIRE_EQ(desc.feature_count, 2U);
    CY_CHECK(std::string_view(desc.features[0].tag, 4) == "liga");
    CY_CHECK_EQ(desc.features[0].value, 0U);
    CY_CHECK(std::string_view(desc.features[1].tag, 4) == "onum");
    CY_CHECK_EQ(desc.features[1].value, 1U);
    CY_REQUIRE_EQ(cooked.fallbacks().size(), 2U);
    CY_CHECK(cooked.fallbacks()[1] == "Noto Sans Hebrew");
    // A distance field: every glyph on the distance-field page, and no coverage page at all.
    CY_CHECK_GE(cooked.glyphs().size(), 11U);
    for (const text::CookedGlyph& glyph : cooked.glyphs()) {
        CY_CHECK(glyph.format == text::PixelFormat::DistanceField);
    }
    CY_CHECK_GT(cooked.page(text::PixelFormat::DistanceField).extent, 0U);
    CY_CHECK_EQ(cooked.page(text::PixelFormat::Coverage).extent, 0U);

    // The feature defaults reach the closure: digits shaped with `onum` on are the OLDSTYLE forms,
    // which no codepoint maps to, and they were cooked — so drawing digits rasterises nothing.
    Runtime runtime;
    const auto bold = runtime.server.create_face(cooked);
    CY_REQUIRE(bold.has_value());
    text::FallbackChain digits_chain;
    CY_REQUIRE(digits_chain.push(bold.value()).has_value());
    text::ShapedRun digits;
    CY_REQUIRE(
        runtime.server.shape("0123456789", digits_chain, text::Direction::LeftToRight, digits)
            .has_value());
    for (const text::ShapedGlyph& glyph : digits.glyphs) {
        CY_CHECK_NE(glyph.glyph,
                    runtime.server.glyph_for(bold.value(), '0' + (glyph.source_offset)));
        CY_REQUIRE(runtime.server.glyph_slot(glyph.face, glyph.glyph).has_value());
    }
    CY_CHECK_EQ(runtime.server.diagnostics().glyphs_rasterised, 0U);

    // The instance is the bold one: its capitals are wider than the regular's.
    text::FontDesc regular = desc;
    regular.axis_count = 0;
    const auto light = runtime.server.create_face(regular, cooked.source());
    CY_REQUIRE(light.has_value());
    text::FallbackChain bold_chain;
    text::FallbackChain light_chain;
    CY_REQUIRE(bold_chain.push(bold.value()).has_value());
    CY_REQUIRE(light_chain.push(light.value()).has_value());
    CY_CHECK_GT(runtime.server.measure("HOME", bold_chain).value().x,
                runtime.server.measure("HOME", light_chain).value().x);
}

CY_TEST_CASE("font: a range the font lacks is a warning naming how many codepoints") {
    const Array<u8> font = read_font("NotoSansHebrew-Subset.ttf");
    ImportOptions options = grayscale_latin();
    const ImportResult result = import_font(font, &options, "fonts/Hebrew.ttf");
    CY_CHECK_FALSE(result.has_errors());
    CY_CHECK(reported(result, "font-range-missing"));
}

CY_TEST_CASE("font: WOFF2 is refused with the reason, and the registry routes fonts here") {
    Array<u8> woff2;
    const u8 header[] = {'w', 'O', 'F', '2', 0, 1, 0, 0};
    CY_REQUIRE(woff2.append(Span<const u8>(header, sizeof(header))).has_value());
    const ImportResult result = import_font(woff2, nullptr, "fonts/Web.woff2");
    CY_CHECK(result.has_errors());
    CY_CHECK(reported(result, "font-cook"));

    ImporterRegistry registry;
    CY_REQUIRE(register_builtin_importers(registry).has_value());
    for (const std::string_view path : {"a.ttf", "b.otf", "c.ttc", "d.woff", "e.woff2"}) {
        const Importer* importer =
            registry.find_for_source(assets::VirtualPath::normalise(path).value());
        CY_REQUIRE(importer != nullptr);
        CY_CHECK(importer->info().name == "font");
    }
}

#else

CY_TEST_CASE("font: without the complete text backend a font is refused naming the option") {
    CY_CHECK_FALSE(font_cooking_available());
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    const ImportResult result = import_font(font, nullptr);
    CY_CHECK(result.has_errors());
    CY_CHECK(reported(result, "font-backend-missing"));
    CY_CHECK(result.assets().empty());
}

#endif

CY_TEST_CASE("font: codepoint ranges, axes and features parse, and malformed ones are refused") {
    Array<text::CodepointRange> ranges;
    CY_REQUIRE(parse_codepoint_ranges("ascii, U+0400-04FF, u+20ac", ranges).has_value());
    CY_REQUIRE_EQ(ranges.size(), 3U);
    CY_CHECK_EQ(ranges[1].first, 0x400U);
    CY_CHECK_EQ(ranges[1].last, 0x4FFU);
    CY_CHECK_EQ(ranges[2].first, 0x20ACU);
    CY_CHECK_EQ(ranges[2].last, 0x20ACU);
    CY_CHECK_FALSE(parse_codepoint_ranges("U+0050-0040", ranges).has_value());
    CY_CHECK_FALSE(parse_codepoint_ranges("cyrillic", ranges).has_value());

    text::FontDesc desc;
    CY_REQUIRE(parse_axes("wght=650.5,wdth=90", desc).has_value());
    CY_CHECK_EQ(desc.axis_count, 2U);
    CY_CHECK_EQ(desc.axes[0].value, 650.5f);
    CY_CHECK_FALSE(parse_axes("weight=700", desc).has_value());
    CY_CHECK_FALSE(parse_axes("wght=", desc).has_value());
    CY_REQUIRE(parse_features("kern=0,ss01", desc).has_value());
    CY_CHECK_EQ(desc.feature_count, 2U);
    CY_CHECK_FALSE(parse_features("liga=-1", desc).has_value());
}
