// The text server: capabilities, faces, fallback, shaping, its cache, and paragraph layout.
// M5 task 5.3.

#include "grid_font.h"

#include <cy/servers/text/server.h>
#include <cy/test/test.h>

#include <string_view>

using namespace cy::text;
using cy::f32;
using cy::u32;
using cy::usize;

namespace {

TextServerConfig small_config() {
    TextServerConfig config;
    config.atlas.initial_extent = 128;
    config.atlas.maximum_extent = 512;
    return config;
}

FallbackChain chain_of(FontHandle first, FontHandle second = {}) {
    FallbackChain chain;
    CY_REQUIRE(chain.push(first).has_value());
    if (!second.is_null()) {
        CY_REQUIRE(chain.push(second).has_value());
    }
    return chain;
}

}  // namespace

CY_TEST_CASE("server: the minimal backend says what it cannot do") {
    // `text-and-fonts`: "WHEN code needs to know whether bidirectional layout is available THEN it
    // SHALL query the capability rather than testing which backend is active." Every one of these
    // is false, and that is the honest state at M5 rather than a placeholder.
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    const TextCapabilities& capabilities = server.capabilities();
    CY_CHECK(std::string_view(capabilities.backend) == "minimal");
    CY_CHECK(!capabilities.complex_shaping);
    CY_CHECK(!capabilities.bidirectional);
    CY_CHECK(!capabilities.dictionary_line_breaking);
    CY_CHECK(!capabilities.colour_glyphs);
    CY_CHECK(!capabilities.variable_fonts);
    CY_CHECK(!capabilities.signed_distance_fields);
    CY_CHECK(!capabilities.kashida_justification);
}

CY_TEST_CASE("server: asking for the complete backend names what is missing") {
    // Named rather than silently downgraded: a caller that asked for shaping and got Latin has a
    // defect it cannot see.
    TextServer server;
    TextServerConfig config = small_config();
    config.backend = BackendKind::Complete;
    const auto refused = server.start(config);
    CY_REQUIRE(!refused.has_value());
    CY_CHECK(std::string_view(refused.error().message).find("HarfBuzz") != std::string_view::npos);
}

CY_TEST_CASE("server: a grid font whose numbers do not add up is refused at creation") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());

    test::GridFont source(8, 8, 16);
    ImageGridFont grid = source.font();
    grid.glyph_count = 4096;  // far more cells than the image holds
    CY_CHECK(!server.create_face(FontDesc{}, grid).has_value());

    grid = source.font();
    grid.ascent = static_cast<f32>(grid.cell_height) + 4.0f;  // a baseline outside the cell
    CY_CHECK(!server.create_face(FontDesc{}, grid).has_value());
}

CY_TEST_CASE("server: a face reports its metrics and answers for the codepoints it has") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);  // ASCII from space
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    auto metrics = server.face_metrics(face.value());
    CY_REQUIRE(metrics.has_value());
    CY_CHECK(metrics.value().ascent == 6.0f);
    CY_CHECK(metrics.value().descent == 2.0f);
    CY_CHECK(metrics.value().monospace);

    CY_CHECK(server.has_glyph(face.value(), U'A'));
    CY_CHECK(server.has_glyph(face.value(), U' '));
    CY_CHECK(!server.has_glyph(face.value(), U'é'));
    // Cell zero is a real glyph, so no glyph index collides with `.notdef`.
    CY_CHECK(server.glyph_for(face.value(), U' ') != kNotdef);
}

CY_TEST_CASE("server: a destroyed face's handle answers no") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 8, 16);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());
    CY_CHECK(server.is_face(face.value()));

    server.destroy_face(face.value());
    CY_CHECK(!server.is_face(face.value()));

    // The slot is reused and the generation moves, so the old handle stays wrong rather than
    // resolving to whatever took its place.
    auto second = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(second.has_value());
    CY_CHECK(second.value() != face.value());
    CY_CHECK(!server.is_face(face.value()));
    CY_CHECK(server.is_face(second.value()));
}

