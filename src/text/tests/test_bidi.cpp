// The bidirectional algorithm, and the structured-text hint. M8.b task 9.4.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/text/bidi.h>

#include <string>
#include <string_view>

using namespace cy;
using namespace cy::text;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

// Hebrew "שלום" and Arabic "سلام", as UTF-8 literals. Written as escapes rather than as source
// characters so that the file's own encoding cannot change what is being tested.
constexpr std::string_view kShalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
constexpr std::string_view kSalam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";

/// The level covering a byte offset, or 0xFF when nothing does.
[[nodiscard]] u8 level_at(const BidiResult& result, u32 offset) noexcept {
    for (const BidiRun& run : result.runs.span()) {
        if (offset >= run.begin && offset < run.end) {
            return run.level;
        }
    }
    return 0xFFU;
}

}  // namespace

CY_TEST_CASE("text_bidi: the paragraph level comes from the first strong character") {
    // P2 and P3.
    BidiResult latin(allocator());
    CY_REQUIRE(resolve_levels("hello", ParagraphDirection::Auto, latin).has_value());
    CY_CHECK_EQ(latin.paragraph_level, 0U);

    BidiResult hebrew(allocator());
    CY_REQUIRE(resolve_levels(kShalom, ParagraphDirection::Auto, hebrew).has_value());
    CY_CHECK_EQ(hebrew.paragraph_level, 1U);

    // Digits and punctuation are not strong: a paragraph that opens with them takes its direction
    // from the first letter, wherever it is.
    BidiResult digits(allocator());
    std::string leading = "123 ";
    leading += kShalom;
    CY_REQUIRE(resolve_levels(leading, ParagraphDirection::Auto, digits).has_value());
    CY_CHECK_EQ(digits.paragraph_level, 1U);

    // And an explicit direction wins over both.
    BidiResult forced(allocator());
    CY_REQUIRE(resolve_levels("hello", ParagraphDirection::RightToLeft, forced).has_value());
    CY_CHECK_EQ(forced.paragraph_level, 1U);
}

CY_TEST_CASE("text_bidi: a mixed paragraph resolves per-run direction") {
    // "WHEN a paragraph mixes Hebrew and English THEN runs SHALL be reordered visually with correct
    // per-run direction."
    std::string text = "abc ";
    text += kShalom;
    text += " def";

    BidiResult result(allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::Auto, result).has_value());
    CY_CHECK_EQ(result.paragraph_level, 0U);
    CY_CHECK_EQ(level_at(result, 0), 0U);                                  // "abc"
    CY_CHECK_EQ(level_at(result, 4), 1U);                                  // Hebrew
    CY_CHECK_EQ(level_at(result, static_cast<u32>(text.size() - 1)), 0U);  // "def"

    // L2 puts the Hebrew run's bytes in the middle still, but reversed relative to its neighbours.
    Array<u32> order(allocator());
    CY_REQUIRE(reorder_visual(result.runs.span(), result.paragraph_level, order).has_value());
    CY_CHECK_EQ(order.size(), result.runs.size());
    // The first visual run is the left-to-right one, because the paragraph is left-to-right.
    CY_CHECK_EQ(result.runs[order[0]].begin, 0U);
}

CY_TEST_CASE("text_bidi: numbers beside Arabic take the levels UAX #9 gives them") {
    // W2 and I1: a European number after an Arabic letter becomes an Arabic number, and an Arabic
    // number in a right-to-left run goes up TWO levels rather than one — which is what puts digits
    // left-to-right inside right-to-left text.
    std::string text(kSalam);
    text += " 42";

    BidiResult result(allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::Auto, result).has_value());
    CY_CHECK_EQ(result.paragraph_level, 1U);
    CY_CHECK_EQ(level_at(result, 0), 1U);
    CY_CHECK_EQ(level_at(result, static_cast<u32>(text.size() - 1)), 2U);
}

CY_TEST_CASE("text_bidi: an override forces a direction and a pop restores it") {
    // X6 with an override in effect: every character inside takes the override's direction whatever
    // its own class says.
    std::string text = "a\xE2\x80\xAE";  // U+202E RIGHT-TO-LEFT OVERRIDE
    text += "bc";
    text += "\xE2\x80\xAC";  // U+202C POP DIRECTIONAL FORMATTING
    text += "d";

    BidiResult result(allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::LeftToRight, result).has_value());
    CY_CHECK_EQ(level_at(result, 0), 0U);  // 'a'
    CY_CHECK_EQ(level_at(result, 4), 1U);  // 'b', inside the override
    CY_CHECK_EQ(level_at(result, static_cast<u32>(text.size() - 1)), 0U);  // 'd', after the pop
}

