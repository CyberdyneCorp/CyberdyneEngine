// Arabic joining, run itemisation and the shaping cache. M8.b task 9.4.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/values/name.h>
#include <cy/test/test.h>
#include <cy/text/shaping.h>

#include <string>
#include <string_view>

using namespace cy;
using namespace cy::text;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

/// Arabic "سلام" — seen, lam, alef, meem. Written as escapes so the file's encoding cannot change
/// what is tested.
constexpr std::string_view kSalam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";
/// "لا" — lam followed by alef, the mandatory ligature.
constexpr std::string_view kLamAlef = "\xD9\x84\xD8\xA7";
/// Hebrew "שלום".
constexpr std::string_view kShalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";

/// A coverage that says the first face has Latin and the second everything else — the shape a real
/// fallback chain has.
class TwoFaces final : public FaceCoverage {
public:
    [[nodiscard]] u32 face_for(Codepoint codepoint) const noexcept override {
        return (codepoint < 0x0250) ? 0U : 1U;
    }
};

}  // namespace

CY_TEST_CASE("text_shape: Arabic letters take their contextual forms") {
    // "WHEN Arabic text is shaped THEN contextual initial, medial, final, and isolated forms SHALL
    // be selected correctly."
    Array<JoinedGlyph> glyphs(allocator());
    CY_REQUIRE(join_arabic(kSalam, 0, static_cast<u32>(kSalam.size()), glyphs).has_value());

    // Seen, then the lam-alef ligature, then meem: three glyphs from four characters.
    CY_REQUIRE_EQ(glyphs.size(), 3U);
    CY_CHECK_EQ(glyphs[0].source, 0x0633U);             // seen
    CY_CHECK_EQ(glyphs[0].form, JoiningForm::Initial);  // it joins the lam that follows
    CY_CHECK_EQ(glyphs[0].presentation, 0xFEB3U);       // seen initial
    CY_CHECK(glyphs[1].ligature);
    CY_CHECK_EQ(glyphs[1].presentation, 0xFEFCU);  // lam-alef, final form
    CY_CHECK_EQ(glyphs[2].source, 0x0645U);        // meem
    // ISOLATED, and this is the case a shaper gets wrong: an alef joins only to the letter BEFORE
    // it, so it does not connect forward to the meem. A meem drawn in its final form here would be
    // attached to a letter that cannot reach it.
    CY_CHECK_EQ(glyphs[2].form, JoiningForm::Isolated);
    CY_CHECK_EQ(glyphs[2].presentation, 0xFEE1U);

    // Every glyph carries the byte offset it came from, which is what hit-testing and carets need.
    CY_CHECK_EQ(glyphs[0].source_offset, 0U);
    CY_CHECK_EQ(glyphs[1].source_offset, 2U);
    CY_CHECK_EQ(glyphs[2].source_offset, 6U);
}

CY_TEST_CASE("text_shape: a lone letter is isolated and a dual-joining pair is initial and final") {
    Array<JoinedGlyph> alone(allocator());
    const std::string_view beh = "\xD8\xA8";  // a single beh
    CY_REQUIRE(join_arabic(beh, 0, static_cast<u32>(beh.size()), alone).has_value());
    CY_REQUIRE_EQ(alone.size(), 1U);
    CY_CHECK_EQ(alone[0].form, JoiningForm::Isolated);
    CY_CHECK_EQ(alone[0].presentation, 0xFE8FU);

    Array<JoinedGlyph> pair(allocator());
    const std::string_view beh_beh = "\xD8\xA8\xD8\xA8";
    CY_REQUIRE(join_arabic(beh_beh, 0, static_cast<u32>(beh_beh.size()), pair).has_value());
    CY_REQUIRE_EQ(pair.size(), 2U);
    CY_CHECK_EQ(pair[0].form, JoiningForm::Initial);
    CY_CHECK_EQ(pair[1].form, JoiningForm::Final);

    // THREE of them makes the middle one medial, which is the form an implementation that only
    // looks one way gets wrong.
    Array<JoinedGlyph> triple(allocator());
    const std::string_view three = "\xD8\xA8\xD8\xA8\xD8\xA8";
    CY_REQUIRE(join_arabic(three, 0, static_cast<u32>(three.size()), triple).has_value());
    CY_REQUIRE_EQ(triple.size(), 3U);
    CY_CHECK_EQ(triple[1].form, JoiningForm::Medial);
    CY_CHECK_EQ(triple[1].presentation, 0xFE92U);
}

