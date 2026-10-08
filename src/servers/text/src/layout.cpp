#include <cy/servers/text/layout.h>
#include <cy/text/unicode.h>

namespace cy::text {
namespace {

/// Whether a break is allowed between two characters of a script that does not use spaces.
///
/// A crude class test over the blocks that matter, and the place the minimal backend is furthest
/// from UAX-14. It permits a break between any two CJK or Thai characters, which is right for
/// Chinese and Japanese, wrong for Thai — Thai needs a dictionary to find word boundaries — and
/// better than the alternative, which is a Thai paragraph that never wraps at all.
[[nodiscard]] bool breaks_anywhere(Codepoint codepoint) noexcept {
    return (codepoint >= 0x0E00 && codepoint <= 0x0E7F) ||  // Thai
           (codepoint >= 0x3040 && codepoint <= 0x30FF) ||  // Hiragana and Katakana
           (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||  // CJK extension A
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||  // CJK unified ideographs
           (codepoint >= 0xF900 && codepoint <= 0xFAFF);    // CJK compatibility ideographs
}

}  // namespace

// `decode_utf8` is src/text/'s: the two modules share the namespace and once carried two identical
// definitions of it, which is a duplicate symbol the moment one binary links both — and since M11.e
// every binary that links the text server links src/text/ too.

Status find_break_opportunities(std::string_view text, Array<BreakOpportunity>& out) noexcept {
    usize cursor = 0;
    Codepoint previous = 0;
    while (cursor < text.size()) {
        const usize start = cursor;
        const Codepoint codepoint = decode_utf8(text, cursor);

        if (codepoint == '\n') {
            if (Status pushed = out.push_back(BreakOpportunity{static_cast<u32>(cursor), true});
                !pushed) {
                return pushed;
            }
            previous = codepoint;
            continue;
        }
        // After a space, a tab or a hyphen: the break belongs AFTER the character, so the space
        // stays on the line it ended.
        if (codepoint == ' ' || codepoint == '\t' || codepoint == '-') {
            if (Status pushed = out.push_back(BreakOpportunity{static_cast<u32>(cursor), false});
                !pushed) {
                return pushed;
            }
            previous = codepoint;
            continue;
        }
        // Between two characters of a script without spaces, the break is BEFORE this one.
        if (previous != 0 && breaks_anywhere(codepoint) && breaks_anywhere(previous)) {
            if (Status pushed = out.push_back(BreakOpportunity{static_cast<u32>(start), false});
                !pushed) {
                return pushed;
            }
        }
        previous = codepoint;
    }
    return ok();
}

// --- TextLine ------------------------------------------------------------------------------------

f32 TextLine::offset_of(u32 source_offset) const noexcept {
    f32 advance = 0.0f;
    for (const ShapedGlyph& glyph : run_.glyphs) {
        if (glyph.source_offset >= source_offset) {
            return advance;
        }
        advance += glyph.advance;
    }
    return advance;
}

HitResult TextLine::hit_test(Vec2 point) const noexcept {
    HitResult result;
    if (run_.glyphs.empty()) {
        result.source_offset = run_.source_begin;
        result.clamped = true;
        return result;
    }
    if (point.x <= 0.0f) {
        result.source_offset = run_.glyphs[0].source_offset;
        result.clamped = true;
        return result;
    }

    f32 advance = 0.0f;
    for (const ShapedGlyph& glyph : run_.glyphs) {
        if (point.x < advance + glyph.advance) {
            result.source_offset = glyph.source_offset;
            // Past the middle of the glyph places the caret AFTER the character rather than before
            // it, which is the difference between a text field that feels right and one that feels
            // off by one.
            result.trailing = point.x >= advance + (glyph.advance * 0.5f);
            return result;
        }
        advance += glyph.advance;
    }
    result.source_offset = run_.source_end;
    result.trailing = true;
    result.clamped = true;
    return result;
}

Caret TextLine::caret_at(u32 source_offset) const noexcept {
    Caret caret;
    caret.height = height();
    caret.position = Vec2{offset_of(source_offset), 0.0f};
    // `has_secondary` stays false: the minimal backend is left-to-right only, so no direction
    // boundary can arise. The field exists so that a caller drawing both rectangles today needs no
    // change when one can. See layout.h.
    return caret;
}

// --- TextParagraph -------------------------------------------------------------------------------

const TextLine& TextParagraph::line(usize index) const noexcept {
    CY_ASSERT_MSG(index < lines_.size(), "a line index past the end of the paragraph");
    return lines_[index];
}

Span<const InlinePlacement> TextParagraph::inline_objects() const noexcept {
    return {placements_.data(), placements_.size()};
}

Vec2 TextParagraph::line_origin(usize index) const noexcept {
    CY_ASSERT_MSG(index < origins_.size(), "a line index past the end of the paragraph");
    return origins_[index];
}

HitResult TextParagraph::hit_test(Vec2 point) const noexcept {
    HitResult result;
    if (lines_.empty()) {
        result.clamped = true;
        return result;
    }

    // The line whose vertical span contains the point, clamped to the first and the last. A click
    // above the paragraph puts the caret at the start and one below puts it at the end, which is
    // what every text field does.
    usize chosen = 0;
    for (usize index = 0; index < lines_.size(); ++index) {
        const f32 top = origins_[index].y;
        const f32 bottom = top + lines_[index].height();
        chosen = index;
        if (point.y < bottom) {
            break;
        }
    }
    const bool outside =
        point.y < origins_[0].y ||
        point.y >= origins_[lines_.size() - 1].y + lines_[lines_.size() - 1].height();

    result = lines_[chosen].hit_test(Vec2{point.x - origins_[chosen].x, 0.0f});
    result.line = static_cast<u32>(chosen);
    result.clamped = result.clamped || outside;
    return result;
}

Caret TextParagraph::caret_at(u32 source_offset) const noexcept {
    Caret caret;
    if (lines_.empty()) {
        return caret;
    }
    for (usize index = 0; index < lines_.size(); ++index) {
        const ShapedRun& run = lines_[index].run();
        const bool last = index + 1 == lines_.size();
        if (source_offset < run.source_end || last) {
            caret = lines_[index].caret_at(source_offset);
            caret.position = caret.position + origins_[index];
            return caret;
        }
    }
    return caret;
}

void TextParagraph::clear() noexcept {
    lines_.clear();
    origins_.clear();
    placements_.clear();
    size_ = Vec2{0.0f, 0.0f};
    truncated_ = false;
}

}  // namespace cy::text
