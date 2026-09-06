#ifndef CY_SERVERS_TEXT_LAYOUT_H
#define CY_SERVERS_TEXT_LAYOUT_H
// Shaped runs, lines and paragraphs: the objects a caller measures, draws and clicks in. M5 task
// 5.3.
//
// `text-and-fonts` — "Text layout objects": "`TextLine` — a single shaped line with measurement,
// hit-testing and caret positioning. `TextParagraph` — a wrapped, multi-line block with alignment,
// direction, line spacing, overflow behaviour, and **inline objects** ... Both SHALL expose: total
// size, per-line metrics (ascent, descent, baseline), glyph-to-character mapping, hit-test from a
// point to a character index, and caret rectangles."
//
// --- THE GLYPH-TO-CHARACTER MAPPING IS THE POINT -------------------------------------------------
//
// Every shaped glyph carries the byte offset of the character it came from, and it is the field
// that makes the other three requirements possible: hit-testing returns a character index rather
// than a glyph index, a caret sits between characters rather than between glyphs, and a selection
// is a range of the caller's own string. A layout that lost the mapping would be a layout you can
// draw and cannot edit — which is most of what text in an editor is for.
//
// It is a BYTE offset into the UTF-8 the caller supplied, not a codepoint index. Callers hold
// UTF-8; converting to codepoint indices would mean either a second array or a scan per query.
//
// --- CARETS AT A DIRECTION BOUNDARY --------------------------------------------------------------
//
// The specification requires "caret rectangles including split carets at direction boundaries". The
// minimal backend is left-to-right only, so no boundary can arise and `Caret::secondary` is always
// empty — but the field EXISTS, because a caller that renders one caret rectangle today and two
// when bidirectional layout lands is a caller that has to change; one that always renders both is
// not.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/font.h>
#include <cy/servers/text/text.h>

#include <string_view>

namespace cy::text {

/// One positioned glyph.
struct ShapedGlyph {
    GlyphIndex glyph = kNotdef;
    /// Which face it came from — the primary, or whichever fallback had it.
    FontHandle face;
    /// The pen offset from the run's origin, in pixels. Y is the baseline; the glyph's own bearing
    /// places its coverage relative to it.
    Vec2 offset{0.0f, 0.0f};
    f32 advance = 0.0f;
    /// The byte offset in the source text of the character this glyph came from. See the header.
    u32 source_offset = 0;
    /// True when the face had no glyph for the character and `.notdef` was substituted, so a caller
    /// can highlight what is missing rather than shipping a page of boxes nobody noticed.
    bool missing = false;
};

/// A sequence of glyphs from one face, in one direction.
struct ShapedRun {
    Array<ShapedGlyph> glyphs;
    FontHandle face;
    Direction direction = Direction::LeftToRight;
    /// The byte range of the source text this run covers.
    u32 source_begin = 0;
    u32 source_end = 0;
    f32 width = 0.0f;

    [[nodiscard]] usize size() const noexcept { return glyphs.size(); }
};

/// A caret, as one or two rectangles.
///
/// `secondary` is empty except at a direction boundary, where the caret is genuinely in two places:
/// after the last character of one run and before the first of the next, which are not adjacent on
/// screen. See the header for why the field exists before any backend can produce one.
struct Caret {
    Vec2 position{0.0f, 0.0f};
    f32 height = 0.0f;
    /// Zero width: a caret is a line, and its thickness is the interface's decision rather than the
    /// text system's.
    Vec2 secondary{0.0f, 0.0f};
    bool has_secondary = false;
};

/// Where a hit landed.
struct HitResult {
    /// The byte offset of the character hit, in the source text.
    u32 source_offset = 0;
    /// True when the point was past the middle of the character, which is where a click places the
    /// caret AFTER it rather than before. Getting this wrong is the difference between a text field
    /// that feels right and one that feels off by one.
    bool trailing = false;
    /// Which line, for a paragraph. Always zero for a line.
    u32 line = 0;
    /// True when the point was outside the text and the nearest position was returned. A caller
    /// that treats a click in the margin as a click at the end wants to know it did.
    bool clamped = false;
};

/// One shaped line, and everything a caller asks of it.
class TextLine {
public:
    TextLine() noexcept = default;

    TextLine(const TextLine&) = delete;
    TextLine& operator=(const TextLine&) = delete;
    TextLine(TextLine&&) noexcept = default;
    TextLine& operator=(TextLine&&) noexcept = default;

    [[nodiscard]] const ShapedRun& run() const noexcept { return run_; }
    [[nodiscard]] ShapedRun& run() noexcept { return run_; }
    [[nodiscard]] const FontMetrics& metrics() const noexcept { return metrics_; }
    void set_metrics(const FontMetrics& metrics) noexcept { metrics_ = metrics; }

    [[nodiscard]] f32 width() const noexcept { return run_.width; }
    [[nodiscard]] f32 height() const noexcept { return metrics_.ascent + metrics_.descent; }
    /// Where the baseline sits below the line's top edge.
    [[nodiscard]] f32 baseline() const noexcept { return metrics_.ascent; }

    /// The character at a point, and which side of it. Clamps to the ends rather than failing: a
    /// click is always somewhere.
    [[nodiscard]] HitResult hit_test(Vec2 point) const noexcept;

    /// The caret before the character at `source_offset`, or after the last one when the offset is
    /// the text's length.
    [[nodiscard]] Caret caret_at(u32 source_offset) const noexcept;

