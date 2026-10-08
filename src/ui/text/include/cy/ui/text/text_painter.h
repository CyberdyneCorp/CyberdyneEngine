// SPDX-License-Identifier: MIT
#ifndef CY_UI_TEXT_TEXT_PAINTER_H
#define CY_UI_TEXT_TEXT_PAINTER_H
// An element's text, measured for layout and painted into the primitive stream.
//
// --- WHERE TEXT ENTERS CYBERUI -------------------------------------------------------------------
//
// `cy::ui` measures content through `ContentMeasurer` and paints it through `ContentPainter`, and
// links no font server: both are interfaces a caller implements, which is what keeps `unit.ui`
// free of fonts. `TextPainter` is the implementation over `cy::text::TextServer`. A label is an
// element with text attached here; layout asks this class how big the text is, and `flatten()` asks
// it for the glyphs, which come out as `BuiltinMaterial::Glyph` primitives sampling the text
// server's glyph atlas — the same stream, the same batches and the same shader as every panel.
//
// --- TWO KINDS OF FACE --------------------------------------------------------------------------
//
// `start` with an `ImageGridFont` is #102's path: the built-in bitmap font, one line left to right,
// drawn at a whole-number pixel scale with point sampling, which keeps a bitmap font crisp.
//
// `start` with a `FontSource` is the complete backend's (issue #86): an outline face — CyberUI's is
// interface_font.h's Noto Sans — shaped by HarfBuzz, laid out bidirectionally, and drawn from
// whichever atlas each glyph landed in: a distance field (`BuiltinMaterial::GlyphField`, sharp at
// any `TextStyle::size`, with an outline and a shadow from the same entry), coverage (`Glyph`) or
// colour (`Image`). The built-in font is pushed behind it in the fallback chain.
//
// A label is one line, laid out with `TextServer::layout_line`, left-aligned at the top-left of its
// element. Wrapping, alignment, carets and selection are the text server's and arrive with the
// widget set; nothing here would have to change shape for them.
//
// --- THE ATLAS IS WARMED, NOT GROWN MID-FRAME ----------------------------------------------------
//
// A glyph primitive's uv is normalised against the atlas extent at the moment it is painted. If an
// atlas grew or repacked between two glyphs of one frame, the first one's uv would point at the
// wrong texels. So glyphs are made resident BEFORE a frame paints: `start()` rasterises the grid
// font's whole range, or an outline face's printable ASCII; and `set_text` makes every glyph of the
// new text resident as it is set. Painting then only finds. `atlas_revision()` changes whenever any
// page's pixels do, which is when a renderer uploads them again.
//
// --- THE PAGES
// -------------------------------------------------------------------------------------
//
// One per `cy::text::PixelFormat`, numbered from the page `start` is given: coverage, then the
// distance field, then colour. A grid face uses only the first. A renderer uploads each that
// `atlas_extent(format)` says exists: coverage as one byte a texel, the other two as four.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/server.h>
#include <cy/ui/layout.h>
#include <cy/ui/paint.h>

#include <string_view>

namespace cy::ui {

/// How a label's text is drawn.
struct TextStyle {
    /// Premultiplied ARGB, as every interface colour is. The element's opacity is folded in by
    /// `flatten()`, not here.
    u32 colour = 0xFFFFFFFFU;
    /// Each font pixel becomes this many reference units square. Whole numbers keep a bitmap
    /// font's pixels whole on screen. Used when `size` is zero.
    u32 pixel_scale = 1;
    /// The text's size in reference units: the face is drawn at `size / face size`. Zero draws it
    /// at its own size times `pixel_scale`. A distance-field face is sharp at any size; a coverage
    /// or grid face is sharp only at whole multiples of its own.
    f32 size = 0.0F;
    /// An outline around a distance-field glyph, this many reference units wide, in this
    /// premultiplied colour. Zero draws none; a coverage or grid glyph has no field to draw one
    /// from and ignores it.
    f32 outline_width = 0.0F;
    u32 outline_colour = 0;
    /// A drop shadow: every glyph drawn again beneath, `shadow_offset` reference units away, in
    /// this premultiplied colour. A zero colour draws none.
    u32 shadow_colour = 0;
    Vec2 shadow_offset{0.0F, 0.0F};
    /// A horizontal gradient across the line: `colour` at its start, this at its end, each glyph
    /// the blend at its centre. Zero draws none.
    u32 gradient_colour = 0;
};

/// One colour over a range of a label's text — per-character colour. `begin` and `end` are byte
/// offsets into the text; a glyph takes the colour of the last span its source offset falls in,
/// over the style's colour and gradient.
struct ColourSpan {
    u32 begin = 0;
    u32 end = 0;
    u32 colour = 0xFFFFFFFFU;
};

class TextPainter final : public ContentPainter, public ContentMeasurer {
public:
    explicit TextPainter(Allocator& allocator) noexcept;
    ~TextPainter() override = default;

    TextPainter(const TextPainter&) = delete;
    TextPainter& operator=(const TextPainter&) = delete;
    TextPainter(TextPainter&&) = delete;
    TextPainter& operator=(TextPainter&&) = delete;

    /// Make a face of `font` on a running `server`, and rasterise its whole range into the
    /// server's atlas. `atlas_page` is the page number every glyph primitive carries — the atlas
    /// slot a renderer binds this atlas's pixels to. The server and the font must outlive this.
    [[nodiscard]] Status start(cy::text::TextServer& server, const cy::text::ImageGridFont& font,
                               u16 atlas_page) noexcept;