CY_TEST_CASE("text_bidi: an isolate is reported as approximated rather than silently different") {
    // The one place this implementation and ICU can disagree — X10's isolating run sequences — is
    // REPORTED. A caller that cares can refuse; a caller that does not gets levels that are right
    // for the isolate's own content, which is the common case.
    std::string text = "a\xE2\x81\xA6";  // U+2066 LEFT-TO-RIGHT ISOLATE
    text += kShalom;
    text += "\xE2\x81\xA9";  // U+2069 POP DIRECTIONAL ISOLATE
    text += "b";

    BidiResult result(allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::LeftToRight, result).has_value());
    CY_CHECK(result.approximated);
    CY_CHECK_EQ(level_at(result, 0), 0U);
    // The Hebrew inside the isolate is still right-to-left — at level 3 rather than 1, because the
    // left-to-right isolate raised the embedding to 2 and I1 took the Hebrew one above it. The
    // LEVEL is what reordering reads; its parity is what direction means.
    CY_CHECK_EQ(level_at(result, 4) & 1U, 1U);
    CY_CHECK_GT(level_at(result, 4), 1U);

    // And text with no isolate is NOT reported as approximated, so the flag means something.
    BidiResult plain(allocator());
    CY_REQUIRE(resolve_levels("abc", ParagraphDirection::Auto, plain).has_value());
    CY_CHECK_FALSE(plain.approximated);
}

CY_TEST_CASE("text_bidi: trailing whitespace takes the paragraph level") {
    // L1. Without it, the trailing space of a right-to-left line lands on the wrong end and the
    // caret sits in mid-air.
    std::string text(kShalom);
    text += "   ";
    BidiResult result(allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::LeftToRight, result).has_value());
    CY_CHECK_EQ(level_at(result, 0), 1U);
    CY_CHECK_EQ(level_at(result, static_cast<u32>(text.size() - 1)), 0U);
}

CY_TEST_CASE("text_bidi: a file path stays readable in a right-to-left context") {
    // "WHEN a path is displayed with the file structured-text hint in an RTL locale THEN its
    // separators and components SHALL be ordered so the path remains readable."
    Array<char> wrapped(allocator());
    CY_REQUIRE(apply_structured_text("/home/user/save.cysave", StructuredText::FilePath, wrapped)
                   .has_value());
    const std::string_view text(wrapped.data(), wrapped.size());
    // Each component is isolated, so the algorithm cannot reorder them against the separators.
    CY_CHECK_GT(text.size(), std::string_view("/home/user/save.cysave").size());
    CY_CHECK_NE(text.find("\xE2\x81\xA6"), std::string_view::npos);

    BidiResult result(allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::RightToLeft, result).has_value());
    // In a right-to-left paragraph the components are even-levelled — left-to-right islands — which
    // is exactly what keeps `home` before `user` on screen.
    const usize component = text.find('h');
    CY_REQUIRE_NE(component, std::string_view::npos);
    CY_CHECK_EQ(level_at(result, static_cast<u32>(component)) & 1U, 0U);

    // `Code` wraps the whole string, and `None` changes nothing — the identity case a caller relies
    // on when it does not know what kind of string it has.
    Array<char> untouched(allocator());
    CY_REQUIRE(apply_structured_text("plain", StructuredText::None, untouched).has_value());
    CY_CHECK_EQ(std::string_view(untouched.data(), untouched.size()), "plain");
}

CY_TEST_CASE("text_bidi: empty text resolves to the requested direction without a run") {
    BidiResult result(allocator());
    CY_REQUIRE(resolve_levels("", ParagraphDirection::RightToLeft, result).has_value());
    CY_CHECK_EQ(result.runs.size(), 0U);
    CY_CHECK_EQ(result.paragraph_level, 1U);

    Array<u32> order(allocator());
    CY_REQUIRE(reorder_visual(result.runs.span(), result.paragraph_level, order).has_value());
    CY_CHECK_EQ(order.size(), 0U);
}
