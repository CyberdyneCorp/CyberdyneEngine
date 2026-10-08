// SPDX-License-Identifier: MIT
#ifndef CY_BACKENDS_TEXT_COMPLETE_INTERNAL_H
#define CY_BACKENDS_TEXT_COMPLETE_INTERNAL_H
// The seams between the four translation units of the complete text backend.
//
// Each library is named by exactly ONE file — FreeType by freetype_faces.cpp, HarfBuzz by
// harfbuzz_shaper.cpp, msdfgen by msdf_generator.cpp and ICU by icu_bidi.cpp — and every type that
// crosses between them is declared here in engine terms. A glyph outline travels from FreeType to
// msdfgen as a `GlyphOutline` of verbs and points, not as an `FT_Outline`, which is what lets the
// distance-field generator be replaced without touching the rasteriser and the other way round.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/backend.h>

#include <new>
#include <utility>

namespace cy::text::detail {

/// Construct a `T` in memory from `allocator`, or return null when it has none.
template <typename T, typename... Arguments>
[[nodiscard]] T* create(Allocator& allocator, Arguments&&... arguments) noexcept {
    void* memory = allocator.allocate(sizeof(T), alignof(T));
    if (memory == nullptr) {
        return nullptr;
    }
    return ::new (memory) T(std::forward<Arguments>(arguments)...);
}

/// Destroy what `create` made. Null is ignored.
template <typename T>
void destroy(Allocator& allocator, T* object) noexcept {
    if (object != nullptr) {
        object->~T();
        allocator.deallocate(object, sizeof(T), alignof(T));
    }
}

// --- FreeType: faces, metrics, coverage and colour rasters, outlines
// --------------------------------

struct FreeTypeLibrary;
struct FreeTypeFace;

[[nodiscard]] Expected<FreeTypeLibrary*, Error> freetype_open_library(
    Allocator& allocator) noexcept;
void freetype_close_library(Allocator& allocator, FreeTypeLibrary* library) noexcept;

[[nodiscard]] Expected<FreeTypeFace*, Error> freetype_open_face(Allocator& allocator,
                                                                FreeTypeLibrary& library,
                                                                const FontDesc& desc,
                                                                const FontSource& source) noexcept;
void freetype_close_face(Allocator& allocator, FreeTypeFace* face) noexcept;

[[nodiscard]] FontMetrics freetype_metrics(const FreeTypeFace& face) noexcept;
[[nodiscard]] GlyphIndex freetype_glyph_for(const FreeTypeFace& face, Codepoint codepoint) noexcept;
/// The face's whole sfnt, decompressed: for a WOFF file, the TrueType or OpenType font inside it.
/// HarfBuzz reads sfnt tables and not WOFF's compressed ones, so a WOFF face is shaped from this.
[[nodiscard]] Status freetype_sfnt(const FreeTypeFace& face, Array<u8>& out) noexcept;
/// Whether the face carries COLR colour layers.
[[nodiscard]] bool freetype_has_colour(const FreeTypeFace& face) noexcept;

/// A grayscale, monochrome or colour raster of one glyph, in the face's render mode.
[[nodiscard]] Status freetype_rasterise(FreeTypeFace& face, GlyphIndex glyph,
                                        GlyphRaster& out) noexcept;

/// One glyph's outline in pixels at the face's size, Y UP from the baseline, with the synthetic
/// styles applied. Quadratic and cubic segments are kept as they are: the distance field is exact
/// to the curve rather than to a flattening of it.
struct GlyphOutline {
    enum class Verb : u8 { Move = 0, Line = 1, Quadratic = 2, Cubic = 3 };
    struct Point {
        f64 x = 0.0;
        f64 y = 0.0;
    };
    Array<Verb> verbs;
    /// Move and Line consume one point, Quadratic two, Cubic three.
    Array<Point> points;
    /// The pen advance, in pixels.
    f32 advance = 0.0f;
};

[[nodiscard]] Status freetype_outline(FreeTypeFace& face, GlyphIndex glyph,
                                      GlyphOutline& out) noexcept;

// --- msdfgen: the distance field ----------------------------------------------------------------

/// A multi-channel signed distance field of `outline` with the true distance in alpha, padded by
/// `range` pixels on every side and mapped so 0.5 is the edge and `range` pixels either side span
/// [0, 1]. An outline with no contours — a space — produces an empty raster with the advance.
[[nodiscard]] Status msdf_generate(const GlyphOutline& outline, f32 range,
                                   GlyphRaster& out) noexcept;

// --- HarfBuzz: shaping and the glyph closure
// ------------------------------------------------------

struct HarfBuzzFace;

[[nodiscard]] Expected<HarfBuzzFace*, Error> harfbuzz_open_face(Allocator& allocator,
                                                                const FontDesc& desc,
                                                                const FontSource& source) noexcept;
void harfbuzz_close_face(Allocator& allocator, HarfBuzzFace* face) noexcept;

[[nodiscard]] Status harfbuzz_shape(HarfBuzzFace& face, const ShapeRequest& request,
                                    Array<BackendGlyph>& out) noexcept;
[[nodiscard]] Status harfbuzz_closure(HarfBuzzFace& face, Span<const Codepoint> codepoints,
                                      Array<GlyphIndex>& out) noexcept;
/// The face's horizontal advance for a glyph, in pixels, with the variation applied.
[[nodiscard]] f32 harfbuzz_advance(HarfBuzzFace& face, GlyphIndex glyph) noexcept;

// --- ICU: the bidirectional algorithm ------------------------------------------------------------

/// Whether this build compiled ICU in (CY_TEXT_ICU).
[[nodiscard]] bool icu_available() noexcept;
[[nodiscard]] Status icu_resolve_bidi(std::string_view text, ParagraphDirection direction,
                                      BidiResult& out) noexcept;

}  // namespace cy::text::detail

#endif  // CY_BACKENDS_TEXT_COMPLETE_INTERNAL_H
