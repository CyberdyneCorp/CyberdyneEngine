#ifndef CY_TEXT_UNICODE_H
#define CY_TEXT_UNICODE_H
// The character property tables the three algorithms above this file read. M8.b task 9.4.
//
// --- WHAT THIS MODULE IS, AND WHAT IT HONESTLY IS NOT --------------------------------------------
//
// `text-and-fonts` names HarfBuzz, ICU and FreeType and is right that "correctness for the world's
// writing systems is only achievable by using the mature libraries". NONE OF THE THREE IS IN
// `deps/manifest.toml`, and this module does not pretend otherwise. What it is:
//
//   * the Unicode Bidirectional Algorithm (UAX #9), implemented here, over a property table that
//     covers the scripts the table below names and defaults everything else to left-to-right;
//   * the line breaking algorithm (UAX #14) as a pair table over the same coverage, plus a
//     DICTIONARY interface for the scripts that need one, which the caller supplies words to;
//   * grapheme cluster boundaries (UAX #29), so a caret moves by user-perceived character;
//   * Arabic joining and the contextual forms that follow from it (`shaping.h`).
//
// And what it is not: a substitute for ICU. `coverage_of()` answers, per codepoint, whether this
// module's tables actually know about it — so a caller can tell the difference between "this text
// is left-to-right" and "this module has no data for this script and assumed left-to-right". A text
// system that could not tell those apart is one that silently renders Devanagari in the wrong
// order, and `text-and-fonts`' own capability rule — "code needs to know whether bidirectional
// layout is available THEN it SHALL query the capability rather than testing which backend is
// active" — is the same idea one level up.
//
// THE COVERED SCRIPTS, stated so the claim is checkable: Latin (including Latin-1 Supplement and
// Extended-A), Greek, Cyrillic, Hebrew, Arabic (including Supplement and Presentation Forms A and
// B), the common punctuation and digits of ASCII and General Punctuation, Thai, Hiragana, Katakana
// and the CJK Unified Ideographs. Everything else is `Coverage::Assumed`.

#include <cy/core/base/types.h>

#include <string_view>

namespace cy::text {

/// A Unicode scalar value. `char32_t` so a signature saying "codepoint" says it in the type — the
/// same spelling `cy::servers-text` uses, because this module extends that vocabulary rather than
/// starting a second one.
using Codepoint = char32_t;

/// The replacement character, which is what a malformed UTF-8 sequence decodes to.
inline constexpr Codepoint kReplacement = 0xFFFD;

/// Whether this module has real data for a codepoint.
enum class Coverage : u8 {
    /// A table entry exists: the answers below are the Unicode ones.
    Known = 0,
    /// No table entry: the answers are a documented default, not a fact.
    Assumed = 1,
};

/// The bidirectional character types this module distinguishes. A subset of UAX #9's, and the
/// subset is what the covered scripts need: the types it does not name (a dozen rare ones) collapse
/// into `OtherNeutral`, which is what UAX #9 does with them at the N rules anyway.
enum class BidiClass : u8 {
    LeftToRight = 0,        // L
    RightToLeft,            // R
    ArabicLetter,           // AL
    EuropeanNumber,         // EN
    EuropeanSeparator,      // ES
    EuropeanTerminator,     // ET
    ArabicNumber,           // AN
    CommonSeparator,        // CS
    NonSpacingMark,         // NSM
    BoundaryNeutral,        // BN
    ParagraphSeparator,     // B
    SegmentSeparator,       // S
    WhiteSpace,             // WS
    OtherNeutral,           // ON
    LeftToRightEmbedding,   // LRE
    RightToLeftEmbedding,   // RLE
    PopDirectionalFormat,   // PDF
    LeftToRightOverride,    // LRO
    RightToLeftOverride,    // RLO
    LeftToRightIsolate,     // LRI
    RightToLeftIsolate,     // RLI
    FirstStrongIsolate,     // FSI
    PopDirectionalIsolate,  // PDI
    Count,
};

/// The line break classes UAX #14's pair table is written over. Again a working subset, and the
/// classes this module does not distinguish are folded into `Alphabetic`, which is the standard's
/// own fallback for an unassigned class.
enum class LineBreakClass : u8 {
    Mandatory = 0,     // BK, LF, NL
    CarriageReturn,    // CR
    Space,             // SP
    ZeroWidthSpace,    // ZW
    OpenPunctuation,   // OP
    ClosePunctuation,  // CL
    Quotation,         // QU
    Exclamation,       // EX
    InfixSeparator,    // IS
    Numeric,           // NU
    PrefixNumeric,     // PR
    PostfixNumeric,    // PO
    Alphabetic,        // AL
    Ideographic,       // ID
    Hyphen,            // HY
    BreakAfter,        // BA
    BreakBefore,       // BB
    CombiningMark,     // CM
    /// A script that needs a dictionary: Thai, Khmer, Lao, Myanmar. UAX #14's SA class.
    ComplexContext,  // SA
    Count,
};

/// The grapheme cluster break property, in the subset a caret needs: a combining mark and a
/// regional indicator pair must not be split, and neither must a CRLF.
enum class GraphemeBreak : u8 {
    Other = 0,
    CarriageReturn,
    LineFeed,
    Control,
    Extend,
    RegionalIndicator,
    ZeroWidthJoiner,
    Count,
};

/// The scripts run itemisation distinguishes. Shaping is per run, and a run is one script, one
/// direction and one face — `text-and-fonts`: "Runs SHALL be segmented by script, direction, and
/// font before shaping".
enum class Script : u8 {
    Common = 0,
    Latin,
    Greek,
    Cyrillic,
    Hebrew,
    Arabic,
    Thai,
    Han,
    Hiragana,
    Katakana,
    /// A codepoint this module has no script data for.
    Unknown,
    Count,
};

[[nodiscard]] const char* bidi_class_name(BidiClass value) noexcept;
[[nodiscard]] const char* script_name(Script value) noexcept;

[[nodiscard]] BidiClass bidi_class_of(Codepoint codepoint) noexcept;
[[nodiscard]] LineBreakClass line_break_class_of(Codepoint codepoint) noexcept;
[[nodiscard]] GraphemeBreak grapheme_break_of(Codepoint codepoint) noexcept;
[[nodiscard]] Script script_of(Codepoint codepoint) noexcept;
/// Whether the answers above are data or a default. See the header.
[[nodiscard]] Coverage coverage_of(Codepoint codepoint) noexcept;

/// Decode one codepoint, advancing `cursor`. A malformed sequence consumes one byte and returns
/// `kReplacement`, which is the standard's own substitution and what stops a corrupted string
/// becoming an infinite loop.
[[nodiscard]] Codepoint decode_utf8(std::string_view text, usize& cursor) noexcept;

/// Encode one codepoint into `out`, which must have room for four bytes. Returns how many were
/// written; zero for a value that is not a scalar.
[[nodiscard]] u32 encode_utf8(Codepoint codepoint, char* out) noexcept;

/// The byte offset of the next grapheme cluster boundary at or after `offset`, per UAX #29's
/// extended grapheme clusters over the properties above. Used for caret movement: "the caret SHALL
/// move logically rather than visually by default", and one press moves one user-perceived
/// character rather than one codepoint.
[[nodiscard]] usize next_grapheme(std::string_view text, usize offset) noexcept;

/// The byte offset of the previous cluster boundary at or before `offset`.
[[nodiscard]] usize previous_grapheme(std::string_view text, usize offset) noexcept;

}  // namespace cy::text

#endif  // CY_TEXT_UNICODE_H
