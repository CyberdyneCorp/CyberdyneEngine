// SPDX-License-Identifier: MIT
// The complete text backend behind the text server: loading, rasterisation, the three atlases,
// bidirectional layout, containers and synthetic styles. M11.e, issue #86.

#include "fonts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

using namespace cy;
using namespace cy::text;
using namespace cy::text::test;

namespace {

[[nodiscard]] FontDesc weighted(f32 size, f32 weight) {
    FontDesc desc = desc_of(size);
    std::memcpy(desc.axes[0].tag, "wght", 4);
    desc.axes[0].value = weight;
    desc.axis_count = 1;
    return desc;
}

[[nodiscard]] u64 coverage_sum(const GlyphAtlas& atlas, const GlyphSlot& slot) {
    u64 sum = 0;
    const Span<const u8> pixels = atlas.pixels();
    for (i32 y = 0; y < slot.rect.size.y; ++y) {
        for (i32 x = 0; x < slot.rect.size.x; ++x) {
            sum += pixels[(static_cast<usize>(slot.rect.position.y + y) * atlas.extent()) +
                          static_cast<usize>(slot.rect.position.x + x)];
        }
    }
    return sum;
}

/// One RGBA texel of a four-byte atlas, as [0, 1] floats, clamped to the slot.
void texel(const GlyphAtlas& atlas, const GlyphSlot& slot, i32 x, i32 y, f32 (&out)[4]) {
    x = std::clamp(x, 0, slot.rect.size.x - 1);
    y = std::clamp(y, 0, slot.rect.size.y - 1);
    const usize index = ((static_cast<usize>(slot.rect.position.y + y) * atlas.extent()) +
                         static_cast<usize>(slot.rect.position.x + x)) *
                        4U;
    for (usize channel = 0; channel < 4; ++channel) {
        out[channel] = static_cast<f32>(atlas.pixels()[index + channel]) / 255.0f;
    }
}

[[nodiscard]] f32 median(f32 a, f32 b, f32 c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

/// The distance field drawn at `scale`: bilinear samples of the field, the median of the three
/// channels, and a one-pixel ramp across the outline — what the interface shader does.
[[nodiscard]] f32 field_coverage(const GlyphAtlas& atlas, const GlyphSlot& slot, f32 range,
                                 f32 scale, f32 x, f32 y) {
    const f32 u = (x / scale) - 0.5f;
    const f32 v = (y / scale) - 0.5f;
    const auto x0 = static_cast<i32>(std::floor(u));
    const auto y0 = static_cast<i32>(std::floor(v));
    const f32 fx = u - static_cast<f32>(x0);
    const f32 fy = v - static_cast<f32>(y0);
    f32 corners[4][4] = {};
    texel(atlas, slot, x0, y0, corners[0]);
    texel(atlas, slot, x0 + 1, y0, corners[1]);
    texel(atlas, slot, x0, y0 + 1, corners[2]);
    texel(atlas, slot, x0 + 1, y0 + 1, corners[3]);
    f32 sample[3] = {};
    for (usize channel = 0; channel < 3; ++channel) {
        const f32 top = corners[0][channel] + ((corners[1][channel] - corners[0][channel]) * fx);
        const f32 bottom = corners[2][channel] + ((corners[3][channel] - corners[2][channel]) * fx);
        sample[channel] = top + ((bottom - top) * fy);
    }
    const f32 distance = (median(sample[0], sample[1], sample[2]) - 0.5f) * 2.0f * range * scale;
    return std::clamp(distance + 0.5f, 0.0f, 1.0f);
}

struct EdgeError {
    f32 mean = 0.0f;
    f32 worst = 0.0f;
    u32 pixels = 0;
    /// Pixels more than a quarter of full coverage away from FreeType's.
    u32 large = 0;
};

/// Compare the distance field of `glyph` drawn at `scale` with FreeType's unhinted grayscale raster
/// of the same glyph at that size, over every pixel either of them touches.
[[nodiscard]] EdgeError edge_error(Stack& stack, const Array<u8>& font, FontHandle field_face,
                                   GlyphIndex glyph, f32 base_size, f32 range, f32 scale) {
    const GlyphSlot field = *stack.server.glyph_slot(field_face, glyph).value();
    FontDesc reference_desc = desc_of(base_size * scale);
    reference_desc.hinting = Hinting::None;
    const FontHandle reference_face = stack.face(font, reference_desc);
    const GlyphSlot reference = *stack.server.glyph_slot(reference_face, glyph).value();
    const GlyphAtlas& coverage = stack.server.atlas(PixelFormat::Coverage);
    const GlyphAtlas& distance = stack.server.atlas(PixelFormat::DistanceField);

    // Both rasters in one frame: pixel (0, 0) is the field's top-left at this scale.
    const f32 field_left = field.metrics.bearing_x * scale;
    const f32 field_top = field.metrics.bearing_y * scale;
    const auto width = static_cast<i32>(std::ceil(static_cast<f32>(field.metrics.width) * scale));
    const auto height = static_cast<i32>(std::ceil(static_cast<f32>(field.metrics.height) * scale));
    const auto offset_x = static_cast<i32>(std::lround(reference.metrics.bearing_x - field_left));
    const auto offset_y = static_cast<i32>(std::lround(reference.metrics.bearing_y - field_top));

    EdgeError error;
    f64 total = 0.0;
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            const f32 drawn =
                field_coverage(distance, field, range, scale, static_cast<f32>(x) + 0.5f,
                               static_cast<f32>(y) + 0.5f);
            const i32 rx = x - offset_x;
            const i32 ry = y - offset_y;
            f32 expected = 0.0f;
            if (rx >= 0 && ry >= 0 && rx < reference.rect.size.x && ry < reference.rect.size.y) {
                expected =
                    static_cast<f32>(
                        coverage.pixels()[(static_cast<usize>(reference.rect.position.y + ry) *
                                           coverage.extent()) +
                                          static_cast<usize>(reference.rect.position.x + rx)]) /
                    255.0f;
            }
            if (drawn == 0.0f && expected == 0.0f) {
                continue;
            }
            const f32 difference = std::fabs(drawn - expected);
            total += static_cast<f64>(difference);
            error.worst = std::max(error.worst, difference);
            error.large += difference > 0.25f ? 1U : 0U;
            ++error.pixels;
        }
    }
    error.mean = error.pixels != 0 ? static_cast<f32>(total / error.pixels) : 0.0f;
    return error;
}

}  // namespace