CY_TEST_CASE("server: a missing glyph falls back, and a missing everywhere becomes a box") {
    // `text-and-fonts`: "WHEN a codepoint is absent from the primary font THEN the fallback chain
    // SHALL be searched ... and finally a visible `.notdef` box SHALL be rendered rather than
    // nothing."
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());

    test::GridFont latin(8, 16, 26, U'a');
    test::GridFont digits(8, 16, 10, U'0');
    auto primary = server.create_face(FontDesc{}, latin.font());
    auto secondary = server.create_face(FontDesc{}, digits.font());
    CY_REQUIRE(primary.has_value());
    CY_REQUIRE(secondary.has_value());
    const FallbackChain chain = chain_of(primary.value(), secondary.value());

    ShapedRun run;
    CY_REQUIRE(server.shape("a1z", chain, Direction::LeftToRight, run).has_value());
    CY_REQUIRE(run.size() == 3);
    CY_CHECK(run.glyphs[0].face == primary.value());
    // The digit is not in the primary and is in the fallback.
    CY_CHECK(run.glyphs[1].face == secondary.value());
    CY_CHECK(!run.glyphs[1].missing);
    CY_CHECK(server.diagnostics().fallbacks_taken >= 1);

    ShapedRun missing;
    CY_REQUIRE(server.shape("\xc3\xa9", chain, Direction::LeftToRight, missing).has_value());
    CY_REQUIRE(missing.size() == 1);
    CY_CHECK(missing.glyphs[0].missing);
    // And the box really is rasterised, so a missing character is visible in a screenshot rather
    // than an absence somebody has to notice.
    CY_REQUIRE(server.glyph_slot(primary.value(), kNotdef).has_value());
    CY_CHECK(server.diagnostics().notdef_served >= 1);
}

CY_TEST_CASE("server: shaping records the byte offset every glyph came from") {
    // The field the other three layout requirements rest on: hit-testing returns a character index,
    // a caret sits between characters, and a selection is a range of the caller's own string.
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    ShapedRun run;
    CY_REQUIRE(
        server.shape("ab c", chain_of(face.value()), Direction::LeftToRight, run).has_value());
    CY_REQUIRE(run.size() == 4);
    CY_CHECK(run.glyphs[0].source_offset == 0);
    CY_CHECK(run.glyphs[3].source_offset == 3);
    CY_CHECK(run.width == 32.0f);
}

CY_TEST_CASE("server: a right-to-left request is refused rather than approximated") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    ShapedRun run;
    const auto refused = server.shape("abc", chain_of(face.value()), Direction::RightToLeft, run);
    CY_REQUIRE(!refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);
}

CY_TEST_CASE("server: the shaping cache serves the second identical request") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());
    const FallbackChain chain = chain_of(face.value());

    ShapedRun first;
    CY_REQUIRE(server.shape("hello", chain, Direction::LeftToRight, first).has_value());
    CY_CHECK(server.diagnostics().shaping_cache_misses == 1);
    CY_CHECK(server.diagnostics().shaping_cache_hits == 0);

    ShapedRun second;
    CY_REQUIRE(server.shape("hello", chain, Direction::LeftToRight, second).has_value());
    CY_CHECK(server.diagnostics().shaping_cache_hits == 1);
    CY_REQUIRE(second.size() == first.size());
    CY_CHECK(second.width == first.width);

    // Different text is a different entry, which is what "keyed by the run's content" means.
    ShapedRun other;
    CY_REQUIRE(server.shape("hellp", chain, Direction::LeftToRight, other).has_value());
    CY_CHECK(server.diagnostics().shaping_cache_misses == 2);
}

