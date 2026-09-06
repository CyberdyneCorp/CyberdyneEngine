#ifndef CY_SERVERS_TEXT_TEXT_H
#define CY_SERVERS_TEXT_TEXT_H
// The text vocabulary: what a caller says about text before anything is measured. M5 task 5.3.
//
// `text-and-fonts` reaches **Seed** at M5: "interfaces, data model and invariants exist; dependents
// can be built against it". This module is that, plus a working minimal backend — because a Seed
// nobody can run is a Seed nobody has checked.
//
// --- WHAT SEED MEANS HERE, STATED BEFORE ANYTHING ELSE -------------------------------------------
//
// `text-and-fonts` names HarfBuzz, ICU and FreeType, and the specification is right that
// "correctness for the world's writing systems is only achievable by using the mature libraries".
// None of the three is integrated at M5 (`deps/manifest.toml`'s header says why), so what is
// delivered is the half the specification itself carves out:
//
//   "A **minimal backend** SHALL be available for size-constrained builds, supporting only simple
//    left-to-right layout without shaping or ICU."
//
// So there is one interface, `TextServer`, and one backend behind it today. The backend DECLARES
// what it cannot do through `TextCapabilities`, and the specification requires exactly that:
// "WHEN code needs to know whether bidirectional layout is available THEN it SHALL query the
// capability rather than testing which backend is active." Nothing here pretends to shape Arabic.
// A caller that asks for it is told no, in a way it can act on.
//
// --- THE INVARIANT THIS MODULE EXISTS TO HOLD ----------------------------------------------------
//
// No HarfBuzz, ICU or FreeType type appears above the backend — today trivially, and in future
// because the interface is shaped so it cannot. Every type a caller touches is defined in this
// module: a glyph is a `GlyphIndex` and an atlas rectangle, a shaped run is an array of positions,
// a font is a handle. The day FreeType is linked, `FT_Face` lives in one `.cpp` beneath this
// interface and nothing above it changes.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy::text {

/// A Unicode scalar value. `char32_t` rather than `u32` so that a signature saying "codepoint" says
/// it in the type.
using Codepoint = char32_t;

/// A glyph within a font. NOT a codepoint: the mapping between the two is the font's, one codepoint
/// can produce several glyphs and several can produce one, and conflating them is how a text system
/// comes to believe a character is a glyph.
using GlyphIndex = u32;

/// The glyph a font uses for a codepoint it does not have.
///
/// `text-and-fonts`: "a visible `.notdef` box SHALL be rendered rather than nothing". Zero is the
/// index every font format reserves for it, which is why it is a constant rather than a policy.
inline constexpr GlyphIndex kNotdef = 0;

