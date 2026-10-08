// SPDX-License-Identifier: MIT
#include <cy/ui/text/text_painter.h>

#include <cy/servers/text/layout.h>
#include <cy/ui/text/builtin_font.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::ui {
namespace {

using cy::text::Codepoint;
using cy::text::PixelFormat;

/// Printable ASCII: what an outline face warms at start, so the common case of an interface's text
/// never rasterises during a frame.
constexpr Codepoint kWarmFirst = 0x20;
constexpr Codepoint kWarmLast = 0x7E;

[[nodiscard]] u16 material_of(const cy::text::GlyphSlot& slot) noexcept {
    switch (static_cast<PixelFormat>(slot.page)) {
        case PixelFormat::DistanceField:
            return material_index(BuiltinMaterial::GlyphField);
        case PixelFormat::Colour:
            return material_index(BuiltinMaterial::Image);
        case PixelFormat::Coverage:
            break;
    }
    return material_index(BuiltinMaterial::Glyph);
}

/// Each channel of two premultiplied colours, blended `t` of the way from the first to the second.
[[nodiscard]] u32 blend(u32 from, u32 to, f32 t) noexcept {
    u32 out = 0;
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        const auto a = static_cast<f32>((from >> shift) & 0xFFU);
        const auto b = static_cast<f32>((to >> shift) & 0xFFU);
        const auto mixed = static_cast<u32>(std::lround(a + ((b - a) * t)));
        out |= std::min(mixed, 255U) << shift;
    }
    return out;
}

}  // namespace

TextPainter::TextPainter(Allocator& allocator) noexcept
    : allocator_(&allocator), entries_(allocator), characters_(allocator), spans_(allocator) {}

Status TextPainter::bind(cy::text::TextServer& server, cy::text::FontHandle face,
                         u16 atlas_page) noexcept {
    Expected<cy::text::FontMetrics, Error> metrics = server.face_metrics(face);
    if (!metrics.has_value()) {
        return make_unexpected(metrics.error());
    }
    chain_ = cy::text::FallbackChain{};
    if (Status pushed = chain_.push(face); !pushed) {
        return pushed;
    }
    server_ = &server;
    line_height_ = metrics->line_height();
    atlas_page_ = atlas_page;
    return ok();
}

Status TextPainter::start(cy::text::TextServer& server, const cy::text::ImageGridFont& font,
                          u16 atlas_page) noexcept {
    if (server_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "text painter: already started");
    }
    if (!server.is_running()) {
        return fail(ErrorCode::InvalidArgument, "text painter: the text server is not running");
    }
    cy::text::FontDesc desc;
    desc.family = "cyberui";
    desc.size_pixels = static_cast<f32>(font.cell_height);
    Expected<cy::text::FontHandle, Error> face = server.create_face(desc, font);
    if (!face.has_value()) {
        return make_unexpected(face.error());
    }
    if (Status bound = bind(server, *face, atlas_page); !bound) {
        return bound;
    }
    face_size_ = desc.size_pixels;
    outline_ = false;
    if (Status warmed = warm(font); !warmed) {
        server_ = nullptr;
        return warmed;
    }
    return ok();
}

Status TextPainter::start(cy::text::TextServer& server, const cy::text::FontSource& source,
                          const cy::text::FontDesc& desc, u16 first_page) noexcept {
    if (server_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "text painter: already started");
    }
    if (!server.is_running()) {
        return fail(ErrorCode::InvalidArgument, "text painter: the text server is not running");
    }
    Expected<cy::text::FontHandle, Error> face = server.create_face(desc, source);
    if (!face.has_value()) {
        return make_unexpected(face.error());
    }
    // The built-in font behind it: a codepoint the outline face lacks draws as a terminal glyph, or
    // as the grid's visible box, rather than as nothing.
    cy::text::FontDesc grid_desc;
    grid_desc.family = "cyberui";
    grid_desc.size_pixels = static_cast<f32>(builtin_font().cell_height);
    Expected<cy::text::FontHandle, Error> grid = server.create_face(grid_desc, builtin_font());
    Status status =
        grid.has_value() ? bind(server, *face, first_page) : Status(make_unexpected(grid.error()));
    status = status ? chain_.push(*grid) : status;
    if (!status) {
        server_ = nullptr;
        server.destroy_face(*face);
        return status;
    }
    face_size_ = desc.size_pixels;
    distance_range_ =
        desc.mode == cy::text::RenderMode::SignedDistanceField ? desc.distance_range : 0.0F;
    outline_ = true;
    char ascii[kWarmLast - kWarmFirst + 1] = {};
    for (Codepoint codepoint = kWarmFirst; codepoint <= kWarmLast; ++codepoint) {
        ascii[codepoint - kWarmFirst] = static_cast<char>(codepoint);
    }
    if (Status warmed = make_resident(std::string_view(ascii, sizeof(ascii))); !warmed) {
        server_ = nullptr;
        return warmed;
    }
    return ok();
}

