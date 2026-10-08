#ifndef CY_SERVERS_TEXT_FONT_H
#define CY_SERVERS_TEXT_FONT_H
// Fonts: what a face is, how one is supplied, and what happens when a glyph is missing. M5
// task 5.3.
//
// `text-and-fonts` — "Font loading": TrueType, OpenType, WOFF, bitmap and image-grid fonts, with
// variable axes, OpenType features, synthetic styles and a **fallback chain** "so glyphs missing
// from one font are sought in the next".
//
// --- WHAT A FONT IS TO THIS MODULE, AND WHY IT IS DATA -------------------------------------------
//
// A font arrives as BYTES the caller supplies, and never as a path this module opens. Three
// reasons, and the third is the one that decided it:
//
//   * This is layer 2. A server "has no knowledge of the ECS world, the scene graph, or scripting"
//     and it has no filesystem either — reading a file is `cy::assets`' at layer 0 and the asset
//     system's above it.
//   * A cooked font is an ASSET, produced by the font importer `asset-import-pipeline` names, with
//     its pre-rendered ranges and its fallback chain already decided. A text server that loaded
//     `.ttf` files would be a second import path.
//   * It is what makes the module testable with no filesystem at all, which is what every test in
//     this directory does.
//
// So there is no built-in font compiled into the engine. A caller that wants overlay text supplies
// one — and the minimal backend's `ImageGridFont` is deliberately the cheapest possible thing to
// supply: a grid of cells over a codepoint range, which is a screenshot of a terminal font and four
// numbers.
//
// --- THE FALLBACK CHAIN IS PART OF THE FACE, NOT A GLOBAL ----------------------------------------
//
// "WHEN a codepoint is absent from the primary font THEN the fallback chain SHALL be searched, then
// the system fallback, and finally a visible `.notdef` box SHALL be rendered rather than nothing."
//
// The chain is a property of the face a caller asked for, because two callers want different
// chains: an interface wants an emoji font after its Latin one and a code view emphatically does
// not. A global chain makes that a setting somebody has to remember to change per call site.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/handle.h>
#include <cy/servers/text/text.h>

#include <string_view>

namespace cy::text {

CY_HANDLE_TAG(Font);
using FontHandle = Handle<FontTag>;

/// One variable-font axis and the value asked for.
///
/// The tag is the four-character OpenType axis tag — `wght`, `wdth`, `slnt` — as four bytes,
/// because that is what the format uses and translating it into a name would mean a table that has
/// to grow whenever a foundry invents an axis.
struct FontAxis {
    char tag[4] = {' ', ' ', ' ', ' '};
    f32 value = 0.0f;
};

/// The most axes one face instance may pin. Five covers every registered axis and leaves room; a
/// face wanting more is a face doing something this interface should be told about rather than
/// silently truncating.
inline constexpr usize kMaxFontAxes = 5;

/// A grid of glyph cells over a contiguous codepoint range.
///
/// The minimal backend's font format, and a real one: `text-and-fonts` lists "bitmap fonts, and
/// image-grid fonts" among what the engine loads. Every cell is the same size, glyphs are laid out
/// left to right and top to bottom, and the codepoint of cell `n` is `first_codepoint + n`.
///
/// One byte per pixel, coverage, top row first. A monochrome source is expanded to coverage by the
/// caller — this is the interchange form, and a second one-bit path would be a second rasteriser.
struct ImageGridFont {
    /// Coverage, `image_width * image_height` bytes. NOT copied: it must outlive the face, which a
    /// cooked font asset held by the asset system satisfies by construction.
    Span<const u8> pixels;
    u32 image_width = 0;
    u32 image_height = 0;
    u32 cell_width = 0;
    u32 cell_height = 0;
    /// How many cells across. The row count follows from the glyph count.
    u32 columns = 0;
    /// The codepoint of the first cell.
    Codepoint first_codepoint = 32;
    /// How many cells carry a glyph. Cells past this are not part of the font.
    u32 glyph_count = 0;
    /// How far the baseline sits below the top of a cell. Every glyph shares it, which is what
    /// makes a grid font a grid font.
    f32 ascent = 0.0f;
    /// The advance a glyph contributes. Zero takes `cell_width`, which is the monospace case and is
    /// what a grid font almost always is.
    f32 advance = 0.0f;