    /// The advance width from the start of the line to `source_offset`. What a selection rectangle
    /// is built from.
    [[nodiscard]] f32 offset_of(u32 source_offset) const noexcept;

private:
    ShapedRun run_;
    FontMetrics metrics_{};
};

/// Something laid out inline with the text: an image, an icon, a button.
///
/// `text-and-fonts` — "WHEN an image is embedded in a paragraph with a baseline alignment THEN it
/// SHALL occupy its advance in layout and be positioned per the alignment." It is identified by an
/// opaque `id` the caller supplies, because what the object IS is emphatically not this module's
/// business: it takes a box and gives back where the box went.
struct InlineObject {
    /// The caller's own identifier for the thing. Handed back in the placement.
    u64 id = 0;
    /// The byte offset in the source text it sits at.
    u32 source_offset = 0;
    Vec2 size{0.0f, 0.0f};
    /// How far the object's bottom sits below the baseline. Zero sits it ON the baseline.
    f32 baseline_offset = 0.0f;
};

/// Where an inline object ended up.
struct InlinePlacement {
    u64 id = 0;
    Vec2 position{0.0f, 0.0f};
    Vec2 size{0.0f, 0.0f};
    u32 line = 0;
};

/// How a paragraph is laid out.
struct ParagraphOptions {
    /// The width lines are broken to. Zero means no wrapping.
    f32 width = 0.0f;
    Alignment alignment = Alignment::Start;
    Overflow overflow = Overflow::WordWrap;
    /// Multiplied by the face's line height. One is the face's own recommendation.
    f32 line_spacing = 1.0f;
    /// The paragraph's base direction. `LeftToRight` unless a backend reports `bidirectional`.
    Direction direction = Direction::LeftToRight;
    /// The most lines to produce. Zero means no limit; a caller that has room for two lines sets
    /// two and the last one takes the ellipsis.
    u32 max_lines = 0;
    /// What an ellipsis is spelled with. A single character by default; a caller whose font lacks
    /// U+2026 passes "...".
    std::string_view ellipsis = "\xe2\x80\xa6";
};

/// A wrapped block of text.
class TextParagraph {
public:
    TextParagraph() noexcept = default;

    TextParagraph(const TextParagraph&) = delete;
    TextParagraph& operator=(const TextParagraph&) = delete;
    TextParagraph(TextParagraph&&) noexcept = default;
    TextParagraph& operator=(TextParagraph&&) noexcept = default;

    [[nodiscard]] usize line_count() const noexcept { return lines_.size(); }
    [[nodiscard]] const TextLine& line(usize index) const noexcept;
    [[nodiscard]] Span<const InlinePlacement> inline_objects() const noexcept;

    /// The block's bounding size. Height is the sum of the line heights with the spacing applied.
    [[nodiscard]] Vec2 size() const noexcept { return size_; }

    /// Whether the text did not fit and was truncated — by `max_lines`, by an ellipsis, or by
    /// clipping. A caller that wants a tooltip on truncated text asks this rather than measuring
    /// twice.
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

    [[nodiscard]] HitResult hit_test(Vec2 point) const noexcept;
    [[nodiscard]] Caret caret_at(u32 source_offset) const noexcept;

    /// Where each line's origin sits, relative to the paragraph's top-left. Alignment is applied
    /// here rather than baked into the glyph offsets, so re-aligning a paragraph does not reshape
    /// it.
    [[nodiscard]] Vec2 line_origin(usize index) const noexcept;

    void clear() noexcept;

private:
    friend class TextServer;

    Array<TextLine> lines_;
    Array<Vec2> origins_;
    Array<InlinePlacement> placements_;
    Vec2 size_{0.0f, 0.0f};
    bool truncated_ = false;
};

/// Where a line may be broken, and why.
///
/// `text-and-fonts` requires the Unicode line breaking algorithm through ICU, and requires the
/// minimal backend to degrade with "a documented capability query reporting the limitation". This
/// is the shape of the answer either backend gives; what differs is how good it is.
struct BreakOpportunity {
    /// The byte offset the break is BEFORE.
    u32 source_offset = 0;
    /// True when a break here is required rather than merely allowed — a newline.
    bool mandatory = false;
};

/// Find the break opportunities in UTF-8 text, without a dictionary.
///
/// The minimal backend's rule, written out so a reader knows exactly what it does and does not do:
/// a break is allowed after a space or a tab, after a hyphen, and between two characters that are
/// both in a CJK or Thai block; it is required after a line feed. It is a useful approximation of
/// UAX-14 for Latin text and NOT the algorithm — the difference shows up first at a non-breaking
/// space, which this treats as a space, and at Thai, which needs a dictionary.
///
/// `dictionary_line_breaking` is false in the capabilities that describe it, which is how a caller
/// finds this out without reading this comment.
[[nodiscard]] Status find_break_opportunities(std::string_view text,
                                              Array<BreakOpportunity>& out) noexcept;

/// Decode one UTF-8 sequence, returning the codepoint and advancing `cursor`.
///
/// A malformed sequence yields U+FFFD and advances one byte, which is the substitution the Unicode
/// standard specifies and is what keeps a corrupted string from becoming an infinite loop.
[[nodiscard]] Codepoint decode_utf8(std::string_view text, usize& cursor) noexcept;

}  // namespace cy::text

#endif  // CY_SERVERS_TEXT_LAYOUT_H
