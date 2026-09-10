#ifndef CY_TEXT_LINEBREAK_H
#define CY_TEXT_LINEBREAK_H
// Line breaking, justification and overflow. M8.b task 9.4.
//
// `text-and-fonts`: "Line breaking SHALL follow the Unicode line breaking algorithm via ICU, with
// dictionary-based breaking for scripts without spaces (Thai, Japanese, Chinese, Khmer).
// Justification SHALL support: inter-word spacing, inter-character spacing where appropriate to the
// script, kashida elongation for Arabic, and a configurable priority among them. Overflow behaviour
// SHALL support: clipping, ellipsis (start, middle, or end), word wrap, character wrap, and
// shrink-to-fit."
//
// --- THE PAIR TABLE, AND WHAT IS NOT IN IT -------------------------------------------------------
//
// UAX #14 is a pair table over line break classes plus a handful of rules that are not pairwise.
// This implements the pair table over `unicode.h`'s class subset and the rules LB2 to LB8, LB11 to
// LB13, LB18 to LB21 and LB23 to LB25 that the covered classes can express. What is NOT here:
// regional indicators, emoji sequences, the Korean syllable classes and the numeric rule LB25's
// full grammar. Each of those is a class this module's tables do not distinguish, and a caller that
// needs them needs ICU — which `deps/manifest.toml` does not carry.
//
// --- THE DICTIONARY IS THE CALLER'S ------------------------------------------------------------
//
// Thai, Lao, Khmer and Myanmar have no spaces, and UAX #14 says so and stops: the break points come
// from a dictionary. THE ENGINE CANNOT SHIP ONE — a Thai dictionary is megabytes and a licence — so
// `WordDictionary` is an interface over a word list the PROJECT supplies, matched longest-first.
// `find_breaks` consults it for every `ComplexContext` run and, when there is no dictionary, breaks
// between every pair of complex-context characters. That is wrong for Thai and better than a
// paragraph that never wraps; `BreakReport::used_dictionary` says which of the two happened, so an
// interface can tell a developer why their Thai wrapped badly.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/text/unicode.h>

#include <string_view>

namespace cy::text {

/// A place a line may end.
struct BreakOpportunity {
    /// The byte offset in the source text where the next line would begin.
    u32 offset = 0;
    /// A mandatory break: a newline, a paragraph separator. The line MUST end here.
    bool mandatory = false;
    /// The break came from a dictionary rather than from the pair table.
    bool from_dictionary = false;
};

/// A project-supplied word list for the scripts that need one.
class WordDictionary {
public:
    WordDictionary() = default;
    virtual ~WordDictionary() = default;
    WordDictionary(const WordDictionary&) = delete;
    WordDictionary& operator=(const WordDictionary&) = delete;
    WordDictionary(WordDictionary&&) = delete;
    WordDictionary& operator=(WordDictionary&&) = delete;

    /// The length in BYTES of the longest word that starts at `text`'s beginning, or zero when none
    /// does. Longest-match segmentation is what a dictionary breaker does, and doing it here rather
    /// than in the algorithm lets a project use a trie, a hash set or a real morphological analyser
    /// without this module knowing which.
    [[nodiscard]] virtual usize longest_match(std::string_view text) const noexcept = 0;
};

/// A simple longest-match dictionary over a caller-supplied word list. Real enough to use for a
/// small vocabulary — a game's Thai interface strings — and honest about what it is not.
class WordList final : public WordDictionary {
public:
    explicit WordList(Allocator& allocator) noexcept : words_(allocator) {}