Status TextPainter::warm(const cy::text::ImageGridFont& font) noexcept {
    // EVERY GLYPH OF THE RANGE, AND `.notdef`, NOW — see the header: painting must never grow the
    // atlas under a frame's earlier uvs.
    const cy::text::FontHandle face = chain_.faces[0];
    if (Expected<const cy::text::GlyphSlot*, Error> notdef =
            server_->glyph_slot(face, cy::text::kNotdef);
        !notdef.has_value()) {
        return make_unexpected(notdef.error());
    }
    for (u32 index = 0; index < font.glyph_count; ++index) {
        const cy::text::GlyphIndex glyph = server_->glyph_for(face, font.first_codepoint + index);
        if (Expected<const cy::text::GlyphSlot*, Error> slot = server_->glyph_slot(face, glyph);
            !slot.has_value()) {
            return make_unexpected(slot.error());
        }
    }
    note_atlas_changes();
    return ok();
}

Status TextPainter::make_resident(std::string_view text) noexcept {
    cy::text::TextLine line;
    if (Status laid = server_->layout_line(text, chain_, line); !laid) {
        return laid;
    }
    for (const cy::text::ShapedGlyph& glyph : line.run().glyphs) {
        if (Expected<const cy::text::GlyphSlot*, Error> slot =
                server_->glyph_slot(glyph.face, glyph.glyph);
            !slot.has_value()) {
            return make_unexpected(slot.error());
        }
    }
    note_atlas_changes();
    return ok();
}

void TextPainter::note_atlas_changes() noexcept {
    bool changed = false;
    for (u32 format = 0; format < cy::text::kPixelFormatCount; ++format) {
        cy::text::GlyphAtlas& atlas = server_->atlas(static_cast<PixelFormat>(format));
        if (atlas.is_running() && !atlas.dirty_region().is_empty()) {
            changed = true;
            atlas.clear_dirty();
        }
    }
    if (changed) {
        ++atlas_revision_;
    }
}

TextPainter::Entry* TextPainter::find(ElementId element) noexcept {
    for (Entry& entry : entries_) {
        if (entry.element == element) {
            return &entry;
        }
    }
    return nullptr;
}

const TextPainter::Entry* TextPainter::find(ElementId element) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.element == element) {
            return &entry;
        }
    }
    return nullptr;
}

Status TextPainter::set_text(ElementId element, std::string_view text,
                             const TextStyle& style) noexcept {
    if (!element.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "text painter: no element");
    }
    Entry* entry = find(element);
    if (entry == nullptr) {
        if (Status pushed = entries_.push_back(Entry{element, 0, 0, style, 0, 0}); !pushed) {
            return pushed;
        }
        entry = &entries_.back();
    }
    // Replaced text in place when it fits; otherwise appended and the old run counted as dead.
    if (text.size() <= entry->length) {
        for (usize index = 0; index < text.size(); ++index) {
            characters_[entry->offset + index] = text[index];
        }
        dead_characters_ += entry->length - static_cast<u32>(text.size());
    } else {
        dead_characters_ += entry->length;
        entry->offset = static_cast<u32>(characters_.size());
        if (Status appended = characters_.append(Span<const char>(text.data(), text.size()));
            !appended) {
            return appended;
        }
    }
    entry->length = static_cast<u32>(text.size());
    entry->style = style;
    entry->span_count = 0;
    if (outline_ && server_ != nullptr) {
        // An outline face is not warmed whole, so the glyphs this text needs are made resident
        // now, outside any frame — see the header.
        if (Status resident = make_resident(text); !resident) {
            return resident;
        }
    }
    return compact();
}