CY_TEST_CASE("complete: the backend says what it can do, and the server reports it as its own") {
    Stack stack;
    const TextCapabilities& capabilities = stack.server.capabilities();
    CY_CHECK(capabilities.outline_fonts);
    CY_CHECK(capabilities.complex_shaping);
    CY_CHECK(capabilities.bidirectional);
    CY_CHECK(capabilities.colour_glyphs);
    CY_CHECK(capabilities.variable_fonts);
    CY_CHECK(capabilities.signed_distance_fields);
    // Declared false, each for a reason the README gives.
    CY_CHECK_FALSE(capabilities.dictionary_line_breaking);
    CY_CHECK_FALSE(capabilities.subpixel_positioning);
    CY_CHECK_FALSE(capabilities.vertical_layout);
    CY_CHECK(std::string_view(capabilities.backend) == "complete");
}

CY_TEST_CASE(
    "complete: a variable face rasterises one glyph at two weights into two cache entries") {
    // `text-and-fonts` — Font loading: variable fonts load with their axes, and the glyph cache is
    // keyed by the axis values. Two faces of one file at wght 400 and wght 800 are two instances;
    // the same glyph from each must be two atlas entries with two different rasters.
    Stack stack;
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    const FontHandle regular = stack.face(font, weighted(32.0f, 400.0f));
    const FontHandle bold = stack.face(font, weighted(32.0f, 800.0f));
    const GlyphIndex glyph = stack.server.glyph_for(regular, 'H');
    CY_REQUIRE_NE(glyph, kNotdef);
    CY_CHECK_EQ(stack.server.glyph_for(bold, 'H'), glyph);

    const GlyphSlot* light = stack.server.glyph_slot(regular, glyph).value();
    const GlyphSlot* heavy = stack.server.glyph_slot(bold, glyph).value();
    CY_CHECK(light != heavy);
    CY_CHECK_FALSE(light->rect.position == heavy->rect.position);
    const GlyphAtlas& atlas = stack.server.atlas();
    CY_CHECK_EQ(atlas.live_glyphs(), 2U);
    CY_CHECK_EQ(stack.server.diagnostics().glyphs_rasterised, 2U);
    // The heavier instance puts more ink down: a stem twice as thick, roughly.
    CY_CHECK_GT(coverage_sum(atlas, *heavy), coverage_sum(atlas, *light) * 13 / 10);
    // And the shaper agrees the instances differ: H is wider at 800.
    const auto light_width = stack.server.measure("H", chain_of(regular));
    const auto heavy_width = stack.server.measure("H", chain_of(bold));
    CY_CHECK_GT(heavy_width.value().x, light_width.value().x);
}