    [[nodiscard]] Status add(std::string_view word) noexcept;
    [[nodiscard]] usize size() const noexcept { return words_.size(); }
    [[nodiscard]] usize longest_match(std::string_view text) const noexcept override;

private:
    Array<std::string_view> words_;
};

struct BreakReport {
    u32 opportunities = 0;
    u32 mandatory = 0;
    /// True when at least one break came from the dictionary. False with complex-context text in it
    /// means the fallback ran, and the wrapping is approximate.
    bool used_dictionary = false;
    /// True when the text contains characters this module has no data for — see
    /// `unicode.h`'s `Coverage`. The breaks are then a default rather than a fact.
    bool assumed_coverage = false;
};

/// Every place `text` may break, in order, appended to `out`.
[[nodiscard]] Status find_breaks(std::string_view text, const WordDictionary* dictionary,
                                 Array<BreakOpportunity>& out, BreakReport& report) noexcept;

// --- Justification
// --------------------------------------------------------------------------------

/// Which lever justification pulls, in the order it pulls them.
enum class JustifyPriority : u8 {
    /// Widen the spaces. What Latin text wants, and the only lever that is always available.
    InterWord = 0,
    /// Widen the gaps between characters. Right for CJK, wrong for Latin at small sizes, and
    /// catastrophic for Arabic — which is why it is a choice and not a fallback.
    InterCharacter = 1,
    /// Elongate at the kashida positions the caller marked. `text-and-fonts`: "elongation SHALL be
    /// inserted at positions the font marks valid, rather than only stretching spaces".
    Kashida = 2,
};

/// One place justification may add width.
struct JustifyPoint {
    /// The byte offset in the source text.
    u32 offset = 0;
    JustifyPriority kind = JustifyPriority::InterWord;
    /// How much this point may absorb before it looks wrong, in pixels. Zero means unbounded, which
    /// is what a space is; a kashida position has a limit, because a stretched kashida past a few
    /// times its natural length is a defect a typographer will name.
    f32 limit = 0.0F;
};

/// What justification decided.
struct JustifyResult {
    /// How much width each point receives, in the same order as the points that went in.
    Array<f32> distribution;
    /// The width that could not be distributed — every lever hit its limit. A caller shows it as a
    /// ragged edge rather than pretending the line is justified.
    f32 residual = 0.0F;

    explicit JustifyResult(Allocator& allocator) noexcept : distribution(allocator) {}
};

/// Distribute `extra` pixels across the points, honouring the priority order and each point's
/// limit.
///
/// `priorities` is the configurable order the requirement asks for: the first entry is used until
/// it is exhausted, then the second. A priority the points contain none of costs nothing.
[[nodiscard]] Status justify(Span<const JustifyPoint> points,
                             Span<const JustifyPriority> priorities, f32 extra,
                             JustifyResult& out) noexcept;

// --- Overflow
// --------------------------------------------------------------------------------------

/// What text does when it will not fit. The same set `cy::servers-text` declares, restated here
/// because `fit_with_ellipsis` takes it and a caller should not have to include a server header to
/// name a value.
enum class OverflowMode : u8 {
    Clip = 0,
    WordWrap,
    CharacterWrap,
    EllipsisStart,
    EllipsisMiddle,
    EllipsisEnd,
    /// Scale the text down until it fits. `shrink_to_fit` computes the factor.
    ShrinkToFit,
};

/// Where an ellipsis goes and what it replaces.
struct EllipsisPlan {
    /// The byte range of the source text to REMOVE, replaced by the ellipsis.
    u32 remove_begin = 0;
    u32 remove_end = 0;
    /// False when even the ellipsis does not fit, in which case the caller clips: an ellipsis wider
    /// than its box is not an improvement on the text it replaced.
    bool fits = false;
};

/// Plan an ellipsis. `advance_of` gives the width of the character starting at a byte offset, which
/// is how this function stays independent of the shaper — it never asks what a glyph is.
[[nodiscard]] EllipsisPlan plan_ellipsis(std::string_view text, OverflowMode mode, f32 available,
                                         f32 ellipsis_width,
                                         f32 (*advance_of)(std::string_view, u32, void*) noexcept,
                                         void* user) noexcept;

/// The scale that makes `natural` fit `available`, clamped to `minimum`. One over the ratio, and a
/// function rather than a division at the call site because the clamp is a policy: text scaled
/// below half is unreadable, and every caller getting that wrong differently is how a UI ends up
/// with four-pixel text in one language.
[[nodiscard]] f32 shrink_to_fit(f32 natural, f32 available, f32 minimum = 0.5F) noexcept;

}  // namespace cy::text

#endif  // CY_TEXT_LINEBREAK_H