CY_TEST_CASE("server: a line measures, hit-tests and places a caret") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    TextLine line;
    CY_REQUIRE(server.layout_line("abcd", chain_of(face.value()), line).has_value());
    CY_CHECK(line.width() == 32.0f);
    CY_CHECK(line.height() == 8.0f);
    CY_CHECK(line.baseline() == 6.0f);
    CY_CHECK(line.hit_test(cy::Vec2{20.0f, 0.0f}).source_offset == 2);
    CY_CHECK(line.caret_at(2).position.x == 16.0f);

    auto measured = server.measure("abcd", chain_of(face.value()));
    CY_REQUIRE(measured.has_value());
    CY_CHECK(measured.value().x == 32.0f);
}

CY_TEST_CASE("server: a paragraph wraps at the width it was given") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    ParagraphOptions options;
    options.width = 40.0f;  // five glyphs
    options.overflow = Overflow::WordWrap;

    TextParagraph paragraph;
    CY_REQUIRE(server.layout_paragraph("aaa bbb ccc", chain_of(face.value()), options, paragraph)
                   .has_value());
    CY_CHECK(paragraph.line_count() >= 2);
    for (usize index = 0; index < paragraph.line_count(); ++index) {
        // Every line but one that cannot be broken fits the box.
        CY_CHECK(paragraph.line(index).width() <= options.width + 8.0f);
    }
    CY_CHECK(paragraph.size().y > paragraph.line(0).height());
}

CY_TEST_CASE("server: a word longer than the box breaks between characters when asked to") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    ParagraphOptions options;
    options.width = 24.0f;  // three glyphs
    options.overflow = Overflow::CharacterWrap;

    TextParagraph paragraph;
    CY_REQUIRE(server.layout_paragraph("abcdefghi", chain_of(face.value()), options, paragraph)
                   .has_value());
    // A word that has to go somewhere: clipping it would lose the rest of the paragraph with it.
    CY_CHECK(paragraph.line_count() >= 3);
    CY_CHECK(paragraph.line(0).width() <= 24.0f);
}

CY_TEST_CASE("server: alignment moves the line and does not reshape it") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    ParagraphOptions options;
    options.width = 80.0f;
    options.alignment = Alignment::Centre;

    TextParagraph centred;
    CY_REQUIRE(
        server.layout_paragraph("abcd", chain_of(face.value()), options, centred).has_value());
    CY_REQUIRE(centred.line_count() == 1);
    // (80 - 32) / 2
    CY_CHECK(centred.line_origin(0).x == 24.0f);

    options.alignment = Alignment::End;
    TextParagraph trailing;
    CY_REQUIRE(
        server.layout_paragraph("abcd", chain_of(face.value()), options, trailing).has_value());
    CY_CHECK(trailing.line_origin(0).x == 48.0f);
    // The glyphs themselves are identical: alignment is an origin, not a reshape.
    CY_CHECK(trailing.line(0).run().glyphs[0].offset.x == centred.line(0).run().glyphs[0].offset.x);
}

CY_TEST_CASE("server: a line limit truncates and says so") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    ParagraphOptions options;
    options.width = 24.0f;
    options.max_lines = 2;

    TextParagraph paragraph;
    CY_REQUIRE(server.layout_paragraph("aa bb cc dd ee", chain_of(face.value()), options, paragraph)
                   .has_value());
    CY_CHECK(paragraph.line_count() == 2);
    // A caller that wants a tooltip on truncated text asks this rather than measuring twice.
    CY_CHECK(paragraph.truncated());
}

CY_TEST_CASE("server: a paragraph hit-tests to a line and a character") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    ParagraphOptions options;
    options.width = 24.0f;
    TextParagraph paragraph;
    CY_REQUIRE(server.layout_paragraph("aa bb cc", chain_of(face.value()), options, paragraph)
                   .has_value());
    CY_REQUIRE(paragraph.line_count() >= 2);

    const HitResult second_line =
        paragraph.hit_test(cy::Vec2{4.0f, paragraph.line_origin(1).y + 2.0f});
    CY_CHECK(second_line.line == 1);
    CY_CHECK(second_line.source_offset >= paragraph.line(1).run().source_begin);

    // A click above the paragraph clamps to the start, which is what every text field does.
    const HitResult above = paragraph.hit_test(cy::Vec2{4.0f, -50.0f});
    CY_CHECK(above.clamped);
}