CY_TEST_CASE("complete: an axis the font does not have is refused, not ignored") {
    Stack stack;
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    FontDesc desc = weighted(32.0f, 400.0f);
    std::memcpy(desc.axes[0].tag, "opsz", 4);
    const auto refused = stack.server.create_face(desc, source_of(font));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE(
    "complete: one distance-field entry draws a glyph at two scales within the edge bound") {
    // `text-and-fonts` — "MSDF SHALL be used where text must scale". One atlas entry, generated at
    // 32 pixels, is drawn at 32 and at 64 and compared with FreeType's own anti-aliased raster at
    // each size. The bound is on the mean error over every pixel either touches — which is the
    // edge, since both are exact away from it — and on how many pixels are off by more than a
    // quarter: a corner the field rounds is one or two, while a field with the wrong sign, range or
    // scale puts the whole edge out. Measured on this tree: means of 0.006 to 0.017, and three
    // pixels in 829 off by a quarter (the sharp corners of 'g' at twice the size).
    Stack stack;
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    constexpr f32 kRange = 4.0f;
    FontDesc desc = desc_of(32.0f, RenderMode::SignedDistanceField);
    desc.distance_range = kRange;
    const FontHandle face = stack.face(font, desc);
    for (const Codepoint codepoint : {Codepoint{'g'}, Codepoint{'R'}}) {
        const GlyphIndex glyph = stack.server.glyph_for(face, codepoint);
        const GlyphSlot* slot = stack.server.glyph_slot(face, glyph).value();
        CY_CHECK_EQ(slot->page, static_cast<u32>(PixelFormat::DistanceField));
        for (const f32 scale : {1.0f, 2.0f}) {
            const EdgeError error = edge_error(stack, font, face, glyph, 32.0f, kRange, scale);
            CY_TEST_MESSAGE("U+" << static_cast<u32>(codepoint) << " at x" << scale << ": mean "
                                 << error.mean << ", worst " << error.worst << ", " << error.large
                                 << " of " << error.pixels << " off by more than a quarter");
            CY_CHECK_GT(error.pixels, 100U);
            CY_CHECK_LT(error.mean, 0.035f);
            CY_CHECK_LE(error.large * 50U, error.pixels);
        }
    }
    // Two glyphs, each rasterised ONCE into the distance-field atlas, whatever scales they drew at.
    CY_CHECK_EQ(stack.server.atlas(PixelFormat::DistanceField).live_glyphs(), 2U);
    CY_CHECK_EQ(stack.server.atlas(PixelFormat::DistanceField).diagnostics().glyphs_rasterised, 2U);
}

CY_TEST_CASE("complete: a COLR glyph lands in the colour atlas in its palette's colours") {
    // CyberColourTest.ttf's U+25A0 is two COLR layers: a red square, and a smaller blue one inside
    // it. A grayscale face asked for it gets a COLOUR raster, in the colour atlas, and nothing in
    // the coverage atlas.
    Stack stack;
    const Array<u8> font = read_font("CyberColourTest.ttf");
    const FontHandle face = stack.face(font, desc_of(40.0f));
    const GlyphIndex glyph = stack.server.glyph_for(face, 0x25A0);
    CY_REQUIRE_NE(glyph, kNotdef);
    const GlyphSlot* slot = stack.server.glyph_slot(face, glyph).value();
    CY_CHECK(slot->colour);
    CY_CHECK_EQ(slot->page, static_cast<u32>(PixelFormat::Colour));
    CY_CHECK_EQ(stack.server.atlas(PixelFormat::Coverage).live_glyphs(), 0U);
    CY_REQUIRE(slot->rect.size.x > 20);

    const GlyphAtlas& atlas = stack.server.atlas(PixelFormat::Colour);
    f32 centre[4] = {};
    f32 rim[4] = {};
    texel(atlas, *slot, slot->rect.size.x / 2, slot->rect.size.y / 2, centre);
    texel(atlas, *slot, 2, slot->rect.size.y / 2, rim);
    CY_CHECK_GT(centre[2], 0.95f);  // blue
    CY_CHECK_LT(centre[0], 0.05f);
    CY_CHECK_GT(centre[3], 0.95f);
    CY_CHECK_GT(rim[0], 0.95f);  // red
    CY_CHECK_LT(rim[2], 0.05f);
}

CY_TEST_CASE("complete: a mixed line is placed in visual order, the Hebrew right to left") {
    // A Latin face with a Hebrew face behind it in the chain. "ab שלום cd": the Hebrew word's first
    // letter (ש, byte 3) is drawn LAST of the four, and the Latin on either side keeps its order.
    Stack stack;
    const Array<u8> latin = read_font("NotoSans-Latin-VF.ttf");
    const Array<u8> hebrew = read_font("NotoSansHebrew-Subset.ttf");
    const FallbackChain chain =
        chain_of(stack.face(latin, desc_of(24.0f)), stack.face(hebrew, desc_of(24.0f)));
    TextLine line;
    CY_REQUIRE(stack.server.layout_line("ab \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d cd", chain, line)
                   .has_value());
    u32 order[16] = {};
    usize count = 0;
    for (const ShapedGlyph& glyph : line.run().glyphs) {
        CY_REQUIRE(count < 16U);
        order[count++] = glyph.source_offset;
    }
    const u32 visual[] = {0, 1, 2, 9, 7, 5, 3, 11, 12, 13};
    CY_REQUIRE_EQ(count, std::size(visual));
    for (usize index = 0; index < count; ++index) {
        CY_CHECK_EQ(order[index], visual[index]);
    }
    // And the pen only ever moves right.
    for (usize index = 1; index < line.run().glyphs.size(); ++index) {
        CY_CHECK_GE(line.run().glyphs[index].offset.x, line.run().glyphs[index - 1].offset.x);
    }
    // The fallback is reported against the primary that needed it.
    Array<FallbackReport> report;
    CY_REQUIRE(stack.server.fallback_report(report).has_value());
    CY_REQUIRE_EQ(report.size(), 1U);
    CY_CHECK_EQ(report[0].fallbacks, 4U);
}

CY_TEST_CASE("complete: WOFF and a collection shape exactly as the face inside them") {
    Stack stack;
    const Array<u8> plain = read_font("NotoSansHebrew-Subset.ttf");
    const Array<u8> woff = read_font("NotoSansHebrew-Subset.woff");
    const Array<u8> collection = read_font("NotoSansThaiHebrew.ttc");
    const std::string_view text = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    ShapedRun expected;
    CY_REQUIRE(stack.server
                   .shape(text, chain_of(stack.face(plain, desc_of(20.0f))), Direction::RightToLeft,
                          expected)
                   .has_value());
    for (const FontHandle face :
         {stack.face(woff, desc_of(20.0f)), stack.face(collection, desc_of(20.0f), 1)}) {
        ShapedRun run;
        CY_REQUIRE(
            stack.server.shape(text, chain_of(face), Direction::RightToLeft, run).has_value());
        CY_REQUIRE_EQ(run.glyphs.size(), expected.glyphs.size());
        for (usize index = 0; index < run.glyphs.size(); ++index) {
            CY_CHECK_EQ(run.glyphs[index].glyph, expected.glyphs[index].glyph);
            CY_CHECK_EQ(run.glyphs[index].advance, expected.glyphs[index].advance);
        }
        // And it rasterises: a WOFF's tables are compressed, and FreeType reads through that.
        CY_CHECK(stack.server.glyph_slot(face, run.glyphs[0].glyph).has_value());
    }
    // Face 0 of the collection is the Thai one: it has no Hebrew.
    const FontHandle thai = stack.face(collection, desc_of(20.0f), 0);
    CY_CHECK_EQ(stack.server.glyph_for(thai, 0x05E9), kNotdef);
    CY_CHECK_NE(stack.server.glyph_for(thai, 0x0E01), kNotdef);
}

CY_TEST_CASE("complete: WOFF2 is refused, naming the decompressor this build leaves out") {
    Stack stack;
    Array<u8> woff2;
    const u8 header[] = {'w', 'O', 'F', '2', 0, 1, 0, 0, 0, 0, 0, 64, 0, 0, 0, 0};
    CY_REQUIRE(woff2.append(Span<const u8>(header, sizeof(header))).has_value());
    const auto refused = stack.server.create_face(desc_of(16.0f), source_of(woff2));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unsupported);
    CY_CHECK(std::string_view(refused.error().message).find("Brotli") != std::string_view::npos);
}

