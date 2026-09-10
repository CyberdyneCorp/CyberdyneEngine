#ifndef CY_TEXT_SHAPING_H
#define CY_TEXT_SHAPING_H
// Run itemisation, Arabic joining, and the shaping cache. M8.b task 9.4.
//
// `text-and-fonts`: "Runs SHALL be segmented by script, direction, and font before shaping, and
// shaped results SHALL be cached keyed by the run's content and parameters", and "WHEN Arabic text
// is shaped THEN contextual initial, medial, final, and isolated forms SHALL be selected correctly,
// and marks positioned per the font's tables".
//
// --- WHAT IS HERE AND WHAT NEEDS HARFBUZZ --------------------------------------------------------
//
// HARFBUZZ IS NOT IN `deps/manifest.toml`. What that costs, stated precisely rather than left for a
// reader to discover:
//
//   HERE      Run itemisation by script, direction and face. ARABIC JOINING — the joining classes,
//             the contextual form selection, and the mandatory lam-alef ligature — which is real
//             contextual shaping and is what makes Arabic legible rather than a row of isolated
//             letters. The shaping CACHE, keyed by content and parameters.
//   NOT HERE  GSUB and GPOS. Mark positioning "per the font's tables" needs the font's tables, and
//             reading them is FreeType's or HarfBuzz's job; Indic reordering is a per-script
//             engine; kerning pairs come from the font. `ShapingCapabilities` reports each of those
//             as false, and `text-and-fonts`' own rule applies — a caller asks the capability
//             rather than testing which backend is active.
//
// The joining implementation maps to the ARABIC PRESENTATION FORMS-B block (U+FE70..U+FEFF), which
// every Arabic font in wide use covers, so the result is renderable through a face that exposes
// only a codepoint-to-glyph map — which is what `cy::servers-text`'s minimal backend is.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/text/bidi.h>
#include <cy/text/unicode.h>

#include <string_view>

namespace cy::text {

/// What this module's shaping can actually do. Queried, never inferred.
struct ShapingCapabilities {
    /// Arabic contextual forms and the lam-alef ligature.
    bool arabic_joining = true;
    /// Run segmentation by script, direction and face.
    bool itemisation = true;
    /// Shaped results reused when content and parameters are unchanged.
    bool shaping_cache = true;
    /// GSUB: ligatures beyond lam-alef, Indic reordering, contextual substitution.
    bool glyph_substitution = false;
    /// GPOS: mark positioning and kerning from the font's tables.
    bool glyph_positioning = false;
    /// Vertical layout for East Asian scripts.
    bool vertical = false;
};

[[nodiscard]] ShapingCapabilities shaping_capabilities() noexcept;

/// One run: a maximal span of one script, one direction and one face.
struct TextRun {
    u32 begin = 0;
    u32 end = 0;
    Script script = Script::Common;
    /// The resolved bidirectional level, from `bidi.h`.
    u8 level = 0;
    /// Which face in the caller's fallback chain covers this run. An index rather than a handle:
    /// itemisation does not create faces and must not need a server to run.
    u32 face = 0;

    [[nodiscard]] constexpr bool right_to_left() const noexcept { return (level & 1U) != 0U; }
};

/// Answers which face in a chain has a codepoint. Implemented over `TextServer::has_glyph`, or over
/// a cooked font's coverage bitmap — this module does not care which.
class FaceCoverage {
public:
    FaceCoverage() = default;
    virtual ~FaceCoverage() = default;
    FaceCoverage(const FaceCoverage&) = delete;
    FaceCoverage& operator=(const FaceCoverage&) = delete;
    FaceCoverage(FaceCoverage&&) = delete;
    FaceCoverage& operator=(FaceCoverage&&) = delete;