CY_TEST_CASE("server: an inline object takes its place in the flow") {
    // `text-and-fonts`: "WHEN an image is embedded in a paragraph with a baseline alignment THEN it
    // SHALL occupy its advance in layout and be positioned per the alignment."
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    InlineObject object;
    object.id = 17;
    object.source_offset = 2;
    object.size = cy::Vec2{12.0f, 12.0f};

    ParagraphOptions options;
    options.width = 200.0f;
    TextParagraph paragraph;
    CY_REQUIRE(server
                   .layout_paragraph_with_objects("ab cd", chain_of(face.value()), options,
                                                  cy::Span<const InlineObject>(&object, 1),
                                                  paragraph)
                   .has_value());
    CY_REQUIRE(paragraph.inline_objects().size() == 1);
    CY_CHECK(paragraph.inline_objects()[0].id == 17);
    CY_CHECK(paragraph.inline_objects()[0].position.x == 16.0f);
}

CY_TEST_CASE("server: inline objects out of source order are refused rather than sorted") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());

    InlineObject objects[2];
    objects[0].source_offset = 4;
    objects[1].source_offset = 1;

    ParagraphOptions options;
    options.width = 200.0f;
    TextParagraph paragraph;
    // The caller's order is the one its own model holds; silently reordering would make the
    // placements it gets back refer to something else.
    CY_CHECK(!server
                  .layout_paragraph_with_objects("abcdef", chain_of(face.value()), options,
                                                 cy::Span<const InlineObject>(objects, 2),
                                                 paragraph)
                  .has_value());
}

CY_TEST_CASE("server: a full fallback chain refuses another face rather than dropping it") {
    TextServer server;
    CY_REQUIRE(server.start(small_config()).has_value());
    test::GridFont source(8, 8, 16);

    FallbackChain chain;
    for (usize index = 0; index < FallbackChain::kMaxFallbacks; ++index) {
        auto face = server.create_face(FontDesc{}, source.font());
        CY_REQUIRE(face.has_value());
        CY_REQUIRE(chain.push(face.value()).has_value());
    }
    auto extra = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(extra.has_value());
    // A chain that silently stopped growing would search fewer faces than the caller asked for, and
    // the symptom — one script rendering as boxes — would be blamed on the font.
    CY_CHECK(!chain.push(extra.value()).has_value());
    CY_CHECK(!chain.push(FontHandle{}).has_value());
}

CY_TEST_CASE("server: the shaping cache evicts the least recently used, not the first slot") {
    // The defect this is here for: using the HIT count as the cache's clock leaves every entry at
    // zero in a workload that only ever misses, and the eviction then always picks slot zero —
    // which is the entry most recently added.
    TextServer server;
    TextServerConfig config = small_config();
    config.shaping_cache_entries = 2;
    CY_REQUIRE(server.start(config).has_value());
    test::GridFont source(8, 16, 95);
    auto face = server.create_face(FontDesc{}, source.font());
    CY_REQUIRE(face.has_value());
    const FallbackChain chain = chain_of(face.value());

    ShapedRun run;
    CY_REQUIRE(server.shape("aaa", chain, Direction::LeftToRight, run).has_value());
    CY_REQUIRE(server.shape("bbb", chain, Direction::LeftToRight, run).has_value());
    // Touch the first, so the second is the least recently used.
    CY_REQUIRE(server.shape("aaa", chain, Direction::LeftToRight, run).has_value());
    CY_CHECK(server.diagnostics().shaping_cache_hits == 1);

    CY_REQUIRE(server.shape("ccc", chain, Direction::LeftToRight, run).has_value());
    CY_REQUIRE(server.shape("aaa", chain, Direction::LeftToRight, run).has_value());
    CY_CHECK(server.diagnostics().shaping_cache_hits == 2);
}
