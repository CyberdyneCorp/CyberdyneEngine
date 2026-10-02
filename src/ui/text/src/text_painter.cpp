// SPDX-License-Identifier: MIT
#include <cy/ui/text/text_painter.h>

#include <cy/servers/text/layout.h>

#include <utility>

namespace cy::ui {
namespace {

[[nodiscard]] f32 scale_of(const TextStyle& style) noexcept {
    return static_cast<f32>(style.pixel_scale == 0U ? 1U : style.pixel_scale);
}

}  // namespace

TextPainter::TextPainter(Allocator& allocator) noexcept
    : allocator_(&allocator), entries_(allocator), characters_(allocator) {}

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
    Expected<cy::text::FontMetrics, Error> metrics = server.face_metrics(*face);
    if (!metrics.has_value()) {
        return make_unexpected(metrics.error());
    }
    chain_ = cy::text::FallbackChain{};
    if (Status pushed = chain_.push(*face); !pushed) {
        return pushed;
    }
    server_ = &server;
    line_height_ = metrics->line_height();
    atlas_page_ = atlas_page;
    if (Status warmed = warm(font); !warmed) {
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
    ++atlas_revision_;
    return ok();
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
        if (Status pushed = entries_.push_back(Entry{element, 0, 0, style}); !pushed) {
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
    return compact();
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
    for (Entry& entry : entries_) {
        const u32 offset = static_cast<u32>(live.size());
        if (Status appended =
                live.append(Span<const char>(characters_.data() + entry.offset, entry.length));
            !appended) {
            return appended;
        }
        entry.offset = offset;
    }
    characters_ = std::move(live);
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
    const auto extent = static_cast<f32>(server_->atlas().extent());
    for (const cy::text::ShapedGlyph& glyph : line.run().glyphs) {
        // A space draws nothing; a primitive for it would be a quad of zero coverage.
        if (glyph.source_offset < text.size() && text[glyph.source_offset] == ' ') {
            continue;
        }
        Expected<const cy::text::GlyphSlot*, Error> found =
            server_->glyph_slot(glyph.face, glyph.glyph);
        if (!found.has_value()) {
            return make_unexpected(found.error());
        }
        const cy::text::GlyphSlot& slot = **found;
        Primitive primitive;
        primitive.bounds.x = rect.x + ((glyph.offset.x + slot.metrics.bearing_x) * scale);
        primitive.bounds.y =
            rect.y + ((line.baseline() + glyph.offset.y + slot.metrics.bearing_y) * scale);
        primitive.bounds.width = static_cast<f32>(slot.metrics.width) * scale;
        primitive.bounds.height = static_cast<f32>(slot.metrics.height) * scale;
        primitive.uv.x = static_cast<f32>(slot.rect.position.x) / extent;
        primitive.uv.y = static_cast<f32>(slot.rect.position.y) / extent;
        primitive.uv.width = static_cast<f32>(slot.metrics.width) / extent;
        primitive.uv.height = static_cast<f32>(slot.metrics.height) / extent;
        primitive.material = material_index(BuiltinMaterial::Glyph);
        primitive.atlas = atlas_page_;
        primitive.colour = entry->style.colour;
        if (Status pushed = out.push_back(primitive); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Span<const u8> TextPainter::atlas_pixels() const noexcept {
    if (server_ == nullptr) {
        return {};
    }
    return server_->atlas().pixels();
}

u32 TextPainter::atlas_extent() const noexcept {
    return server_ == nullptr ? 0U : server_->atlas().extent();
}

}  // namespace cy::ui
