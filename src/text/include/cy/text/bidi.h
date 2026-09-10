#ifndef CY_TEXT_BIDI_H
#define CY_TEXT_BIDI_H
// The Unicode Bidirectional Algorithm, UAX #9. M8.b task 9.4.
//
// `text-and-fonts`: "Text SHALL be laid out per the Unicode Bidirectional Algorithm via ICU,
// resolving paragraph direction (explicit or from first strong character), embedding levels, and
// visual reordering. The engine SHALL support: direction overrides, isolates, and structured text
// hints so technical strings (file paths, URLs, code) are ordered sensibly in RTL contexts."
//
// ICU IS NOT IN `deps/manifest.toml`, so this is the algorithm implemented over `unicode.h`'s
// tables rather than a call into ICU. What that buys and what it costs is stated here rather than
// discovered:
//
//   IMPLEMENTED   P2 and P3 (paragraph level, explicit or from the first strong character, skipping
//                 isolate runs); X1 to X8 (embeddings, overrides, isolates and their stack, with
//                 the depth limit and the overflow counters); W1 to W7; N0 to N2; I1 and I2; L1;
//                 L2.
//   APPROXIMATED  X10's ISOLATING RUN SEQUENCES. The W, N and I rules run over each LEVEL RUN
//   rather
//                 than over a sequence linked across matching isolates. The two agree except for a
//                 neutral or a number that sits either side of an isolate at the same level, where
//                 the reference algorithm sees context this one does not. `resolve_levels` reports
//                 `approximated` when the text contains an isolate initiator, so a caller can tell.
//   NOT PRESENT   The mirrored-glyph property (`bidi_mirrored`), which belongs to the shaper, and
//                 paragraph splitting, which is the caller's — `resolve_levels` treats its input as
//                 ONE paragraph and says so.
//
// --- WHY A STRUCTURED-TEXT HINT IS A SEPARATE STEP -----------------------------------------------
//
// "WHEN a path is displayed with the file structured-text hint in an RTL locale THEN its separators
// and components SHALL be ordered so the path remains readable." A file path is not fixed by the
// bidirectional algorithm — the algorithm is doing exactly what it should and the result is still
// unreadable, because a path's components are logically ordered and its separators are neutral. The
// fix is to insert isolates around the components before the algorithm runs, which is what
// `apply_structured_text()` does, and it is a separate call because it changes the TEXT.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/text/unicode.h>

#include <string_view>

namespace cy::text {

/// The paragraph's base direction.
enum class ParagraphDirection : u8 {
    /// P2/P3: the first strong character decides, and left-to-right when there is none.
    Auto = 0,
    LeftToRight = 1,
    RightToLeft = 2,
};

/// One codepoint's resolved level, with the byte range it came from.
struct BidiRun {
    /// Byte offsets into the source text.
    u32 begin = 0;
    u32 end = 0;
    /// The resolved embedding level. Even is left-to-right, odd is right-to-left — UAX #9's own
    /// convention, and the reason a level rather than a boolean is what travels.
    u8 level = 0;

    [[nodiscard]] constexpr bool right_to_left() const noexcept { return (level & 1U) != 0U; }
};

/// What `resolve_levels` produced.
struct BidiResult {
    /// The runs, in LOGICAL order. `reorder_visual` turns them into visual order.
    Array<BidiRun> runs;
    /// The paragraph level P2/P3 resolved, or the one the caller forced.
    u8 paragraph_level = 0;
    /// True when the text contains an isolate initiator, so X10's isolating run sequences were
    /// approximated by level runs. See the header: this is the one place this implementation and
    /// ICU can disagree, and it is reported rather than hidden.
    bool approximated = false;
    /// True when the embedding depth limit (125) or the stack overflowed, which UAX #9 handles by
    /// counting rather than failing — reported because an author who hit it wrote something they
    /// did not mean.
    bool overflowed = false;

    explicit BidiResult(Allocator& allocator) noexcept : runs(allocator) {}
};

/// Resolve embedding levels for ONE paragraph.
[[nodiscard]] Status resolve_levels(std::string_view text, ParagraphDirection direction,
                                    BidiResult& out) noexcept;

/// L2: reorder resolved runs into visual order, appending indices INTO `runs` to `out`.
///
/// Indices rather than copies, so a caller can carry its own per-run data — a shaped run, a font —
/// alongside and reorder both with one array.
[[nodiscard]] Status reorder_visual(Span<const BidiRun> runs, u8 paragraph_level,
                                    Array<u32>& out) noexcept;

/// What kind of technical string a hint describes.
enum class StructuredText : u8 {
    None = 0,
    /// A file path: components ordered logically, separators kept between them.
    FilePath,
    /// A URL.
    Url,
    /// Source code or an identifier: left-to-right whatever surrounds it.
    Code,
};

/// Wrap a technical string so the bidirectional algorithm leaves it readable.
///
/// Appends to `out` the text with isolates inserted — `FilePath` and `Url` isolate each component
/// so the separators keep their order; `Code` wraps the whole string in a left-to-right isolate.
/// The result is text, so it goes through `resolve_levels` like anything else, which is the
/// property that keeps this from being a second layout path.
[[nodiscard]] Status apply_structured_text(std::string_view text, StructuredText kind,
                                           Array<char>& out) noexcept;

}  // namespace cy::text

#endif  // CY_TEXT_BIDI_H
