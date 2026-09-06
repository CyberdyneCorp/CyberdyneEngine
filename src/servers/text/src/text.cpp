#include <cy/servers/text/font.h>
#include <cy/servers/text/text.h>

namespace cy::text {

const char* direction_name(Direction direction) noexcept {
    switch (direction) {
        case Direction::LeftToRight:
            return "left-to-right";
        case Direction::RightToLeft:
            return "right-to-left";
        case Direction::TopToBottom:
            return "top-to-bottom";
    }
    return "left-to-right";
}

const char* render_mode_name(RenderMode mode) noexcept {
    switch (mode) {
        case RenderMode::Grayscale:
            return "grayscale";
        case RenderMode::SubpixelLcd:
            return "subpixel-lcd";
        case RenderMode::Monochrome:
            return "monochrome";
        case RenderMode::SignedDistanceField:
            return "signed-distance-field";
    }
    return "grayscale";
}

const char* overflow_name(Overflow overflow) noexcept {
    switch (overflow) {
        case Overflow::Clip:
            return "clip";
        case Overflow::WordWrap:
            return "word-wrap";
        case Overflow::CharacterWrap:
            return "character-wrap";
        case Overflow::EllipsisStart:
            return "ellipsis-start";
        case Overflow::EllipsisMiddle:
            return "ellipsis-middle";
        case Overflow::EllipsisEnd:
            return "ellipsis-end";
    }
    return "clip";
}

Status ImageGridFont::validate() const noexcept {
    if (cell_width == 0 || cell_height == 0 || columns == 0 || glyph_count == 0) {
        return fail(ErrorCode::InvalidArgument, "an image-grid font with a zero dimension");
    }
    if (image_width == 0 || image_height == 0) {
        return fail(ErrorCode::InvalidArgument, "an image-grid font with no image");
    }
    if (pixels.size() != static_cast<usize>(image_width) * image_height) {
        return fail(ErrorCode::InvalidArgument,
                    "an image-grid font whose pixels do not fill its image");
    }
    if (static_cast<usize>(columns) * cell_width > image_width) {
        return fail(ErrorCode::InvalidArgument, "an image-grid font wider than its own image");
    }
    const u32 rows = (glyph_count + columns - 1) / columns;
    if (static_cast<usize>(rows) * cell_height > image_height) {
        return fail(ErrorCode::InvalidArgument, "an image-grid font taller than its own image");
    }
    if (ascent < 0.0f || ascent > static_cast<f32>(cell_height)) {
        // A baseline outside the cell would put every glyph somewhere the metrics do not describe,
        // which shows up as text that is correct in isolation and misaligned beside anything else.
        return fail(ErrorCode::InvalidArgument,
                    "an image-grid font whose baseline is outside its cell");
    }
    return ok();
}

Status FallbackChain::push(FontHandle face) noexcept {
    if (face.is_null()) {
        return fail(ErrorCode::InvalidArgument, "a null face cannot be part of a fallback chain");
    }
    if (count >= kMaxFallbacks) {
        // Refused rather than dropped. A chain that silently stopped growing would search fewer
        // faces than the caller asked for, and the symptom — one script rendering as boxes — would
        // be blamed on the font.
        return fail(ErrorCode::OutOfRange, "the fallback chain is full");
    }
    faces[count++] = face;
    return ok();
}

}  // namespace cy::text