    /// Whether the arrays agree with the dimensions. Called by `create_face`, so a font whose
    /// numbers do not add up is refused at creation rather than read out of bounds at layout.
    [[nodiscard]] Status validate() const noexcept;
};

/// How strongly an outline is fitted to the pixel grid before it is rasterised.
///
/// A property of the face rather than of the call, like the size: two hinting modes produce two
/// different rasters of the same glyph, so they are two faces with two sets of atlas entries.
/// Ignored by an image-grid face, which has no outline to fit, and by a distance-field face, whose
/// whole point is a raster that is NOT fitted to one size.
enum class Hinting : u8 {
    /// The outline as designed. Positions are exact and stems blur across pixel boundaries.
    None = 0,
    /// Vertical fitting only: crisper baselines and x-heights without distorting widths. The
    /// default, and what every modern desktop renders interface text with.
    Light = 1,
    /// The font's own instructions, or the autohinter's, in both directions.
    Full = 2,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* hinting_name(Hinting hinting) noexcept;

/// One OpenType feature setting: `liga` off, `ss01` on, `kern` off.
///
/// Four bytes, for the reason `FontAxis` is: the tag is the format's, and a table translating it
/// into names would have to grow every time a foundry registered a feature. `value` is 0 to turn a
/// feature off, 1 to turn it on, and an index into the alternates for a feature that has several.
struct FontFeature {
    char tag[4] = {' ', ' ', ' ', ' '};
    u32 value = 1;
};

/// The most feature settings one face may carry. Defaults are the font's; this is for the handful a
/// caller overrides — tabular figures in a table, ligatures off in a code view.
inline constexpr usize kMaxFontFeatures = 8;

/// The bytes of an outline font: TrueType, OpenType (TrueType or CFF outlines), a TrueType
/// collection, or WOFF.
///
/// NOT copied, for the reason `ImageGridFont::pixels` is not: a cooked font asset held by the asset
/// system outlives every face made from it by construction, and a backend that parses the tables
/// lazily reads them for as long as the face exists.
struct FontSource {
    Span<const u8> bytes;
    /// Which face of a collection. Zero for a single-face file.
    u32 face_index = 0;
};

/// What a caller asks for when it creates a face.
struct FontDesc {
    /// The family name, for a diagnostic and for a system-font query. Never used to resolve
    /// anything here: this module is given bytes, not asked to find them.
    std::string_view family;
    /// The size in pixels the face is rasterised at. A face is created per size, because a glyph
    /// cache keyed by size and a face keyed by size are the same thing and having both would mean
    /// two lookups.
    f32 size_pixels = 16.0f;
    RenderMode mode = RenderMode::Grayscale;
    /// The variable-font instance. Ignored by a backend whose capabilities say `variable_fonts` is
    /// false, and part of the glyph cache's key when it is true.
    FontAxis axes[kMaxFontAxes] = {};
    u32 axis_count = 0;
    /// Emboldening applied by the rasteriser when the family has no bold face.
    bool synthetic_bold = false;
    /// A shear applied by the rasteriser when the family has no italic face.
    bool synthetic_italic = false;
    /// Grid fitting for an outline face. See `Hinting`.
    Hinting hinting = Hinting::Light;
    /// OpenType feature overrides, applied on top of the font's defaults when it is shaped.
    FontFeature features[kMaxFontFeatures] = {};
    u32 feature_count = 0;
    /// For `RenderMode::SignedDistanceField`: how many atlas pixels the field spans on each side of
    /// the outline. A wider range survives more magnification and allows a thicker outline effect,
    /// at the cost of padding every glyph by that much.
    f32 distance_range = 4.0f;
};

/// A face and the faces to search after it.
///
/// Handles rather than descriptions: a fallback face is created like any other, which means it is
/// cached like any other and two chains sharing a face share its glyphs.
struct FallbackChain {
    static constexpr usize kMaxFallbacks = 7;

    FontHandle faces[kMaxFallbacks] = {};
    u32 count = 0;

    [[nodiscard]] Status push(FontHandle face) noexcept;
};

/// What one glyph occupies, before it is placed.
///
/// In pixels, in the face's own size. `bearing` is the offset from the pen position to the top-left
/// of the glyph's coverage, with Y measured DOWN from the baseline — the direction a raster grows —
/// so a glyph above the baseline has a negative `bearing.y`. Getting that sign wrong flips every
/// glyph about its baseline, which is why it is written down here rather than left to a reader.
struct GlyphMetrics {
    f32 advance = 0.0f;
    f32 bearing_x = 0.0f;
    f32 bearing_y = 0.0f;
    u32 width = 0;
    u32 height = 0;
};

}  // namespace cy::text

#endif  // CY_SERVERS_TEXT_FONT_H
