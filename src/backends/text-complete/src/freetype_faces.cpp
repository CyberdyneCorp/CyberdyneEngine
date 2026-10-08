// SPDX-License-Identifier: MIT
// FreeType: the one translation unit that names it. See internal.h for what crosses out of here.
#include "internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H

#include <cmath>
#include <cstring>

namespace cy::text::detail {

struct FreeTypeLibrary {
    FT_Library library = nullptr;
};

struct FreeTypeFace {
    FT_Face face = nullptr;
    FontDesc desc;
    FT_Int32 load_flags = FT_LOAD_DEFAULT;
    FT_Render_Mode render_mode = FT_RENDER_MODE_NORMAL;
};

namespace {

[[nodiscard]] FT_ULong tag_of(const char (&tag)[4]) noexcept {
    return FT_MAKE_TAG(tag[0], tag[1], tag[2], tag[3]);
}

[[nodiscard]] f32 from_26_6(FT_Pos value) noexcept {
    return static_cast<f32>(value) / 64.0f;
}

/// Whether the bytes are a WOFF2 file. FreeType reads one only with Brotli, which this build leaves
/// out (cmake/dependencies.cmake), and its error for the attempt is "unknown file format" — which
/// sends a reader looking for a corrupt file rather than for a build option.
[[nodiscard]] bool is_woff2(Span<const u8> bytes) noexcept {
    return bytes.size() >= 4 && bytes[0] == 'w' && bytes[1] == 'O' && bytes[2] == 'F' &&
           bytes[3] == '2';
}

/// Pin the variable-font instance `desc` asks for, starting from the font's defaults so an axis the
/// caller did not name keeps its designed value.
[[nodiscard]] Status set_instance(FT_Library library, FT_Face face, const FontDesc& desc) noexcept {
    if (desc.axis_count == 0) {
        return ok();
    }
    if (!FT_HAS_MULTIPLE_MASTERS(face)) {
        return fail(ErrorCode::InvalidArgument,
                    "variable-font axes were asked of a face that has none");
    }
    FT_MM_Var* variation = nullptr;
    if (FT_Get_MM_Var(face, &variation) != 0 || variation == nullptr) {
        return fail(ErrorCode::InvalidArgument, "FreeType could not read the font's axes");
    }
    FT_Fixed coordinates[16] = {};
    const FT_UInt count = variation->num_axis < 16 ? variation->num_axis : 16;
    for (FT_UInt axis = 0; axis < count; ++axis) {
        coordinates[axis] = variation->axis[axis].def;
    }
    Status status = ok();
    for (u32 index = 0; index < desc.axis_count && status; ++index) {
        bool found = false;
        for (FT_UInt axis = 0; axis < count; ++axis) {
            if (variation->axis[axis].tag == tag_of(desc.axes[index].tag)) {
                coordinates[axis] =
                    static_cast<FT_Fixed>(std::lround(desc.axes[index].value * 65536.0f));
                found = true;
            }
        }
        if (!found) {
            status =
                fail(ErrorCode::InvalidArgument, "a variable-font axis the font does not have");
        }
    }
    if (status && FT_Set_Var_Design_Coordinates(face, count, coordinates) != 0) {
        status = fail(ErrorCode::InvalidArgument, "FreeType refused the variable-font instance");
    }
    FT_Done_MM_Var(library, variation);
    return status;
}

[[nodiscard]] FT_Int32 load_flags_for(const FontDesc& desc, bool colour) noexcept {
    FT_Int32 flags = FT_LOAD_DEFAULT;
    if (desc.mode == RenderMode::Monochrome) {
        flags |= FT_LOAD_TARGET_MONO;
    } else if (desc.hinting == Hinting::None) {
        flags |= FT_LOAD_NO_HINTING;
    } else if (desc.hinting == Hinting::Light) {
        flags |= FT_LOAD_TARGET_LIGHT;
    } else {
        flags |= FT_LOAD_TARGET_NORMAL;
    }
    if (colour) {
        flags |= FT_LOAD_COLOR;
    }
    return flags;
}

void apply_synthetic_style(const FontDesc& desc, FT_GlyphSlot slot) noexcept {
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE) {
        return;
    }
    if (desc.synthetic_bold) {
        FT_GlyphSlot_Embolden(slot);
    }
    if (desc.synthetic_italic) {
        FT_GlyphSlot_Oblique(slot);
    }
}

/// Copy FreeType's bitmap, whose rows are `pitch` bytes apart and may run bottom up, into a tight
/// top-down raster of the engine's format.
[[nodiscard]] Status copy_bitmap(const FT_Bitmap& bitmap, GlyphRaster& out) noexcept {
    const u32 width = bitmap.width;
    const u32 height = bitmap.rows;
    const bool colour = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
    if (!colour && bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
        bitmap.pixel_mode != FT_PIXEL_MODE_MONO) {
        return fail(ErrorCode::Unsupported,
                    "FreeType produced a bitmap format the atlas cannot hold");
    }
    out.format = colour ? PixelFormat::Colour : PixelFormat::Coverage;
    const usize bpp = bytes_per_pixel(out.format);
    if (Status resized = out.pixels.resize(static_cast<usize>(width) * height * bpp); !resized) {
        return resized;
    }
    const auto pitch = static_cast<isize>(bitmap.pitch);
    const u8* origin = bitmap.buffer;
    if (pitch < 0 && height > 0) {
        origin -= pitch * static_cast<isize>(height - 1);
    }
    for (u32 y = 0; y < height; ++y) {
        const u8* row = origin + (pitch * static_cast<isize>(y));
        u8* target = out.pixels.data() + (static_cast<usize>(y) * width * bpp);
        for (u32 x = 0; x < width; ++x) {
            if (colour) {
                // FreeType's BGRA is premultiplied already, which is what the colour atlas holds.
                target[(x * 4) + 0] = row[(x * 4) + 2];
                target[(x * 4) + 1] = row[(x * 4) + 1];
                target[(x * 4) + 2] = row[(x * 4) + 0];
                target[(x * 4) + 3] = row[(x * 4) + 3];
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                const bool set = ((row[x / 8] >> (7 - (x % 8))) & 1U) != 0;
                target[x] = set ? 255U : 0U;
            } else {
                target[x] = row[x];
            }
        }
    }
    return ok();
}

struct Decomposer {
    GlyphOutline* outline = nullptr;
    Status status = ok();

