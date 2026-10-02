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
// --- ONE LINE, LEFT TO RIGHT, AND WHY THAT IS ENOUGH FOR NOW -------------------------------------
//
// A label here is one line laid out with `TextServer::layout_line`, left-aligned at the top-left of
// its element and drawn at a whole-number pixel scale with point sampling, which keeps a bitmap
// font crisp. Wrapping, alignment, carets and selection are the text server's and arrive with the
// widget set; nothing here would have to change shape for them.
//
// --- THE ATLAS IS WARMED, NOT GROWN MID-FRAME ----------------------------------------------------
//
// A glyph primitive's uv is normalised against the atlas extent at the moment it is painted. If
// the atlas grew or repacked between two glyphs of one frame, the first one's uv would point at the
// wrong texels. `start()` therefore rasterises the whole of the face's range up front — ninety-five
// glyphs of the built-in font fit the initial atlas many times over — and painting never inserts.
// `atlas_revision()` changes whenever the pixels do, which is when a renderer uploads them again.

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
    /// font's pixels whole on screen.
    u32 pixel_scale = 1;
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
    [[nodiscard]] bool is_running() const noexcept { return server_ != nullptr; }

    /// Attach, or replace, an element's text. The text is copied. Marks nothing dirty: the caller
    /// decides whether the change is a repaint or a relayout, as it does for every paint input.
    [[nodiscard]] Status set_text(ElementId element, std::string_view text,
                                  const TextStyle& style = {}) noexcept;
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

    [[nodiscard]] u16 atlas_page() const noexcept { return atlas_page_; }
    /// The atlas's coverage bytes, `atlas_extent()` square, top row first.
    [[nodiscard]] Span<const u8> atlas_pixels() const noexcept;
    [[nodiscard]] u32 atlas_extent() const noexcept;
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
    };

    [[nodiscard]] Entry* find(ElementId element) noexcept;
    [[nodiscard]] const Entry* find(ElementId element) const noexcept;
    [[nodiscard]] Status warm(const cy::text::ImageGridFont& font) noexcept;
    /// Drop the characters no entry points at, once they outweigh the live ones.
    [[nodiscard]] Status compact() noexcept;

    Allocator* allocator_ = nullptr;
    cy::text::TextServer* server_ = nullptr;
    cy::text::FallbackChain chain_;
    f32 line_height_ = 0.0F;
    u16 atlas_page_ = 0;
    u32 atlas_revision_ = 0;
    Array<Entry> entries_;
    Array<char> characters_;
    u32 dead_characters_ = 0;
};

}  // namespace cy::ui

#endif  // CY_UI_TEXT_TEXT_PAINTER_H