    /// Make an outline face of `source` as `desc` describes on a server started with a backend,
    /// with the built-in font behind it in the chain, and warm printable ASCII. Pages are
    /// `first_page` (coverage), `first_page + 1` (distance field) and `first_page + 2` (colour).
    /// Fails, leaving the painter unstarted, on a server with no outline support — the caller then
    /// starts on the built-in font instead. The server and the bytes must outlive this.
    [[nodiscard]] Status start(cy::text::TextServer& server, const cy::text::FontSource& source,
                               const cy::text::FontDesc& desc, u16 first_page) noexcept;
    [[nodiscard]] bool is_running() const noexcept { return server_ != nullptr; }

    /// Attach, or replace, an element's text. The text is copied. Marks nothing dirty: the caller
    /// decides whether the change is a repaint or a relayout, as it does for every paint input.
    [[nodiscard]] Status set_text(ElementId element, std::string_view text,
                                  const TextStyle& style = {}) noexcept;
    /// Colour ranges of an element's text, replacing any it had; empty clears them. Copied. Fails
    /// for an element with no text.
    [[nodiscard]] Status set_colours(ElementId element, Span<const ColourSpan> spans) noexcept;
    /// Detach an element's text. A no-op for an element that has none.
    void clear_text(ElementId element) noexcept;
    /// The text attached to an element, or empty.
    [[nodiscard]] std::string_view text_of(ElementId element) const noexcept;

    /// The size of `text` drawn in `style`, in reference units: the line's advance by the face's
    /// line height, each multiplied by the pixel scale.
    [[nodiscard]] Vec2 measure(std::string_view text, const TextStyle& style) noexcept;

    // --- ContentMeasurer and ContentPainter ----------------------------------------------------

    [[nodiscard]] Vec2 measure_content(ElementId element, Vec2 available) noexcept override;
    [[nodiscard]] Status paint_content(ElementId element, const Rect& rect,
                                       Array<Primitive>& out) noexcept override;

    // --- The atlas a renderer uploads ----------------------------------------------------------

    /// The coverage page's number: the first of the three.
    [[nodiscard]] u16 atlas_page() const noexcept { return atlas_page_; }
    /// The page number primitives sampling `format` carry.
    [[nodiscard]] u16 atlas_page(cy::text::PixelFormat format) const noexcept {
        return static_cast<u16>(atlas_page_ + static_cast<u16>(format));
    }
    /// The coverage atlas's bytes, `atlas_extent()` square, top row first.
    [[nodiscard]] Span<const u8> atlas_pixels() const noexcept;
    [[nodiscard]] u32 atlas_extent() const noexcept;
    /// One page's bytes, `atlas_extent(format)` square times `bytes_per_pixel(format)`, or empty
    /// when no glyph of that format has been placed.
    [[nodiscard]] Span<const u8> atlas_pixels(cy::text::PixelFormat format) const noexcept;
    /// One page's extent, or zero when it does not exist yet.
    [[nodiscard]] u32 atlas_extent(cy::text::PixelFormat format) const noexcept;
    /// Whether this painter draws an outline face. False is the built-in grid font.
    [[nodiscard]] bool outline() const noexcept { return outline_; }
    /// Bumped whenever the atlas's pixels may have changed. A renderer re-uploads when it differs
    /// from the revision it last uploaded.
    [[nodiscard]] u32 atlas_revision() const noexcept { return atlas_revision_; }

private:
    struct Entry {
        ElementId element;
        /// Into `characters_`.
        u32 offset = 0;
        u32 length = 0;
        TextStyle style;
        /// Into `spans_`.
        u32 first_span = 0;
        u32 span_count = 0;
    };

    /// What one glyph of a line becomes: its slot, where it goes, and its colour.
    struct Placed {
        const cy::text::GlyphSlot* slot = nullptr;
        Rect bounds;
        u32 colour = 0;
    };

    [[nodiscard]] Entry* find(ElementId element) noexcept;
    [[nodiscard]] const Entry* find(ElementId element) const noexcept;
    [[nodiscard]] Status warm(const cy::text::ImageGridFont& font) noexcept;
    /// Make every glyph `text` shapes into resident, so painting it finds them.
    [[nodiscard]] Status make_resident(std::string_view text) noexcept;
    /// Bump `atlas_revision_` when any page changed since the last call.
    void note_atlas_changes() noexcept;
    [[nodiscard]] Status bind(cy::text::TextServer& server, cy::text::FontHandle face,
                              u16 atlas_page) noexcept;
    [[nodiscard]] f32 scale_of(const TextStyle& style) const noexcept;
    [[nodiscard]] u32 colour_at(const Entry& entry, u32 source_offset, f32 along) const noexcept;
    [[nodiscard]] Primitive primitive_of(const Placed& placed, const TextStyle& style,
                                         f32 scale) const noexcept;
    /// Drop the characters no entry points at, once they outweigh the live ones.
    [[nodiscard]] Status compact() noexcept;

    Allocator* allocator_ = nullptr;
    cy::text::TextServer* server_ = nullptr;
    cy::text::FallbackChain chain_;
    f32 line_height_ = 0.0F;
    /// The primary face's size in pixels: what `TextStyle::size` is relative to.
    f32 face_size_ = 0.0F;
    /// The distance-field range the primary face was made with, in atlas pixels.
    f32 distance_range_ = 0.0F;
    bool outline_ = false;
    u16 atlas_page_ = 0;
    u32 atlas_revision_ = 0;
    Array<Entry> entries_;
    Array<char> characters_;
    Array<ColourSpan> spans_;
    u32 dead_characters_ = 0;
};

}  // namespace cy::ui

#endif  // CY_UI_TEXT_TEXT_PAINTER_H