CY_TEST_CASE("text_shape: a mark is transparent and does not break a join") {
    // The rule an implementation usually forgets: a vowelled word still joins across its marks.
    // beh + fatha + beh — the fatha must not turn the first beh into an isolated form.
    const std::string_view vowelled = "\xD8\xA8\xD9\x8E\xD8\xA8";
    Array<JoinedGlyph> glyphs(allocator());
    CY_REQUIRE(join_arabic(vowelled, 0, static_cast<u32>(vowelled.size()), glyphs).has_value());
    CY_REQUIRE_EQ(glyphs.size(), 3U);
    CY_CHECK_EQ(glyphs[0].form, JoiningForm::Initial);
    CY_CHECK_EQ(glyphs[1].presentation, 0x064EU);  // the mark passes through unchanged
    CY_CHECK_EQ(glyphs[2].form, JoiningForm::Final);
    CY_CHECK_EQ(joining_type_of(0x064EU), JoiningType::Transparent);
}

CY_TEST_CASE("text_shape: a right-joining letter never takes an initial form") {
    // Alef, dal, ra and waw join only to the letter before them. A shaper that gave alef an initial
    // form would produce a word no reader recognises.
    const std::string_view alef_beh = "\xD8\xA7\xD8\xA8";  // alef then beh
    Array<JoinedGlyph> glyphs(allocator());
    CY_REQUIRE(join_arabic(alef_beh, 0, static_cast<u32>(alef_beh.size()), glyphs).has_value());
    CY_REQUIRE_EQ(glyphs.size(), 2U);
    CY_CHECK_EQ(glyphs[0].form, JoiningForm::Isolated);
    CY_CHECK_EQ(glyphs[1].form, JoiningForm::Isolated);
    CY_CHECK_EQ(joining_type_of(0x0627U), JoiningType::Right);
}

CY_TEST_CASE("text_shape: non-Arabic passes through untouched") {
    Array<JoinedGlyph> glyphs(allocator());
    CY_REQUIRE(join_arabic("ab", 0, 2, glyphs).has_value());
    CY_REQUIRE_EQ(glyphs.size(), 2U);
    CY_CHECK_EQ(glyphs[0].presentation, glyphs[0].source);
    CY_CHECK_EQ(glyphs[1].presentation, U'b');

    // A range outside the text is refused rather than read.
    const Status refused = join_arabic("ab", 0, 9, glyphs);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::OutOfRange);
}

CY_TEST_CASE("text_itemise: runs split by script, direction and face") {
    // "Runs SHALL be segmented by script, direction, and font before shaping."
    std::string text = "ab ";
    text += kShalom;
    text += " cd";

    BidiResult levels(allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::Auto, levels).has_value());
    TwoFaces coverage;
    Array<TextRun> runs(allocator());
    CY_REQUIRE(itemise(text, levels.runs.span(), &coverage, runs).has_value());

    CY_REQUIRE(runs.size() > 2U);
    CY_CHECK_EQ(runs[0].script, Script::Latin);
    CY_CHECK_EQ(runs[0].face, 0U);
    bool saw_hebrew = false;
    for (const TextRun& run : runs.span()) {
        if (run.script == Script::Hebrew) {
            saw_hebrew = true;
            CY_CHECK(run.right_to_left());
            CY_CHECK_EQ(run.face, 1U);  // the fallback face, because face 0 has no Hebrew
        }
    }
    CY_CHECK(saw_hebrew);
}