CY_TEST_CASE("complete: synthetic bold widens a glyph and synthetic italic leans it") {
    Stack stack;
    const Array<u8> font =
        read_font("NotoSansHebrew-Subset.ttf");  // a static face: no bold of its own
    FontDesc plain = desc_of(48.0f);
    plain.hinting = Hinting::None;
    FontDesc bold = plain;
    bold.synthetic_bold = true;
    FontDesc italic = plain;
    italic.synthetic_italic = true;
    const FontHandle regular_face = stack.face(font, plain);
    const FontHandle bold_face = stack.face(font, bold);
    const FontHandle italic_face = stack.face(font, italic);
    const GlyphIndex glyph = stack.server.glyph_for(regular_face, 0x05D5);  // vav: one stem
    CY_REQUIRE_NE(glyph, kNotdef);

    const GlyphAtlas& atlas = stack.server.atlas();
    const GlyphSlot regular = *stack.server.glyph_slot(regular_face, glyph).value();
    const GlyphSlot emboldened = *stack.server.glyph_slot(bold_face, glyph).value();
    CY_CHECK_GT(coverage_sum(atlas, emboldened), coverage_sum(atlas, regular) * 11 / 10);
    // The shaper widens the advance by the same twenty-fourth of the em the rasteriser does.
    CY_CHECK_GT(stack.server.measure("\xd7\x95", chain_of(bold_face)).value().x,
                stack.server.measure("\xd7\x95", chain_of(regular_face)).value().x + 1.0f);

    // A shear moves the top of the stem right of its bottom.
    const GlyphSlot leaning = *stack.server.glyph_slot(italic_face, glyph).value();
    const auto centre_of_row = [&atlas](const GlyphSlot& slot, i32 row) {
        f64 weighted = 0.0;
        f64 total = 0.0;
        for (i32 x = 0; x < slot.rect.size.x; ++x) {
            const f64 value =
                atlas.pixels()[(static_cast<usize>(slot.rect.position.y + row) * atlas.extent()) +
                               static_cast<usize>(slot.rect.position.x + x)];
            weighted += value * x;
            total += value;
        }
        return total > 0.0 ? weighted / total : 0.0;
    };
    // FreeType's oblique shears by 0x0366A / 0x10000, about 0.21 pixels across per pixel up; the
    // rows compared are half the glyph's height apart, so the lean over the upright glyph's must be
    // most of 0.21 of that.
    const i32 top = leaning.rect.size.y / 4;
    const i32 bottom = (leaning.rect.size.y * 3) / 4;
    const f64 regular_lean = centre_of_row(regular, regular.rect.size.y / 4) -
                             centre_of_row(regular, (regular.rect.size.y * 3) / 4);
    const f64 lean = centre_of_row(leaning, top) - centre_of_row(leaning, bottom) - regular_lean;
    CY_CHECK_GT(lean, 0.15 * static_cast<f64>(bottom - top));
    CY_CHECK_LT(lean, 0.27 * static_cast<f64>(bottom - top));
}