    void add(GlyphOutline::Verb verb, const FT_Vector* const* points, usize count) noexcept {
        if (!status) {
            return;
        }
        status = outline->verbs.push_back(verb);
        for (usize index = 0; status && index < count; ++index) {
            status = outline->points.push_back(
                GlyphOutline::Point{static_cast<f64>(points[index]->x) / 64.0,
                                    static_cast<f64>(points[index]->y) / 64.0});
        }
    }
};

int move_to(const FT_Vector* to, void* user) {
    const FT_Vector* points[] = {to};
    static_cast<Decomposer*>(user)->add(GlyphOutline::Verb::Move, points, 1);
    return 0;
}

int line_to(const FT_Vector* to, void* user) {
    const FT_Vector* points[] = {to};
    static_cast<Decomposer*>(user)->add(GlyphOutline::Verb::Line, points, 1);
    return 0;
}

int conic_to(const FT_Vector* control, const FT_Vector* to, void* user) {
    const FT_Vector* points[] = {control, to};
    static_cast<Decomposer*>(user)->add(GlyphOutline::Verb::Quadratic, points, 2);
    return 0;
}

int cubic_to(const FT_Vector* first, const FT_Vector* second, const FT_Vector* to, void* user) {
    const FT_Vector* points[] = {first, second, to};
    static_cast<Decomposer*>(user)->add(GlyphOutline::Verb::Cubic, points, 3);
    return 0;
}

}  // namespace

Expected<FreeTypeLibrary*, Error> freetype_open_library(Allocator& allocator) noexcept {
    auto* library = create<FreeTypeLibrary>(allocator);
    if (library == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate the FreeType library");
    }
    if (FT_Init_FreeType(&library->library) != 0) {
        destroy(allocator, library);
        return fail(ErrorCode::Internal, "FreeType failed to initialise");
    }
    return library;
}

void freetype_close_library(Allocator& allocator, FreeTypeLibrary* library) noexcept {
    if (library != nullptr && library->library != nullptr) {
        FT_Done_FreeType(library->library);
    }
    destroy(allocator, library);
}

Expected<FreeTypeFace*, Error> freetype_open_face(Allocator& allocator, FreeTypeLibrary& library,
                                                  const FontDesc& desc,
                                                  const FontSource& source) noexcept {
    if (is_woff2(source.bytes)) {
        return fail(ErrorCode::Unsupported,
                    "WOFF2 needs Brotli, which this build leaves out of FreeType; convert the font "
                    "to TrueType, OpenType or WOFF before importing it");
    }
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(library.library, source.bytes.data(),
                           static_cast<FT_Long>(source.bytes.size()),
                           static_cast<FT_Long>(source.face_index), &face) != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "FreeType could not read the font: it is not TrueType, OpenType, a collection "
                    "or WOFF, or the face index is past the collection's end");
    }
    Status status = set_instance(library.library, face, desc);
    if (status &&
        FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(std::lround(desc.size_pixels * 64.0f)),
                         72, 72) != 0) {
        status = fail(ErrorCode::InvalidArgument, "FreeType could not size the face");
    }
    auto* opened = status ? create<FreeTypeFace>(allocator) : nullptr;
    if (status && opened == nullptr) {
        status = fail(ErrorCode::OutOfMemory, "could not allocate a FreeType face");
    }
    if (!status) {
        FT_Done_Face(face);
        return make_unexpected(status.error());
    }
    opened->face = face;
    opened->desc = desc;
    opened->load_flags = load_flags_for(desc, FT_HAS_COLOR(face));
    opened->render_mode =
        desc.mode == RenderMode::Monochrome ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
    return opened;
}

