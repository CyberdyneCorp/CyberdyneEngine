// Line breaking, justification, overflow and grapheme clusters. M8.b task 9.4.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/text/linebreak.h>

#include <algorithm>
#include <string_view>

using namespace cy;
using namespace cy::text;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

/// Thai: "ภาษาไทย" — two words, no space between them, which is the whole problem.
constexpr std::string_view kThai =
    "\xE0\xB8\xA0\xE0\xB8\xB2\xE0\xB8\xA9\xE0\xB8\xB2\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2";
constexpr std::string_view kThaiFirst = "\xE0\xB8\xA0\xE0\xB8\xB2\xE0\xB8\xA9\xE0\xB8\xB2";

[[nodiscard]] bool breaks_at(Span<const BreakOpportunity> breaks, u32 offset) noexcept {
    return std::ranges::any_of(breaks, [offset](const BreakOpportunity& opportunity) noexcept {
        return opportunity.offset == offset;
    });
}

/// A fixed advance per character, so the ellipsis cases are arithmetic rather than typography.
f32 fixed_advance(std::string_view, u32, void*) noexcept {
    return 10.0F;
}

}  // namespace

CY_TEST_CASE("text_break: a space is a break and a word is not broken inside") {
    Array<BreakOpportunity> breaks(allocator());
    BreakReport report;
    CY_REQUIRE(find_breaks("hello world", nullptr, breaks, report).has_value());
    CY_CHECK(breaks_at(breaks.span(), 6));        // after the space, "world" starts a line
    CY_CHECK_FALSE(breaks_at(breaks.span(), 2));  // not inside "hello"
    CY_CHECK_EQ(report.mandatory, 0U);
}

CY_TEST_CASE("text_break: a newline is mandatory and a hyphen allows a break") {
    Array<BreakOpportunity> breaks(allocator());
    BreakReport report;
    CY_REQUIRE(find_breaks("one\ntwo-three", nullptr, breaks, report).has_value());
    CY_CHECK_EQ(report.mandatory, 1U);
    CY_CHECK(breaks_at(breaks.span(), 4));
    // A hyphen breaks AFTER itself: "two-" then "three".
    CY_CHECK(breaks_at(breaks.span(), 8));
}

CY_TEST_CASE("text_break: no break before a closing bracket or after an opening one") {
    // LB13 and LB14 — the two rules whose absence produces the "(\nword)" that everybody notices.
    Array<BreakOpportunity> breaks(allocator());
    BreakReport report;
    CY_REQUIRE(find_breaks("a (b) c", nullptr, breaks, report).has_value());
    CY_CHECK_FALSE(breaks_at(breaks.span(), 3));  // between '(' and 'b'
    CY_CHECK_FALSE(breaks_at(breaks.span(), 4));  // between 'b' and ')'
    CY_CHECK(breaks_at(breaks.span(), 2));        // after "a "
}

CY_TEST_CASE("text_break: Thai needs a dictionary, and says which answer it gave") {
    // "WHEN Thai text with no spaces is wrapped THEN dictionary-based breaking SHALL find valid
    // break points." The dictionary is the project's; what this module owns is the deferral to it.
    Array<BreakOpportunity> without(allocator());
    BreakReport without_report;
    CY_REQUIRE(find_breaks(kThai, nullptr, without, without_report).has_value());
    CY_CHECK_FALSE(without_report.used_dictionary);
    // The fallback breaks between every pair — wrong for Thai, and better than a paragraph that
    // never wraps. The report is how a developer finds out which one they got.
    CY_CHECK_GT(without.size(), 2U);

    WordList words(allocator());
    CY_REQUIRE(words.add(kThaiFirst).has_value());
    Array<BreakOpportunity> with(allocator());
    BreakReport with_report;
    CY_REQUIRE(find_breaks(kThai, &words, with, with_report).has_value());
    CY_CHECK(with_report.used_dictionary);
    // Exactly one break, at the end of the word the dictionary knows.
    CY_CHECK_EQ(with.size(), 1U);
    CY_CHECK_EQ(with[0].offset, static_cast<u32>(kThaiFirst.size()));
    CY_CHECK(with[0].from_dictionary);
}

