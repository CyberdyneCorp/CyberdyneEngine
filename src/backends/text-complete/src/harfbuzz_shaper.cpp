// SPDX-License-Identifier: MIT
// HarfBuzz: the one translation unit that names it. See internal.h for what crosses out of here.
#include "internal.h"

#include <hb-ot.h>
#include <hb.h>

#include <cmath>

namespace cy::text::detail {

/// The most feature settings one shaping call carries: the face's own and the request's.
constexpr unsigned kMaxShapeFeatures = static_cast<unsigned>(kMaxFontFeatures) * 2U;

struct HarfBuzzFace {
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    /// Reused by every call on this face, so shaping allocates only when a run is longer than any
    /// before it.
    hb_buffer_t* buffer = nullptr;
    hb_feature_t features[kMaxFontFeatures] = {};
    unsigned feature_count = 0;
};

namespace {

[[nodiscard]] hb_tag_t tag_of(const char (&tag)[4]) noexcept {
    return HB_TAG(tag[0], tag[1], tag[2], tag[3]);
}

[[nodiscard]] hb_feature_t feature_of(const FontFeature& feature) noexcept {
    hb_feature_t out{};
    out.tag = tag_of(feature.tag);
    out.value = feature.value;
    out.start = HB_FEATURE_GLOBAL_START;
    out.end = HB_FEATURE_GLOBAL_END;
    return out;
}

/// HarfBuzz's positions are in the font's scale, which is set to the size in 26.6 fixed point, so a
/// position is pixels times sixty-four.
[[nodiscard]] f32 to_pixels(hb_position_t value) noexcept {
    return static_cast<f32>(value) / 64.0f;
}

void release(HarfBuzzFace& face) noexcept {
    hb_buffer_destroy(face.buffer);
    hb_font_destroy(face.font);
    hb_face_destroy(face.face);
    hb_blob_destroy(face.blob);
    face = HarfBuzzFace{};
}

/// The face's features followed by the request's: a later setting of the same tag wins, which is
/// how HarfBuzz resolves the list, so a per-call override beats the face's default.
[[nodiscard]] unsigned gather_features(const HarfBuzzFace& face, Span<const FontFeature> extra,
                                       hb_feature_t (&out)[kMaxShapeFeatures]) noexcept {
    unsigned count = 0;
    for (unsigned index = 0; index < face.feature_count; ++index) {
        out[count++] = face.features[index];
    }
    for (usize index = 0; index < extra.size() && count < kMaxShapeFeatures; ++index) {
        out[count++] = feature_of(extra[index]);
    }
    return count;
}

}  // namespace

Expected<HarfBuzzFace*, Error> harfbuzz_open_face(Allocator& allocator, const FontDesc& desc,
                                                  const FontSource& source) noexcept {
    auto* opened = create<HarfBuzzFace>(allocator);
    if (opened == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate a HarfBuzz face");
    }
    // READONLY over the caller's bytes: font.h requires them to outlive the face, so HarfBuzz may
    // read them in place rather than copying a font per face.
    opened->blob = hb_blob_create(reinterpret_cast<const char*>(source.bytes.data()),
                                  static_cast<unsigned>(source.bytes.size()),
                                  HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    opened->face = hb_face_create(opened->blob, source.face_index);
    opened->font = hb_font_create(opened->face);
    opened->buffer = hb_buffer_create();
    if (hb_face_get_glyph_count(opened->face) == 0 ||
        hb_buffer_allocation_successful(opened->buffer) == 0) {
        release(*opened);
        destroy(allocator, opened);
        return fail(ErrorCode::InvalidArgument, "HarfBuzz found no glyphs in the font");
    }

    const auto scale = static_cast<int>(std::lround(desc.size_pixels * 64.0f));
    hb_font_set_scale(opened->font, scale, scale);
    hb_variation_t variations[kMaxFontAxes] = {};
    for (u32 index = 0; index < desc.axis_count && index < kMaxFontAxes; ++index) {
        variations[index].tag = tag_of(desc.axes[index].tag);
        variations[index].value = desc.axes[index].value;
    }
    if (desc.axis_count != 0) {
        hb_font_set_variations(opened->font, variations, desc.axis_count);
    }
    // The same synthetic style FreeType applies to the raster, so advances and outlines agree:
    // FT_GlyphSlot_Embolden widens by a twenty-fourth of the em and FT_GlyphSlot_Oblique shears by
    // 0x0366A in 16.16.
    if (desc.synthetic_bold) {
        hb_font_set_synthetic_bold(opened->font, 1.0f / 24.0f, 1.0f / 24.0f, 0);
    }
    if (desc.synthetic_italic) {
        hb_font_set_synthetic_slant(opened->font, static_cast<float>(0x0366A) / 65536.0f);
    }
    for (u32 index = 0; index < desc.feature_count && index < kMaxFontFeatures; ++index) {
        opened->features[index] = feature_of(desc.features[index]);
    }
    opened->feature_count = desc.feature_count;
    return opened;
}

void harfbuzz_close_face(Allocator& allocator, HarfBuzzFace* face) noexcept {
    if (face != nullptr) {
        release(*face);
    }
    destroy(allocator, face);
}

Status harfbuzz_shape(HarfBuzzFace& face, const ShapeRequest& request,
                      Array<BackendGlyph>& out) noexcept {
    out.clear();
    if (request.direction == Direction::TopToBottom) {
        return fail(ErrorCode::Unsupported, "vertical layout is not built in this backend");
    }
    hb_buffer_t* buffer = face.buffer;
    hb_buffer_clear_contents(buffer);
    const auto length = static_cast<int>(request.text.size());
    // The length is passed, so HarfBuzz never looks for a terminator; the check cannot see that a
    // C API's second argument is the size.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    hb_buffer_add_utf8(buffer, request.text.data(), length, 0, length);
    hb_buffer_set_direction(
        buffer, request.direction == Direction::RightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    if (!request.language.empty() && request.language.size() < 64) {
        // HarfBuzz reads a language tag up to a terminator when given no length, and the request's
        // view has none; a copy with one is the unambiguous form.
        char tag[64] = {};
        (void)request.language.copy(tag, request.language.size());
        hb_buffer_set_language(buffer, hb_language_from_string(tag, -1));
    }
    // The script — and the language, when the request names none — from the text itself. A run is
    // one script by construction, so the guess is the run's script.
    hb_buffer_guess_segment_properties(buffer);

    hb_feature_t features[kMaxShapeFeatures] = {};
    const unsigned feature_count = gather_features(face, request.features, features);
    hb_shape(face.font, buffer, features, feature_count);
    if (hb_buffer_allocation_successful(buffer) == 0) {
        return fail(ErrorCode::OutOfMemory, "HarfBuzz could not grow its buffer");
    }

    unsigned count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    if (Status reserved = out.reserve(count); !reserved) {
        return reserved;
    }
    for (unsigned index = 0; index < count; ++index) {
        BackendGlyph glyph;
        glyph.glyph = infos[index].codepoint;
        glyph.cluster = infos[index].cluster;
        glyph.advance =
            Vec2{to_pixels(positions[index].x_advance), -to_pixels(positions[index].y_advance)};
        // HarfBuzz's Y grows up; this module's grows down.
        glyph.offset =
            Vec2{to_pixels(positions[index].x_offset), -to_pixels(positions[index].y_offset)};
        if (Status pushed = out.push_back(glyph); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status harfbuzz_closure(HarfBuzzFace& face, Span<const Codepoint> codepoints,
                        Array<GlyphIndex>& out) noexcept {
    out.clear();
    hb_buffer_t* buffer = face.buffer;
    hb_buffer_clear_contents(buffer);
    for (const Codepoint codepoint : codepoints) {
        hb_buffer_add(buffer, static_cast<hb_codepoint_t>(codepoint), 0);
    }
    hb_buffer_set_content_type(buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_guess_segment_properties(buffer);
    hb_set_t* glyphs = hb_set_create();
    hb_ot_shape_glyphs_closure(face.font, buffer, face.features, face.feature_count, glyphs);
    hb_buffer_clear_contents(buffer);
    Status status = hb_set_allocation_successful(glyphs) != 0
                        ? ok()
                        : Status(fail(ErrorCode::OutOfMemory, "HarfBuzz could not grow a set"));
    hb_codepoint_t glyph = HB_SET_VALUE_INVALID;
    while (status && hb_set_next(glyphs, &glyph) != 0) {
        status = out.push_back(static_cast<GlyphIndex>(glyph));
    }
    hb_set_destroy(glyphs);
    return status;
}

f32 harfbuzz_advance(HarfBuzzFace& face, GlyphIndex glyph) noexcept {
    return to_pixels(hb_font_get_glyph_h_advance(face.font, glyph));
}

}  // namespace cy::text::detail
