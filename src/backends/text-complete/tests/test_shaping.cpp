// SPDX-License-Identifier: MIT
// Shaping goldens for four scripts, and the comparisons with src/text/ where both answer.
//
// THE GOLDENS ARE NOT THIS BACKEND'S OWN OUTPUT. Each array below was produced by uharfbuzz 14.6.0
// — HarfBuzz's Python binding, at the version deps/manifest.toml pins — shaping the same string in
// the same font at 32 pixels (a scale of 2048), with clusters as UTF-8 byte offsets:
//
//     buf = hb.Buffer(); buf.add_utf8(text.encode()); buf.direction = d
//     buf.guess_segment_properties(); hb.shape(font, buf)
//
// So a case here fails when the engine's path to HarfBuzz — the blob, the scale, the direction, the
// cluster convention, the Y flip — differs from HarfBuzz's own, and a regenerated golden is a
// statement that HarfBuzz itself changed. Values are (glyph, cluster, x advance, x offset, y
// offset) in 26.6 fixed point, Y up, which is HarfBuzz's convention; the backend's Y-down pixels
// are converted back before comparing.

#include "fonts.h"

#include <cy/text/bidi.h>
#include <cy/text/shaping.h>

#include <cmath>
#include <string_view>

using namespace cy;
using namespace cy::text;
using namespace cy::text::test;

namespace {

struct Expected26 {
    u32 glyph;
    u32 cluster;
    i32 x_advance;
    i32 x_offset;
    i32 y_offset;
};

[[nodiscard]] i32 to_26_6(f32 pixels) {
    return static_cast<i32>(std::lround(pixels * 64.0f));
}

/// Shape `text` in `font` at 32 pixels through the backend and compare glyph by glyph.
void check_golden(const char* font, std::string_view text, Direction direction,
                  Span<const Expected26> golden) {
    CompleteTextBackend backend;
    CY_REQUIRE(backend.start().has_value());
    const Array<u8> bytes = read_font(font);
    const auto face = backend.open_face(desc_of(32.0f), source_of(bytes));
    CY_REQUIRE(face.has_value());
    ShapeRequest request;
    request.text = text;
    request.direction = direction;
    Array<BackendGlyph> shaped;
    CY_REQUIRE(backend.shape(face.value(), request, shaped).has_value());
    CY_REQUIRE_EQ(shaped.size(), golden.size());
    for (usize index = 0; index < golden.size(); ++index) {
        CY_TEST_INFO("glyph " << index << " of " << font);
        CY_CHECK_EQ(shaped[index].glyph, golden[index].glyph);
        CY_CHECK_EQ(shaped[index].cluster, golden[index].cluster);
        CY_CHECK_EQ(to_26_6(shaped[index].advance.x), golden[index].x_advance);
        CY_CHECK_EQ(to_26_6(shaped[index].offset.x), golden[index].x_offset);
        CY_CHECK_EQ(-to_26_6(shaped[index].offset.y), golden[index].y_offset);
    }
}

}  // namespace

CY_TEST_CASE("shaping: Latin kerns AV and ligates ffi, as HarfBuzz shapes it") {
    // "AVATAR office". The A before V advances 1227 where its own advance is 1309 — that is the
    // kerning — and "ffi" is one glyph, 220, whose cluster is the first f.
    constexpr Expected26 golden[] = {
        {34, 0, 1227, 0, 0},  {55, 1, 1147, 0, 0}, {34, 2, 1166, 0, 0},  {53, 3, 996, 0, 0},
        {34, 4, 1309, 0, 0},  {51, 5, 1274, 0, 0}, {1, 6, 532, 0, 0},    {80, 7, 1239, 0, 0},
        {220, 8, 1937, 0, 0}, {68, 11, 983, 0, 0}, {70, 12, 1155, 0, 0},
    };
    check_golden("NotoSans-Latin-VF.ttf", "AVATAR office", Direction::LeftToRight, golden);
}