void freetype_close_face(Allocator& allocator, FreeTypeFace* face) noexcept {
    if (face != nullptr && face->face != nullptr) {
        FT_Done_Face(face->face);
    }
    destroy(allocator, face);
}

FontMetrics freetype_metrics(const FreeTypeFace& face) noexcept {
    const FT_Size_Metrics& size = face.face->size->metrics;
    FontMetrics metrics;
    metrics.ascent = from_26_6(size.ascender);
    metrics.descent = -from_26_6(size.descender);
    const f32 gap = from_26_6(size.height) - metrics.ascent - metrics.descent;
    metrics.line_gap = gap > 0.0f ? gap : 0.0f;
    const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face.face, FT_SFNT_OS2));
    if (os2 != nullptr && os2->version >= 2 && os2->version != 0xFFFF) {
        metrics.x_height = from_26_6(FT_MulFix(os2->sxHeight, size.y_scale));
        metrics.cap_height = from_26_6(FT_MulFix(os2->sCapHeight, size.y_scale));
    }
    metrics.monospace = FT_IS_FIXED_WIDTH(face.face);
    return metrics;
}

GlyphIndex freetype_glyph_for(const FreeTypeFace& face, Codepoint codepoint) noexcept {
    return FT_Get_Char_Index(face.face, static_cast<FT_ULong>(codepoint));
}

Status freetype_sfnt(const FreeTypeFace& face, Array<u8>& out) noexcept {
    FT_ULong length = 0;
    // Tag zero is "the whole font": for a WOFF file, the sfnt FreeType decompressed it into.
    if (FT_Load_Sfnt_Table(face.face, 0, 0, nullptr, &length) != 0 || length == 0) {
        return fail(ErrorCode::InvalidArgument, "FreeType has no sfnt for this face");
    }
    if (Status resized = out.resize(length); !resized) {
        return resized;
    }
    if (FT_Load_Sfnt_Table(face.face, 0, 0, out.data(), &length) != 0) {
        return fail(ErrorCode::InvalidArgument, "FreeType could not copy the face's sfnt");
    }
    return ok();
}

bool freetype_has_colour(const FreeTypeFace& face) noexcept {
    return FT_HAS_COLOR(face.face);
}

Status freetype_rasterise(FreeTypeFace& face, GlyphIndex glyph, GlyphRaster& out) noexcept {
    if (FT_Load_Glyph(face.face, glyph, face.load_flags) != 0) {
        return fail(ErrorCode::InvalidArgument, "FreeType could not load the glyph");
    }
    FT_GlyphSlot slot = face.face->glyph;
    apply_synthetic_style(face.desc, slot);
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, face.render_mode) != 0) {
        return fail(ErrorCode::InvalidArgument, "FreeType could not rasterise the glyph");
    }
    if (Status copied = copy_bitmap(slot->bitmap, out); !copied) {
        return copied;
    }
    out.metrics.advance = from_26_6(slot->advance.x);
    out.metrics.bearing_x = static_cast<f32>(slot->bitmap_left);
    out.metrics.bearing_y = -static_cast<f32>(slot->bitmap_top);
    out.metrics.width = slot->bitmap.width;
    out.metrics.height = slot->bitmap.rows;
    return ok();
}

Status freetype_outline(FreeTypeFace& face, GlyphIndex glyph, GlyphOutline& out) noexcept {
    // Unhinted: a distance field is drawn at every size but this one, and an outline fitted to this
    // size's pixel grid would carry that fitting to all of them.
    if (FT_Load_Glyph(face.face, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) {
        return fail(ErrorCode::InvalidArgument, "FreeType could not load the glyph");
    }
    FT_GlyphSlot slot = face.face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE) {
        return fail(ErrorCode::Unsupported,
                    "a bitmap-only glyph has no outline to make a distance field of");
    }
    apply_synthetic_style(face.desc, slot);
    out.verbs.clear();
    out.points.clear();
    out.advance = from_26_6(slot->advance.x);
    FT_Outline_Funcs functions{};
    functions.move_to = &move_to;
    functions.line_to = &line_to;
    functions.conic_to = &conic_to;
    functions.cubic_to = &cubic_to;
    Decomposer decomposer;
    decomposer.outline = &out;
    if (FT_Outline_Decompose(&slot->outline, &functions, &decomposer) != 0) {
        return fail(ErrorCode::InvalidArgument, "FreeType could not decompose the outline");
    }
    return decomposer.status;
}

}  // namespace cy::text::detail
