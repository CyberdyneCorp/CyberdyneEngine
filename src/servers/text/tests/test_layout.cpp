// UTF-8 decoding, break opportunities, and the line and paragraph queries. M5 task 5.3.

#include <cy/servers/text/layout.h>
#include <cy/test/test.h>

#include <string_view>

using namespace cy::text;
using cy::u32;
using cy::usize;

namespace {

usize count_breaks(std::string_view text) {
    cy::Array<BreakOpportunity> breaks;
    CY_REQUIRE(find_break_opportunities(text, breaks).has_value());
    return breaks.size();
}

}  // namespace

CY_TEST_CASE("utf8: the four lengths decode, and the offsets advance by the right amount") {
    std::string_view text = "aé\xe4\xb8\xad\xf0\x9f\x98\x80";  // 'a', U+00E9, U+4E2D, U+1F600
    usize cursor = 0;
    CY_CHECK(decode_utf8(text, cursor) == U'a');
    CY_CHECK(cursor == 1);
    CY_CHECK(decode_utf8(text, cursor) == 0x00E9);
    CY_CHECK(cursor == 3);
    CY_CHECK(decode_utf8(text, cursor) == 0x4E2D);
    CY_CHECK(cursor == 6);
    CY_CHECK(decode_utf8(text, cursor) == 0x1F600);
    CY_CHECK(cursor == 10);
    CY_CHECK(cursor == text.size());
}

CY_TEST_CASE("utf8: malformed input yields the replacement and always advances") {
    // Always advancing is the property that matters: a decoder that stood still on a bad byte turns
    // a corrupted string into an infinite loop, which is a hang rather than a wrong glyph.
    const char* cases[] = {
        "\x80",              // a continuation byte with no lead
        "\xC0\x80",          // an overlong encoding of NUL
        "\xE0\x80",          // a truncated three-byte sequence
        "\xED\xA0\x80",      // a surrogate, which UTF-8 may not encode
        "\xF5\x80\x80\x80",  // past U+10FFFF
    };
    for (const char* raw : cases) {
        std::string_view text(raw);
        usize cursor = 0;
        const Codepoint decoded = decode_utf8(text, cursor);
        CY_CHECK(decoded == 0xFFFD);
        CY_CHECK(cursor > 0);
    }
}

CY_TEST_CASE("breaks: after a space, after a hyphen, and required after a newline") {
    cy::Array<BreakOpportunity> breaks;
    CY_REQUIRE(find_break_opportunities("a b-c\nd", breaks).has_value());
    CY_REQUIRE(breaks.size() == 3);
    CY_CHECK(breaks[0].source_offset == 2);  // after the space
    CY_CHECK(!breaks[0].mandatory);
    CY_CHECK(breaks[1].source_offset == 4);  // after the hyphen
    CY_CHECK(breaks[2].source_offset == 6);  // after the newline
    CY_CHECK(breaks[2].mandatory);
}

CY_TEST_CASE("breaks: a script without spaces breaks between characters") {
    // Right for Chinese and Japanese; an approximation for Thai, which needs a dictionary. The
    // capability query says `dictionary_line_breaking` is false, which is how a caller learns this
    // without reading the source.
    CY_CHECK(count_breaks("\xe4\xb8\xad\xe6\x96\x87") == 1);
    // And Latin without spaces has no opportunities at all, which is what makes a long word
    // overflow rather than break in the middle of itself.
    CY_CHECK(count_breaks("abcdef") == 0);
}

CY_TEST_CASE("line: hit-testing returns a character and which side of it") {
    TextLine line;
    FontMetrics metrics;
    metrics.ascent = 8.0f;
    metrics.descent = 2.0f;
    line.set_metrics(metrics);

    for (u32 index = 0; index < 3; ++index) {
        ShapedGlyph glyph;
        glyph.advance = 10.0f;
        glyph.offset = cy::Vec2{static_cast<cy::f32>(index) * 10.0f, 0.0f};
        glyph.source_offset = index;
        CY_REQUIRE(line.run().glyphs.push_back(glyph).has_value());
    }
    line.run().width = 30.0f;
    line.run().source_end = 3;

    CY_CHECK(line.hit_test(cy::Vec2{2.0f, 0.0f}).source_offset == 0);
    CY_CHECK(!line.hit_test(cy::Vec2{2.0f, 0.0f}).trailing);
    // Past the middle of a glyph places the caret AFTER the character, which is the difference
    // between a text field that feels right and one that feels off by one.
    CY_CHECK(line.hit_test(cy::Vec2{8.0f, 0.0f}).trailing);
    CY_CHECK(line.hit_test(cy::Vec2{12.0f, 0.0f}).source_offset == 1);

    const HitResult before = line.hit_test(cy::Vec2{-5.0f, 0.0f});
    CY_CHECK(before.source_offset == 0);
    CY_CHECK(before.clamped);
    const HitResult after = line.hit_test(cy::Vec2{500.0f, 0.0f});
    CY_CHECK(after.source_offset == 3);
    CY_CHECK(after.clamped);
}

CY_TEST_CASE("line: a caret sits between characters and is as tall as the line") {
    TextLine line;
    FontMetrics metrics;
    metrics.ascent = 8.0f;
    metrics.descent = 2.0f;
    line.set_metrics(metrics);
    for (u32 index = 0; index < 3; ++index) {
        ShapedGlyph glyph;
        glyph.advance = 10.0f;
        glyph.source_offset = index;
        CY_REQUIRE(line.run().glyphs.push_back(glyph).has_value());
    }

    CY_CHECK(line.caret_at(0).position.x == 0.0f);
    CY_CHECK(line.caret_at(1).position.x == 10.0f);
    CY_CHECK(line.caret_at(3).position.x == 30.0f);
    CY_CHECK(line.caret_at(1).height == 10.0f);
    // Left-to-right only, so no direction boundary can arise and there is never a second rectangle.
    // The field exists so that a caller drawing both needs no change when one can.
    CY_CHECK(!line.caret_at(1).has_secondary);
}