/// Which way a run of text advances.
enum class Direction : u8 {
    /// Latin, Cyrillic, Greek, the Indic scripts, and the minimal backend's only answer.
    LeftToRight = 0,
    /// Arabic, Hebrew, Thaana.
    RightToLeft = 1,
    /// Chinese, Japanese and Korean set vertically.
    TopToBottom = 2,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* direction_name(Direction direction) noexcept;

/// How a glyph is turned into coverage.
enum class RenderMode : u8 {
    /// Coverage in one channel. The default, and what UI at a fixed size wants.
    Grayscale = 0,
    /// Three horizontal coverage samples per pixel, for an LCD panel whose subpixel order is known.
    SubpixelLcd = 1,
    /// One bit per pixel. For a bitmap font and for the deliberately aliased look.
    Monochrome = 2,
    /// A multi-channel signed distance field, so one raster serves every size.
    ///
    /// `text-and-fonts`: "MSDF SHALL be used where text must scale, rotate, or be rendered in 3D
    /// without re-rasterisation; grayscale SHALL be the default for UI at fixed sizes."
    SignedDistanceField = 3,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* render_mode_name(RenderMode mode) noexcept;

/// What overflowing text does when it will not fit.
enum class Overflow : u8 {
    /// Draw what fits and cut the rest at the box's edge.
    Clip = 0,
    /// Break between words.
    WordWrap = 1,
    /// Break anywhere, for a language with no spaces or a very narrow box.
    CharacterWrap = 2,
    /// Replace the leading glyphs with an ellipsis.
    EllipsisStart = 3,
    /// Replace glyphs in the middle, keeping both ends. What a file path wants.
    EllipsisMiddle = 4,
    /// Replace the trailing glyphs with an ellipsis.
    EllipsisEnd = 5,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* overflow_name(Overflow overflow) noexcept;

/// Where a line sits within its box.
enum class Alignment : u8 { Start = 0, Centre = 1, End = 2, Justify = 3 };

/// What a backend can actually do.
///
/// Queried rather than inferred, which is the specification's own requirement. Every `false` here
/// is a thing a caller must handle rather than a thing that will silently produce wrong output —
/// and every one of them is `false` in the minimal backend, which is the honest state at M5.
struct TextCapabilities {
    /// Contextual forms, ligatures, reordering, mark positioning: what HarfBuzz does.
    bool complex_shaping = false;
    /// The Unicode Bidirectional Algorithm.
    bool bidirectional = false;
    /// Dictionary line breaking for Thai, Japanese, Chinese and Khmer.
    bool dictionary_line_breaking = false;
    /// COLR/CPAL, CBDT and SVG-in-OpenType colour glyphs.
    bool colour_glyphs = false;
    /// Variable font axes, and a cache keyed by their values.
    bool variable_fonts = false;
    /// Positioning a glyph at a fraction of a pixel, at the cost of a cache entry per position.
    bool subpixel_positioning = false;
    /// Multi-channel signed distance fields.
    bool signed_distance_fields = false;
    /// Vertical layout for East Asian scripts.
    bool vertical_layout = false;
    /// Kashida elongation for justified Arabic.
    bool kashida_justification = false;
    /// Which backend answered. For a diagnostic and for a log line; never for a branch — that is
    /// what the flags above are for.
    const char* backend = "";
};

/// A font's vertical metrics, in the size the face was created at.
///
/// Ascent is positive above the baseline and descent is positive below it, which is the convention
/// every layout expression in this module is written in. The alternative — descent negative — makes
/// `ascent + descent` the wrong sign in half the places it appears, and it is the commonest source
/// of a line spacing that is subtly wrong.
struct FontMetrics {
    f32 ascent = 0.0f;
    f32 descent = 0.0f;
    /// The recommended extra space between the descent of one line and the ascent of the next.
    f32 line_gap = 0.0f;
    /// Ascent plus descent plus line gap: the distance between two baselines.
    [[nodiscard]] f32 line_height() const noexcept { return ascent + descent + line_gap; }
    /// The height of a lower-case x, for optical centring. Zero when the face does not say.
    f32 x_height = 0.0f;
    /// The height of a capital. Zero when the face does not say.
    f32 cap_height = 0.0f;
    /// The width of a space, which justification distributes.
    f32 space_advance = 0.0f;
    /// True when every glyph has the same advance, which lets a caller measure by counting.
    bool monospace = false;
};

/// What a text system has been doing, for the diagnostics `text-and-fonts` requires: "atlas
/// occupancy and eviction rates, per-frame glyph rasterisation counts, shaping cache hit rates, and
/// fonts that trigger fallback frequently".
struct TextDiagnostics {
    u64 glyphs_rasterised = 0;
    u64 glyphs_evicted = 0;
    u64 atlas_growths = 0;
    u64 shaping_cache_hits = 0;
    u64 shaping_cache_misses = 0;
    u64 fallbacks_taken = 0;
    u64 notdef_served = 0;
    /// Used atlas area over total, in [0, 1].
    f32 atlas_occupancy = 0.0f;
    /// Rasterisations of a glyph that had been evicted since it was last used. A number climbing
    /// with a steady workload is the thrashing the specification asks to be reported: the atlas is
    /// too small for the set of glyphs in use, and every frame pays to redraw what it just threw
    /// away.
    u64 thrashes = 0;
};

}  // namespace cy::text

#endif  // CY_SERVERS_TEXT_TEXT_H