Status TextPainter::set_colours(ElementId element, Span<const ColourSpan> spans) noexcept {
    Entry* entry = find(element);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "text painter: colours for an element with no text");
    }
    // Appended rather than replaced in place: spans are few, and `compact` reclaims them with the
    // characters.
    entry->first_span = static_cast<u32>(spans_.size());
    entry->span_count = static_cast<u32>(spans.size());
    return spans_.append(spans);
}

void TextPainter::clear_text(ElementId element) noexcept {
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].element == element) {
            dead_characters_ += entries_[index].length;
            entries_.remove_unordered(index);
            return;
        }
    }
}

Status TextPainter::compact() noexcept {
    if (dead_characters_ <= characters_.size() / 2U) {
        return ok();
    }
    Array<char> live(*allocator_);
    Array<ColourSpan> live_spans(*allocator_);
    for (Entry& entry : entries_) {
        const u32 offset = static_cast<u32>(live.size());
        const u32 first_span = static_cast<u32>(live_spans.size());
        Status status =
            live.append(Span<const char>(characters_.data() + entry.offset, entry.length));
        status = status ? live_spans.append(Span<const ColourSpan>(spans_.data() + entry.first_span,
                                                                   entry.span_count))
                        : status;
        if (!status) {
            return status;
        }
        entry.offset = offset;
        entry.first_span = first_span;
    }
    characters_ = std::move(live);
    spans_ = std::move(live_spans);
    dead_characters_ = 0;
    return ok();
}

std::string_view TextPainter::text_of(ElementId element) const noexcept {
    const Entry* entry = find(element);
    if (entry == nullptr) {
        return {};
    }
    return {characters_.data() + entry->offset, entry->length};
}

f32 TextPainter::scale_of(const TextStyle& style) const noexcept {
    if (style.size > 0.0F && face_size_ > 0.0F) {
        return style.size / face_size_;
    }
    return static_cast<f32>(style.pixel_scale == 0U ? 1U : style.pixel_scale);
}

Vec2 TextPainter::measure(std::string_view text, const TextStyle& style) noexcept {
    if (server_ == nullptr || text.empty()) {
        return Vec2{0.0F, 0.0F};
    }
    Expected<Vec2, Error> size = server_->measure(text, chain_);
    if (!size.has_value()) {
        return Vec2{0.0F, 0.0F};
    }
    const f32 scale = scale_of(style);
    return Vec2{size->x * scale, line_height_ * scale};
}

Vec2 TextPainter::measure_content(ElementId element, Vec2 /*available*/) noexcept {
    const Entry* entry = find(element);
    if (entry == nullptr) {
        return Vec2{0.0F, 0.0F};
    }
    return measure(text_of(element), entry->style);
}

u32 TextPainter::colour_at(const Entry& entry, u32 source_offset, f32 along) const noexcept {
    u32 colour = entry.style.colour;
    if (entry.style.gradient_colour != 0U) {
        colour =
            blend(entry.style.colour, entry.style.gradient_colour, std::clamp(along, 0.0F, 1.0F));
    }
    for (u32 index = 0; index < entry.span_count; ++index) {
        const ColourSpan& span = spans_[entry.first_span + index];
        if (source_offset >= span.begin && source_offset < span.end) {
            colour = span.colour;
        }
    }
    return colour;
}

Primitive TextPainter::primitive_of(const Placed& placed, const TextStyle& style,
                                    f32 scale) const noexcept {
    const cy::text::GlyphSlot& slot = *placed.slot;
    const auto format = static_cast<PixelFormat>(slot.page);
    const auto extent = static_cast<f32>(server_->atlas(format).extent());
    Primitive primitive;
    primitive.bounds = placed.bounds;
    primitive.uv.x = static_cast<f32>(slot.rect.position.x) / extent;
    primitive.uv.y = static_cast<f32>(slot.rect.position.y) / extent;
    primitive.uv.width = static_cast<f32>(slot.metrics.width) / extent;
    primitive.uv.height = static_cast<f32>(slot.metrics.height) / extent;
    primitive.material = material_of(slot);
    primitive.atlas = atlas_page(format);
    primitive.colour = placed.colour;
    if (format == PixelFormat::DistanceField) {
        // The field spans twice the range in atlas pixels, and one atlas pixel is `scale`
        // reference units on the page.
        primitive.distance_range = 2.0F * distance_range_ * scale;
        primitive.border_width = style.outline_width;
        primitive.border_colour = style.outline_width > 0.0F ? style.outline_colour : 0U;
    }
    if (format == PixelFormat::Colour) {
        // A colour glyph is its own colour: only the label's alpha reaches it, as a premultiplied
        // white that the image material multiplies the glyph's texels by.
        primitive.colour =
            scale_premultiplied(0xFFFFFFFFU, static_cast<f32>(placed.colour >> 24U) / 255.0F);
    }
    return primitive;
}