CY_TEST_CASE("text_break: an empty word is refused rather than matching everywhere") {
    WordList words(allocator());
    const Status added = words.add("");
    CY_REQUIRE_FALSE(added.has_value());
    CY_CHECK_EQ(added.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("text_justify: the priority order decides which lever is pulled") {
    // "Justification SHALL support: inter-word spacing, inter-character spacing where appropriate
    // to the script, kashida elongation for Arabic, and a configurable priority among them."
    const JustifyPoint points[4] = {
        JustifyPoint{0, JustifyPriority::InterWord, 0.0F},
        JustifyPoint{5, JustifyPriority::InterWord, 0.0F},
        JustifyPoint{7, JustifyPriority::Kashida, 6.0F},
        JustifyPoint{9, JustifyPriority::Kashida, 6.0F},
    };

    // Latin: spaces first. The kashida points get nothing.
    const JustifyPriority latin[2] = {JustifyPriority::InterWord, JustifyPriority::Kashida};
    JustifyResult spaces(allocator());
    CY_REQUIRE(justify(Span<const JustifyPoint>(points, 4), Span<const JustifyPriority>(latin, 2),
                       10.0F, spaces)
                   .has_value());
    CY_CHECK_NEAR(spaces.distribution[0], 5.0F, 1e-4F);
    CY_CHECK_EQ(spaces.distribution[2], 0.0F);
    CY_CHECK_EQ(spaces.residual, 0.0F);

    // Arabic: kashida first, and the limit is respected — a kashida stretched past a few times its
    // natural length is a defect a typographer will name.
    const JustifyPriority arabic[2] = {JustifyPriority::Kashida, JustifyPriority::InterWord};
    JustifyResult elongated(allocator());
    CY_REQUIRE(justify(Span<const JustifyPoint>(points, 4), Span<const JustifyPriority>(arabic, 2),
                       20.0F, elongated)
                   .has_value());
    CY_CHECK_NEAR(elongated.distribution[2], 6.0F, 1e-4F);
    CY_CHECK_NEAR(elongated.distribution[3], 6.0F, 1e-4F);
    // What the kashida could not take went to the spaces.
    CY_CHECK_GT(elongated.distribution[0], 0.0F);
    CY_CHECK_EQ(elongated.residual, 0.0F);
}

CY_TEST_CASE("text_justify: width that no lever can absorb is reported, not hidden") {
    const JustifyPoint points[1] = {JustifyPoint{3, JustifyPriority::Kashida, 2.0F}};
    const JustifyPriority order[1] = {JustifyPriority::Kashida};
    JustifyResult result(allocator());
    CY_REQUIRE(justify(Span<const JustifyPoint>(points, 1), Span<const JustifyPriority>(order, 1),
                       30.0F, result)
                   .has_value());
    CY_CHECK_NEAR(result.distribution[0], 2.0F, 1e-4F);
    // The caller shows a ragged edge rather than a line that pretends to be justified.
    CY_CHECK_NEAR(result.residual, 28.0F, 1e-4F);
}

CY_TEST_CASE("text_overflow: an end ellipsis replaces exactly the glyphs that do not fit") {
    // "WHEN text exceeds its width with end-ellipsis overflow THEN trailing glyphs SHALL be
    // replaced by an ellipsis that fits within the width."
    const std::string_view text = "abcdefghij";  // ten characters at ten pixels each
    const EllipsisPlan plan =
        plan_ellipsis(text, OverflowMode::EllipsisEnd, 55.0F, 10.0F, fixed_advance, nullptr);
    CY_CHECK(plan.fits);
    // Forty-five pixels of budget after the ellipsis: four characters kept.
    CY_CHECK_EQ(plan.remove_begin, 4U);
    CY_CHECK_EQ(plan.remove_end, 10U);

    const EllipsisPlan start =
        plan_ellipsis(text, OverflowMode::EllipsisStart, 55.0F, 10.0F, fixed_advance, nullptr);
    CY_CHECK(start.fits);
    CY_CHECK_EQ(start.remove_begin, 0U);
    CY_CHECK_EQ(start.remove_end, 6U);

    const EllipsisPlan middle =
        plan_ellipsis(text, OverflowMode::EllipsisMiddle, 55.0F, 10.0F, fixed_advance, nullptr);
    CY_CHECK(middle.fits);
    CY_CHECK_GT(middle.remove_begin, 0U);
    CY_CHECK_LT(middle.remove_end, 10U);
}

CY_TEST_CASE(
    "text_overflow: text that fits is left alone, and an ellipsis that does not fit gives up") {
    const std::string_view text = "abc";
    const EllipsisPlan fits =
        plan_ellipsis(text, OverflowMode::EllipsisEnd, 100.0F, 10.0F, fixed_advance, nullptr);
    CY_CHECK_FALSE(fits.fits);  // nothing to remove
    CY_CHECK_EQ(fits.remove_begin, fits.remove_end);

    // An ellipsis wider than the box is not an improvement on the text it replaced.
    const EllipsisPlan hopeless =
        plan_ellipsis(text, OverflowMode::EllipsisEnd, 5.0F, 10.0F, fixed_advance, nullptr);
    CY_CHECK_FALSE(hopeless.fits);
}

CY_TEST_CASE("text_overflow: shrink-to-fit is clamped so text stays readable") {
    CY_CHECK_EQ(shrink_to_fit(50.0F, 100.0F), 1.0F);
    CY_CHECK_NEAR(shrink_to_fit(100.0F, 80.0F), 0.8F, 1e-5F);
    // Ten times too wide would scale to a tenth, which is unreadable. The clamp is a policy, and
    // having it in one place is the point.
    CY_CHECK_EQ(shrink_to_fit(1000.0F, 100.0F), 0.5F);
    CY_CHECK_EQ(shrink_to_fit(1000.0F, 100.0F, 0.25F), 0.25F);
}

CY_TEST_CASE("text_grapheme: a caret moves by user-perceived character") {
    // A combining mark, and a CRLF: the two cases where a codepoint step puts a caret somewhere the
    // user did not ask for.
    const std::string_view accented = "e\xCC\x81x";  // e + U+0301 COMBINING ACUTE, then x
    CY_CHECK_EQ(next_grapheme(accented, 0), 3U);
    CY_CHECK_EQ(next_grapheme(accented, 3), 4U);
    CY_CHECK_EQ(previous_grapheme(accented, 3), 0U);

    const std::string_view crlf = "a\r\nb";
    CY_CHECK_EQ(next_grapheme(crlf, 1), 3U);

    // And a plain string steps one byte at a time.
    CY_CHECK_EQ(next_grapheme("abc", 1), 2U);
    CY_CHECK_EQ(next_grapheme("abc", 3), 3U);
}

CY_TEST_CASE("text_unicode: coverage is reported rather than assumed silently") {
    // The one property that separates "this module knows" from "this module guessed": Devanagari is
    // not in the tables, and the answer says so.
    CY_CHECK_EQ(coverage_of(U'a'), Coverage::Known);
    CY_CHECK_EQ(coverage_of(0x05D0), Coverage::Known);    // Hebrew alef
    CY_CHECK_EQ(coverage_of(0x0915), Coverage::Assumed);  // Devanagari ka
    CY_CHECK_EQ(script_of(0x0915), Script::Unknown);
    // And the assumed answer is the documented default rather than a random one.
    CY_CHECK_EQ(bidi_class_of(0x0915), BidiClass::LeftToRight);
}