CY_TEST_CASE("shaping: Arabic joins, ligates lam-alef and places its marks, in visual order") {
    // "سلام لا بِسْمِ": the run comes back right to left on the page, so the last character's
    // cluster (22) is first; the marks (glyphs 260, 240, 211) have zero advance and an offset; lam
    // and alef are one glyph (63).
    constexpr Expected26 golden[] = {
        {260, 22, 0, 285, 0}, {67, 22, 1151, 0, 0},    {240, 18, 0, 616, -258},
        {28, 18, 1741, 0, 0}, {260, 14, 0, -72, -391}, {211, 14, 0, 63, -6},
        {14, 14, 551, 0, 0},  {1, 13, 532, 0, 0},      {6, 11, 743, 0, 0},
        {63, 9, 449, 0, 0},   {1, 8, 532, 0, 0},       {64, 6, 991, 0, 0},
        {7, 4, 776, 0, 0},    {61, 2, 451, 0, 0},      {29, 0, 1606, 0, 0},
    };
    check_golden("NotoSansArabic-Subset.ttf",
                 "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 \xd9\x84\xd8\xa7 "
                 "\xd8\xa8\xd9\x90\xd8\xb3\xd9\x92\xd9\x85\xd9\x90",
                 Direction::RightToLeft, golden);
}

CY_TEST_CASE("shaping: Devanagari moves its vowel sign before the consonant and forms conjuncts") {
    // "हिन्दी क्षत्रिय": the i-matra (446) is drawn BEFORE ह (84) although it follows it in the
    // text — both in cluster 0 — and न्द and क्ष are each one conjunct glyph.
    constexpr Expected26 golden[] = {
        {446, 0, 530, 0, 0},   {84, 0, 1087, 0, 0},  {150, 6, 633, 0, 0},  {69, 12, 1087, 0, 0},
        {29, 12, 530, 0, 0},   {1, 18, 532, 0, 0},   {86, 19, 1468, 0, 0}, {447, 28, 530, 0, 0},
        {209, 28, 1130, 0, 0}, {77, 40, 1188, 0, 0},
    };
    check_golden("NotoSansDevanagari-Subset.ttf",
                 "\xe0\xa4\xb9\xe0\xa4\xbf\xe0\xa4\xa8\xe0\xa5\x8d\xe0\xa4\xa6\xe0\xa5\x80 "
                 "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7\xe0\xa4\xa4\xe0\xa5\x8d\xe0\xa4\xb0\xe0\xa4"
                 "\xbf\xe0\xa4\xaf",
                 Direction::LeftToRight, golden);
}

CY_TEST_CASE("shaping: Thai decomposes sara am and moves the tone mark clear of it") {
    // "ภาษาไทย น้ำ": in น้ำ, sara am becomes nikhahit (54) and sara aa (73), and the tone mark
    // (45) shifts left by 59 to sit over the nikhahit.
    constexpr Expected26 golden[] = {
        {66, 0, 1235, 0, 0},  {73, 3, 831, 0, 0},    {94, 6, 1311, 0, 0},   {73, 9, 831, 0, 0},
        {75, 12, 623, 0, 0},  {104, 15, 1247, 0, 0}, {117, 18, 1217, 0, 0}, {98, 21, 532, 0, 0},
        {58, 22, 1255, 0, 0}, {54, 22, 0, 0, 0},     {45, 22, 0, -59, 0},   {73, 22, 831, 0, 0},
    };
    check_golden("NotoSansThai-Subset.ttf",
                 "\xe0\xb8\xa0\xe0\xb8\xb2\xe0\xb8\xa9\xe0\xb8\xb2\xe0\xb9\x84\xe0\xb8\x97\xe0\xb8"
                 "\xa2 \xe0\xb8\x99\xe0\xb9\x89\xe0\xb8\xb3",
                 Direction::LeftToRight, golden);
}

CY_TEST_CASE("shaping: Arabic contextual forms agree with src/text's joining, letter by letter") {
    // src/text/ decides each letter's joining form from Unicode's joining types; HarfBuzz decides
    // it from the font's init, medi and fina lookups. Noto draws a letter as a skeleton and its
    // dots (ccmp decomposes them), so the comparison is on each letter's SKELETON — the glyph of
    // its cluster that advances: where src/text says a letter is isolated, the skeleton must be the
    // one the letter has when shaped alone; where it says anything else, it must not be. "بيت"
    // (three joining letters) and "دار" (dal, alef and reh join only to the right).
    const std::string_view words[] = {"\xd8\xa8\xd9\x8a\xd8\xaa", "\xd8\xaf\xd8\xa7\xd8\xb1"};
    CompleteTextBackend backend;
    CY_REQUIRE(backend.start().has_value());
    const Array<u8> bytes = read_font("NotoSansArabic-Subset.ttf");
    const auto face = backend.open_face(desc_of(32.0f), source_of(bytes));
    CY_REQUIRE(face.has_value());
    const auto skeleton = [&](std::string_view text, u32 cluster) {
        ShapeRequest request;
        request.text = text;
        request.direction = Direction::RightToLeft;
        Array<BackendGlyph> shaped;
        CY_REQUIRE(backend.shape(face.value(), request, shaped).has_value());
        for (const BackendGlyph& glyph : shaped) {
            if (glyph.cluster == cluster && glyph.advance.x > 0.0f) {
                return glyph.glyph;
            }
        }
        return kNotdef;
    };
    for (const std::string_view word : words) {
        Array<JoinedGlyph> forms;
        CY_REQUIRE(join_arabic(word, 0, static_cast<u32>(word.size()), forms).has_value());
        CY_REQUIRE_EQ(forms.size(), 3U);
        for (const JoinedGlyph& letter : forms) {
            const std::string_view alone = word.substr(letter.source_offset, 2);
            const GlyphIndex in_word = skeleton(word, letter.source_offset);
            const GlyphIndex isolated = skeleton(alone, 0);
            CY_REQUIRE_NE(in_word, kNotdef);
            CY_TEST_INFO("byte " << letter.source_offset << ", form "
                                 << static_cast<int>(letter.form));
            CY_CHECK_EQ(in_word == isolated, letter.form == JoiningForm::Isolated);
        }
    }
}

