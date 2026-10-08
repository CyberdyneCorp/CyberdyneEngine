// SPDX-License-Identifier: MIT
#ifndef CY_BACKENDS_TEXT_COMPLETE_BACKEND_H
#define CY_BACKENDS_TEXT_COMPLETE_BACKEND_H
// The complete text backend: FreeType, HarfBuzz, msdfgen and ICU behind `cy::text::TextBackend`.
// M11.e, issue #86.
//
// `text-and-fonts` names the libraries in its normative text — "shaped through HarfBuzz", "per the
// Unicode Bidirectional Algorithm via ICU" — and requires that none of their types appear outside
// the backend. This header is the whole of what the rest of the engine sees of them: a class that
// implements the layer-2 interface, and a constructor. The four libraries are named only in the
// four translation units under src/, one each, and tools/layercheck/layercheck.py fails the build
// if any other file includes one of their headers.
//
// --- USE
// --------------------------------------------------------------------------------------------
//
//     cy::text::CompleteTextBackend backend;
//     if (Status started = backend.start(); !started) { ... }
//     cy::text::TextServer server;
//     server.start_with(config, backend);                       // the backend outlives the server
//     auto face = server.create_face(desc, FontSource{bytes});  // or create_face(cooked_font)
//
// What it can do is `capabilities()`; what it cannot — WOFF2, CBDT and sbix bitmaps, LCD subpixel
// rendering, dictionary line breaking, vertical layout — is false there and refused with a message
// naming the reason, and src/backends/text-complete/README.md says why each is out.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/backend.h>

namespace cy::text {

struct CompleteBackendState;

/// FreeType faces and rasters, msdfgen distance fields, HarfBuzz shaping and ICU's bidirectional
/// algorithm, behind the text server's backend interface.
///
/// Not thread-safe, like the server it serves. One per server; two servers on two threads take two
/// backends, which is what FreeType's own threading rule (one `FT_Library` per thread) requires.
class CompleteTextBackend final : public TextBackend {
public:
    /// Every allocation the backend makes for its own state and its faces comes from `allocator`.
    /// FreeType's and HarfBuzz's internal allocations use the C heap: their allocator hooks need
    /// the size of a block on release, which the C heap's interface does not carry and the
    /// engine's does, so the bridge would have to store it in a header per block.
    explicit CompleteTextBackend(Allocator& allocator = current_allocator()) noexcept;
    ~CompleteTextBackend() override;

    CompleteTextBackend(const CompleteTextBackend&) = delete;
    CompleteTextBackend& operator=(const CompleteTextBackend&) = delete;
    CompleteTextBackend(CompleteTextBackend&&) = delete;
    CompleteTextBackend& operator=(CompleteTextBackend&&) = delete;

    /// Initialise FreeType. Every other call fails with `Unavailable` until this has succeeded.
    [[nodiscard]] Status start() noexcept;
    /// Close every face and FreeType itself. Safe to call twice; the destructor calls it.
    void stop() noexcept;
    /// Whether `start` has succeeded and `stop` has not been called since.
    [[nodiscard]] bool is_running() const noexcept { return state_ != nullptr; }

    /// "complete".
    [[nodiscard]] const char* name() const noexcept override { return "complete"; }
    /// Complex shaping, bidirectional layout, colour glyphs, variable fonts and signed distance
    /// fields; not dictionary breaking, subpixel positioning, vertical layout or kashida.
    [[nodiscard]] TextCapabilities capabilities() const noexcept override;

    /// Open a face: FreeType for the outlines and the raster, HarfBuzz over the same bytes for
    /// shaping, both set to the same size, variable instance and synthetic style.
    [[nodiscard]] Expected<BackendFace, Error> open_face(
        const FontDesc& desc, const FontSource& source) noexcept override;
    /// Close a face. An unknown or already-closed face is ignored.
    void close_face(BackendFace face) noexcept override;
    /// The face's vertical metrics and space advance at its size.
    [[nodiscard]] FontMetrics face_metrics(BackendFace face) const noexcept override;
    /// The character map's glyph, or `kNotdef`.
    [[nodiscard]] GlyphIndex glyph_for(BackendFace face,
                                       Codepoint codepoint) const noexcept override;
    /// A grayscale or monochrome raster from FreeType, a colour one from COLR layers, or a
    /// multi-channel distance field from msdfgen, per the face's render mode.
    [[nodiscard]] Status rasterise(BackendFace face, GlyphIndex glyph,
                                   GlyphRaster& out) noexcept override;
    /// HarfBuzz's shaping of one run, in visual order.
    [[nodiscard]] Status shape(BackendFace face, const ShapeRequest& request,
                               Array<BackendGlyph>& out) noexcept override;
    /// HarfBuzz's GSUB closure over `codepoints`.
    [[nodiscard]] Status glyph_closure(BackendFace face, Span<const Codepoint> codepoints,
                                       Array<GlyphIndex>& out) noexcept override;
    /// ICU's bidirectional algorithm when CY_TEXT_ICU compiled it in; otherwise `Unsupported`, and
    /// the server falls back to src/text/'s.
    [[nodiscard]] Status resolve_bidi(std::string_view text, ParagraphDirection direction,
                                      BidiResult& out) noexcept override;

private:
    Allocator* allocator_ = nullptr;
    CompleteBackendState* state_ = nullptr;
};

}  // namespace cy::text

#endif  // CY_BACKENDS_TEXT_COMPLETE_BACKEND_H