CY_TEST_CASE("text_itemise: a space does not split a run") {
    // Common characters join whatever they are beside — otherwise every space is a batch boundary
    // and a paragraph of Latin text becomes fifty draw calls.
    Array<TextRun> runs(allocator());
    CY_REQUIRE(itemise("hello world", Span<const BidiRun>{}, nullptr, runs).has_value());
    CY_CHECK_EQ(runs.size(), 1U);
    CY_CHECK_EQ(runs[0].begin, 0U);
    CY_CHECK_EQ(runs[0].end, 11U);
}

CY_TEST_CASE("text_cache: the same string with the same parameters is shaped once") {
    // "WHEN the same string is laid out repeatedly with unchanged parameters THEN the shaped result
    // SHALL be reused rather than reshaped."
    ShapeCache cache(allocator(), 4);
    ShapeKey key;
    key.content = hash_content("hello");
    key.face = 1;
    key.size_sixteenths = 16 * 16;
    key.language = Name::intern("en").index();

    u32 value = 0;
    CY_CHECK_FALSE(cache.find(key, value));
    CY_REQUIRE(cache.insert(key, 42).has_value());
    CY_CHECK(cache.find(key, value));
    CY_CHECK_EQ(value, 42U);
    CY_CHECK_EQ(cache.hits(), 1U);
    CY_CHECK_EQ(cache.misses(), 1U);
    CY_CHECK_NEAR(cache.hit_rate(), 0.5F, 1e-5F);

    // A DIFFERENT SIZE IS A DIFFERENT RESULT. A cache keyed on content alone returns glyphs shaped
    // at the wrong size, which is the bug this key exists to prevent.
    ShapeKey bigger = key;
    bigger.size_sixteenths = 32 * 16;
    CY_CHECK_FALSE(cache.find(bigger, value));
}

CY_TEST_CASE("text_cache: a locale change invalidates the runs that depend on language") {
    // "WHEN the locale changes at runtime THEN cached shaped runs SHALL be invalidated where
    // language-dependent."
    ShapeCache cache(allocator(), 8);
    const u32 english = Name::intern("en").index();
    const u32 turkish = Name::intern("tr").index();

    ShapeKey a;
    a.content = hash_content("fi");
    a.language = english;
    ShapeKey b = a;
    b.language = turkish;
    CY_REQUIRE(cache.insert(a, 1).has_value());
    CY_REQUIRE(cache.insert(b, 2).has_value());
    CY_CHECK_EQ(cache.size(), 2U);

    cache.invalidate_language(turkish);
    CY_CHECK_EQ(cache.size(), 1U);
    u32 value = 0;
    CY_CHECK(cache.find(a, value));
    CY_CHECK_EQ(value, 1U);
    CY_CHECK_FALSE(cache.find(b, value));
}

CY_TEST_CASE("text_cache: a full cache evicts the least recently used entry") {
    ShapeCache cache(allocator(), 2);
    ShapeKey first;
    first.content = 1;
    ShapeKey second;
    second.content = 2;
    ShapeKey third;
    third.content = 3;

    CY_REQUIRE(cache.insert(first, 1).has_value());
    CY_REQUIRE(cache.insert(second, 2).has_value());
    u32 value = 0;
    CY_CHECK(cache.find(first, value));  // first is now the more recently used
    CY_REQUIRE(cache.insert(third, 3).has_value());
    CY_CHECK_EQ(cache.evictions(), 1U);
    CY_CHECK(cache.find(first, value));
    CY_CHECK_FALSE(cache.find(second, value));  // the least recently used went
}

CY_TEST_CASE("text_shape: the capabilities say what is missing without HarfBuzz") {
    // `text-and-fonts`: "code needs to know whether bidirectional layout is available THEN it SHALL
    // query the capability rather than testing which backend is active". The same rule one level
    // up: this module says what it can do, and the two `false`s are the honest half.
    const ShapingCapabilities capabilities = shaping_capabilities();
    CY_CHECK(capabilities.arabic_joining);
    CY_CHECK(capabilities.itemisation);
    CY_CHECK(capabilities.shaping_cache);
    CY_CHECK_FALSE(capabilities.glyph_substitution);
    CY_CHECK_FALSE(capabilities.glyph_positioning);
    CY_CHECK_FALSE(capabilities.vertical);
}