CY_TEST_CASE("shaping: ICU and src/text resolve a mixed paragraph to the same runs") {
    // English with a Hebrew phrase and a number in it — no isolates and no brackets, the part of
    // UAX #9 src/text/ implements without approximation — so the two must agree run for run.
    const std::string_view text =
        "Shalom \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d 2024 again";
    CompleteTextBackend backend;
    CY_REQUIRE(backend.start().has_value());
    BidiResult icu(current_allocator());
    const auto resolved = backend.resolve_bidi(text, ParagraphDirection::Auto, icu);
    if (!resolved && resolved.error().code == ErrorCode::Unsupported) {
        CY_TEST_MESSAGE(
            "CY_TEXT_ICU is off: the server uses src/text/'s algorithm, nothing to compare");
        return;
    }
    CY_REQUIRE(resolved.has_value());
    BidiResult engine(current_allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::Auto, engine).has_value());
    CY_CHECK_FALSE(engine.approximated);
    CY_CHECK_EQ(icu.paragraph_level, engine.paragraph_level);
    CY_REQUIRE_EQ(icu.runs.size(), engine.runs.size());
    for (usize index = 0; index < icu.runs.size(); ++index) {
        CY_CHECK_EQ(icu.runs[index].begin, engine.runs[index].begin);
        CY_CHECK_EQ(icu.runs[index].end, engine.runs[index].end);
        CY_CHECK_EQ(icu.runs[index].level, engine.runs[index].level);
    }
}

CY_TEST_CASE("shaping: ICU pairs brackets in a right-to-left paragraph, which src/text does not") {
    // UAX #9's N0, in a right-to-left paragraph: "ש (a) b". The brackets enclose a left-to-right
    // letter and follow a right-to-left one, so N0 (rule c.2) gives BOTH the embedding direction —
    // the closing bracket is level 1. src/text/ leaves N0 out (its README says so) and resolves the
    // closing bracket by N1 instead: between "a" and "b", both left to right, so level 2. This is
    // the gap ICU closes, so it is the case that says ICU is actually the one answering.
    const std::string_view text = "\xd7\xa9 (a) b";
    constexpr u32 kOpen = 3;
    constexpr u32 kClose = 5;
    CompleteTextBackend backend;
    CY_REQUIRE(backend.start().has_value());
    BidiResult icu(current_allocator());
    const auto resolved = backend.resolve_bidi(text, ParagraphDirection::RightToLeft, icu);
    if (!resolved && resolved.error().code == ErrorCode::Unsupported) {
        CY_TEST_MESSAGE("CY_TEXT_ICU is off: nothing to compare");
        return;
    }
    CY_REQUIRE(resolved.has_value());
    BidiResult engine(current_allocator());
    CY_REQUIRE(resolve_levels(text, ParagraphDirection::RightToLeft, engine).has_value());
    const auto level_at = [](const BidiResult& result, u32 offset) {
        for (const BidiRun& run : result.runs) {
            if (offset >= run.begin && offset < run.end) {
                return static_cast<int>(run.level);
            }
        }
        return -1;
    };
    CY_CHECK_EQ(level_at(icu, kOpen), 1);
    CY_CHECK_EQ(level_at(icu, kClose), 1);
    CY_CHECK_EQ(level_at(icu, 4), 2);  // "a"
    CY_CHECK_EQ(level_at(engine, kOpen), 1);
    CY_CHECK_EQ(level_at(engine, kClose), 2);
}