    /// The index of the first face in the chain that has `codepoint`, or `kNoFace` when none does.
    static constexpr u32 kNoFace = 0xFFFFFFFFU;
    [[nodiscard]] virtual u32 face_for(Codepoint codepoint) const noexcept = 0;
};

/// Split text into runs. `runs` from `bidi.h` supplies the levels; passing an empty span itemises
/// by script and face alone, which is what a left-to-right-only caller wants.
[[nodiscard]] Status itemise(std::string_view text, Span<const BidiRun> levels,
                             const FaceCoverage* coverage, Array<TextRun>& out) noexcept;

// --- Arabic joining ------------------------------------------------------------------------------

/// A letter's joining behaviour. The Unicode joining type, in the subset Arabic needs.
enum class JoiningType : u8 {
    /// Joins on both sides: most Arabic letters.
    Dual = 0,
    /// Joins only to the right (to the preceding letter): alef, dal, ra, waw and their relatives.
    Right,
    /// Joins nothing: a space, a digit, punctuation.
    NonJoining,
    /// Transparent: a mark. It does not break a join — which is the rule that makes a vowelled word
    /// still join correctly, and the one an implementation usually forgets.
    Transparent,
};

[[nodiscard]] JoiningType joining_type_of(Codepoint codepoint) noexcept;

/// The four contextual forms.
enum class JoiningForm : u8 { Isolated = 0, Initial, Medial, Final };

/// One shaped Arabic character: the presentation form to draw, and where it came from.
struct JoinedGlyph {
    /// The codepoint to look up in the face: a presentation form for a joined letter, or the
    /// original codepoint when the letter has no form or is not Arabic.
    Codepoint presentation = 0;
    Codepoint source = 0;
    /// The byte offset in the source text. Every later stage — hit-testing, carets, selection —
    /// needs it, which is why it travels with the glyph rather than being recomputed.
    u32 source_offset = 0;
    JoiningForm form = JoiningForm::Isolated;
    /// True when this glyph is the lam-alef ligature, which is TWO source characters. A caret
    /// between them is a caret inside a ligature, and a caller that reports it has to know.
    bool ligature = false;
};

/// Apply Arabic joining to a run, appending its glyphs to `out` in LOGICAL order.
///
/// Non-Arabic characters pass through with `presentation == source`, so a caller may hand this a
/// mixed run without splitting it first — though `itemise` will already have.
[[nodiscard]] Status join_arabic(std::string_view text, u32 begin, u32 end,
                                 Array<JoinedGlyph>& out) noexcept;

// --- The shaping cache ---------------------------------------------------------------------------

/// The key a shaped run is cached under: its content and every parameter that changes the result.
struct ShapeKey {
    /// A hash of the run's bytes. The text itself is not kept: a cache that owned its strings would
    /// own the lifetime question too.
    u64 content = 0;
    u32 face = 0;
    /// Size in pixels, quantised to a sixteenth so that an animated size does not miss every frame.
    u32 size_sixteenths = 0;
    u8 level = 0;
    Script script = Script::Common;
    /// The language tag, as `Name`. Language changes shaping — Turkish's dotless i, Serbian's
    /// italic forms — so it is part of the key, which is what makes "cached shaped runs SHALL be
    /// invalidated where language-dependent" a consequence rather than an extra step.
    u32 language = 0;

    [[nodiscard]] u64 hash() const noexcept;
    friend bool operator==(const ShapeKey& a, const ShapeKey& b) noexcept;
};

/// A fixed-capacity shaping cache with least-recently-used eviction.
///
/// Fixed rather than growing: a cache that grows without bound is a leak with a good reputation,
/// and a text system's working set is the strings actually on screen.
class ShapeCache {
public:
    ShapeCache(Allocator& allocator, usize capacity = 256) noexcept;

    /// The stored value is an INDEX into whatever array the caller keeps its shaped runs in. This
    /// module does not own shaped glyphs; `cy::servers-text` does, and a cache that copied them
    /// would be a second copy to keep in step.
    [[nodiscard]] bool find(const ShapeKey& key, u32& value) noexcept;
    [[nodiscard]] Status insert(const ShapeKey& key, u32 value) noexcept;
    void clear() noexcept;
    /// Drop every entry whose language matches — what a locale change calls.
    void invalidate_language(u32 language) noexcept;

    [[nodiscard]] u64 hits() const noexcept { return hits_; }
    [[nodiscard]] u64 misses() const noexcept { return misses_; }
    [[nodiscard]] u64 evictions() const noexcept { return evictions_; }
    [[nodiscard]] usize size() const noexcept { return entries_.size(); }
    /// The hit rate `text-and-fonts` asks the diagnostics to report.
    [[nodiscard]] f32 hit_rate() const noexcept;

private:
    struct Entry {
        ShapeKey key;
        u32 value = 0;
        u64 used = 0;
    };

    Array<Entry> entries_;
    usize capacity_ = 256;
    u64 clock_ = 0;
    u64 hits_ = 0;
    u64 misses_ = 0;
    u64 evictions_ = 0;
};

/// The content hash a `ShapeKey` carries. Exposed because the caller hashes the bytes it already
/// has, and hashing them twice is the kind of cost a text system cannot afford per frame.
[[nodiscard]] u64 hash_content(std::string_view text) noexcept;

}  // namespace cy::text

#endif  // CY_TEXT_SHAPING_H