Status TextPainter::paint_content(ElementId element, const Rect& rect,
                                  Array<Primitive>& out) noexcept {
    const Entry* entry = find(element);
    if (entry == nullptr || entry->length == 0 || server_ == nullptr) {
        return ok();
    }
    const std::string_view text = text_of(element);
    cy::text::TextLine line;
    if (Status laid = server_->layout_line(text, chain_, line); !laid) {
        return laid;
    }
    const f32 scale = scale_of(entry->style);
    const f32 width = std::max(line.width(), 1.0F);

    // Placed once, emitted twice when there is a shadow: every shadow beneath every glyph, so a
    // shadow never falls over the glyph before it — and both passes share a material and a page,
    // so the line is still one batch.
    Array<Placed> placed(*allocator_);
    for (const cy::text::ShapedGlyph& glyph : line.run().glyphs) {
        Expected<const cy::text::GlyphSlot*, Error> found =
            server_->glyph_slot(glyph.face, glyph.glyph);
        if (!found.has_value()) {
            return make_unexpected(found.error());
        }
        const cy::text::GlyphSlot& slot = **found;
        // A space draws nothing; a primitive for it would be a quad of zero coverage.
        const bool space = glyph.source_offset < text.size() && text[glyph.source_offset] == ' ';
        if (space || slot.metrics.width == 0 || slot.metrics.height == 0) {
            continue;
        }
        Placed item;
        item.slot = *found;
        item.bounds.x = rect.x + ((glyph.offset.x + slot.metrics.bearing_x) * scale);
        item.bounds.y =
            rect.y + ((line.baseline() + glyph.offset.y + slot.metrics.bearing_y) * scale);
        item.bounds.width = static_cast<f32>(slot.metrics.width) * scale;
        item.bounds.height = static_cast<f32>(slot.metrics.height) * scale;
        const f32 centre = glyph.offset.x + (glyph.advance * 0.5F);
        item.colour = colour_at(*entry, glyph.source_offset, centre / width);
        if (Status pushed = placed.push_back(item); !pushed) {
            return pushed;
        }
    }
    // A glyph evicted since `set_text` was rasterised again above; say so, so the renderer
    // re-uploads before it draws.
    note_atlas_changes();

    const TextStyle& style = entry->style;
    if ((style.shadow_colour >> 24U) != 0U) {
        for (Placed shadow : placed) {
            shadow.bounds.x += style.shadow_offset.x;
            shadow.bounds.y += style.shadow_offset.y;
            shadow.colour = style.shadow_colour;
            Primitive primitive = primitive_of(shadow, style, scale);
            primitive.border_width = 0.0F;
            primitive.border_colour = 0;
            if (Status pushed = out.push_back(primitive); !pushed) {
                return pushed;
            }
        }
    }
    for (const Placed& item : placed) {
        if (Status pushed = out.push_back(primitive_of(item, style, scale)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Span<const u8> TextPainter::atlas_pixels() const noexcept {
    return atlas_pixels(PixelFormat::Coverage);
}

u32 TextPainter::atlas_extent() const noexcept {
    return atlas_extent(PixelFormat::Coverage);
}

Span<const u8> TextPainter::atlas_pixels(PixelFormat format) const noexcept {
    if (server_ == nullptr || !server_->atlas(format).is_running()) {
        return {};
    }
    return server_->atlas(format).pixels();
}

u32 TextPainter::atlas_extent(PixelFormat format) const noexcept {
    if (server_ == nullptr || !server_->atlas(format).is_running()) {
        return 0U;
    }
    return server_->atlas(format).extent();
}

}  // namespace cy::ui
