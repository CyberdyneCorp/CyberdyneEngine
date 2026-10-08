#ifndef CY_SERVERS_TEXT_BACKEND_H
#define CY_SERVERS_TEXT_BACKEND_H
// `TextBackend` — the seam the outline-font libraries sit beneath. M11.e, issue #86.
//
// `text-and-fonts`: "no HarfBuzz, ICU, or FreeType type SHALL appear outside the backend". The
// server at layer 2 owns faces, atlases, the shaping cache and layout; what it cannot do itself is
// read an OpenType file, rasterise an outline, run GSUB and GPOS, or resolve the Unicode
// Bidirectional Algorithm in full. Those four are this interface, and src/backends/text-complete/
// implements it over FreeType, msdfgen, HarfBuzz and ICU at layer 3 — the arrangement
// `cy::audio::AudioBackend` and miniaudio have, for the same reason.
//
// --- WHAT CROSSES THIS INTERFACE ----------------------------------------------------------------
//
// Only engine types. A face is a `BackendFace` number the backend chose; a glyph is a
// `GlyphIndex` and a raster in one of three `PixelFormat`s; a shaped run is an array of glyph
// indices with clusters and positions in pixels, Y DOWN like everything else in this module. The
// backend converts HarfBuzz's 26.6 fixed point with Y up, FreeType's bitmaps with their pitch, and
// ICU's UTF-16 offsets before anything is returned — so a second backend (a platform shaper, or a
// size-constrained one) is a second implementation of this header and nothing above it changes.
//
// --- WHAT THE SERVER STILL DOES ITSELF -----------------------------------------------------------
//
// The fallback chain, the atlas, the shaping cache, line breaking and the paragraph are the
// server's. A backend shapes ONE run in ONE face in ONE direction; the server splits text into
// those runs — by face, where the chain falls back, and by bidirectional level — and lays the
// shaped runs out in visual order. That keeps every decision a caller can observe in one place,
// with one set of tests, whichever backend produced the glyphs.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/font.h>
#include <cy/servers/text/text.h>
#include <cy/text/bidi.h>

#include <string_view>

namespace cy::text {

/// A face as the backend knows it. Chosen by the backend and meaningful only to it; the server maps
/// its own `FontHandle` onto one.
using BackendFace = u32;

/// One rasterised glyph, in the format the rasteriser produced.
///
/// `metrics` is in the face's pixels with the bearing convention font.h fixes (Y down from the
/// baseline). `pixels` is `width * height * bytes_per_pixel(format)` bytes, top row first and
/// tightly packed: the backend removes any row pitch its library used.
struct GlyphRaster {
    GlyphMetrics metrics;
    PixelFormat format = PixelFormat::Coverage;
    Array<u8> pixels;
};

/// One glyph of a shaped run, as the backend produced it.
///
/// Positions are in pixels. `offset` is where the glyph sits relative to the pen, Y DOWN;
/// `advance` is how far the pen moves after it. `cluster` is the byte offset, within the text the
/// backend was given, of the first character the glyph came from — several glyphs share one when a
/// character decomposes, and one glyph covers several characters when they ligate.
struct BackendGlyph {
    GlyphIndex glyph = kNotdef;
    u32 cluster = 0;
    Vec2 advance{0.0f, 0.0f};
    Vec2 offset{0.0f, 0.0f};
};

/// One run to shape: one face, one direction, one script.
struct ShapeRequest {
    /// UTF-8. The whole run; the backend sees no text beyond it, so contextual forms across a face
    /// or direction change are not joined — which is also what every shaping engine does.
    std::string_view text;
    Direction direction = Direction::LeftToRight;
    /// A BCP 47 tag, or empty. Selects language-specific forms (`locl`), as Serbian and Russian
    /// Cyrillic differ.
    std::string_view language;
    /// Feature overrides on top of the face's own (`FontDesc::features`) and the font's defaults.
    Span<const FontFeature> features;
};

/// Fonts, rasterisation, shaping and the bidirectional algorithm, beneath the text server.
///
/// Not thread-safe, like the server it serves. Every call is made from the thread that owns the
/// server.
class TextBackend {
public:
    TextBackend() = default;
    virtual ~TextBackend() = default;

    TextBackend(const TextBackend&) = delete;
    TextBackend& operator=(const TextBackend&) = delete;
    TextBackend(TextBackend&&) = delete;
    TextBackend& operator=(TextBackend&&) = delete;

    /// "complete", or another backend's name. Reported in `TextCapabilities::backend`.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// What this backend can do. The server reports it as its own, so it must be true of every face
    /// this backend opens.
    [[nodiscard]] virtual TextCapabilities capabilities() const noexcept = 0;

    // --- Faces -----------------------------------------------------------------------------------

    /// Open a face of `source` at the size, axes, hinting, synthetic style and render mode `desc`
    /// names. The source's bytes are held by reference and must outlive the face.
    [[nodiscard]] virtual Expected<BackendFace, Error> open_face(
        const FontDesc& desc, const FontSource& source) noexcept = 0;
    virtual void close_face(BackendFace face) noexcept = 0;

    [[nodiscard]] virtual FontMetrics face_metrics(BackendFace face) const noexcept = 0;

    /// The font's glyph for a codepoint, from its character map, or `kNotdef`.
    [[nodiscard]] virtual GlyphIndex glyph_for(BackendFace face,
                                               Codepoint codepoint) const noexcept = 0;

    // --- Rasterisation ---------------------------------------------------------------------------

    /// Rasterise one glyph in the face's render mode. A glyph with colour layers comes back as
    /// `PixelFormat::Colour` whatever the mode, when the backend's `colour_glyphs` is true.
    [[nodiscard]] virtual Status rasterise(BackendFace face, GlyphIndex glyph,
                                           GlyphRaster& out) noexcept = 0;

    // --- Shaping ---------------------------------------------------------------------------------

    /// Shape one run. `out` is cleared first and holds the glyphs in VISUAL order — left to right
    /// on the page — whatever the run's direction, which is the order the server places them in.
    [[nodiscard]] virtual Status shape(BackendFace face, const ShapeRequest& request,
                                       Array<BackendGlyph>& out) noexcept = 0;

    /// Every glyph shaping can produce from `codepoints` — the character map's, and those GSUB
    /// reaches from them: a ligature, a contextual or positional form, a localised variant. What a
    /// font importer pre-renders so that shaping a cooked range never rasterises at runtime.
    ///
    /// Optional: the default answers `Unsupported`, and a cook then pre-renders the character map's
    /// glyphs alone. `out` is cleared first and holds each glyph once, in ascending order.
    [[nodiscard]] virtual Status glyph_closure(BackendFace face, Span<const Codepoint> codepoints,
                                               Array<GlyphIndex>& out) noexcept;

    // --- The bidirectional algorithm -------------------------------------------------------------

    /// Resolve embedding levels for one paragraph of UTF-8 into runs of byte offsets.
    ///
    /// Optional: the default answers `Unsupported`, and the server then resolves the paragraph with
    /// the in-tree algorithm in src/text/, whose `BidiResult::approximated` says when an isolate
    /// made its answer approximate. A backend that implements this must produce the runs in
    /// LOGICAL order, as `cy::text::resolve_levels` does.
    [[nodiscard]] virtual Status resolve_bidi(std::string_view text, ParagraphDirection direction,
                                              BidiResult& out) noexcept;
};

}  // namespace cy::text

#endif  // CY_SERVERS_TEXT_BACKEND_H