CY_TEST_CASE("complete: the glyph closure reaches the ligature the character map does not") {
    // What a font importer pre-renders: "f" and "i" are in the character map; "ffi" (220) is not,
    // and only GSUB reaches it.
    CompleteTextBackend backend;
    CY_REQUIRE(backend.start().has_value());
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    const auto face = backend.open_face(desc_of(16.0f), source_of(font));
    CY_REQUIRE(face.has_value());
    const Codepoint codepoints[] = {'f', 'i'};
    Array<GlyphIndex> closure;
    CY_REQUIRE(backend.glyph_closure(face.value(), codepoints, closure).has_value());
    const auto contains = [&closure](GlyphIndex glyph) {
        return std::find(closure.begin(), closure.end(), glyph) != closure.end();
    };
    CY_CHECK(contains(backend.glyph_for(face.value(), 'f')));
    CY_CHECK(contains(backend.glyph_for(face.value(), 'i')));
    CY_CHECK(contains(220));
    CY_CHECK(std::is_sorted(closure.begin(), closure.end()));
}

CY_TEST_CASE("complete: LCD subpixel rendering is refused rather than drawn as grayscale") {
    Stack stack;
    const Array<u8> font = read_font("NotoSans-Latin-VF.ttf");
    const auto refused =
        stack.server.create_face(desc_of(16.0f, RenderMode::SubpixelLcd), source_of(font));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unsupported);
}
