// SPDX-License-Identifier: MIT
#ifndef CY_IMPORT_FONT_H
#define CY_IMPORT_FONT_H
// The font importer: a TrueType, OpenType, collection or WOFF file in, a cooked font out. M11.e,
// issue #86.
//
// `text-and-fonts` — "Font import and diagnostics": "Fonts SHALL be imported with configurable:
// rendering mode, pre-rendered glyph ranges (baking common glyphs at build time to avoid runtime
// rasterisation hitches), fallback chain, OpenType feature defaults, and variable-font instances."
// Each of the five is an option below, and the cook writes the decision into the cooked font
// (`cy/servers/text/cooked_font.h`) that `TextServer::create_face(const CookedFont&)` reads.
//
// --- WHAT "PRE-RENDERED" COVERS
// --------------------------------------------------------------------
//
// Every glyph the ranges' codepoints map to, AND every glyph shaping can substitute for them — the
// `ffi` ligature, an Arabic letter's initial form, a localised variant. HarfBuzz computes that set
// (the GSUB closure), so shaping text drawn from a cooked range never meets a glyph the cook did
// not render, and laying it out rasterises nothing. `.notdef` is always included: a codepoint the
// font lacks still draws a box without a hitch.
//
// --- WHAT IT NEEDS
// ----------------------------------------------------------------------------------
//
// The complete text backend, so `CY_TEXT`. Built without it, the importer is still registered — a
// `.ttf` in a project is still claimed by it — and refuses every import with a diagnostic naming
// the option, rather than leaving the file to no importer at all.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/importer.h>
#include <cy/import/options.h>
#include <cy/servers/text/cooked_font.h>
#include <cy/servers/text/font.h>

#include <string_view>

namespace cy::import {

/// What one cook is asked to produce.
struct FontCookOptions {
    /// The face: family, size, render mode, distance range, hinting, synthetic styles, the
    /// variable-font instance and the feature defaults. `family` is written into the cooked font.
    text::FontDesc desc;
    /// Which face of a collection.
    u32 face_index = 0;
    /// The codepoints to pre-render. Empty pre-renders `.notdef` alone.
    Span<const text::CodepointRange> ranges;
    /// The families to search after this one, in order, recorded for whoever loads the chain.
    Span<const std::string_view> fallbacks;
    /// The largest page a cook may grow to. A range that does not fit is refused rather than
    /// cooked with glyphs missing.
    u32 maximum_page_extent = 4096;
};

/// What a cook did.
struct FontCookReport {
    /// Codepoints in the ranges.
    u32 codepoints = 0;
    /// Codepoints in the ranges the font has no glyph for. They draw as `.notdef`.
    u32 missing = 0;
    /// Glyphs pre-rendered, `.notdef` included.
    u32 glyphs = 0;
    /// Of those, the glyphs only substitution reaches: ligatures and contextual forms.
    u32 substituted = 0;
};

/// Whether this build can cook a font — `CY_TEXT` built the complete text backend.
[[nodiscard]] bool font_cooking_available() noexcept;

/// Cook `font` as `options` asks into `out` (cleared first), the bytes `text::CookedFont::parse`
/// reads. `font` is copied into the cooked font.
[[nodiscard]] Status cook_font(Span<const u8> font, const FontCookOptions& options, Array<u8>& out,
                               FontCookReport& report) noexcept;

/// Parse the `prerender` option: comma-separated ranges, each a preset — `ascii` (U+0020–007E),
/// `latin` (ascii and U+00A0–00FF) — or `U+XXXX` or `U+XXXX-U+YYYY` (the second `U+` optional).
/// Appends to `out`.
[[nodiscard]] Status parse_codepoint_ranges(std::string_view text,
                                            Array<text::CodepointRange>& out) noexcept;

/// Parse the `axes` option — `wght=700,wdth=90` — into `desc`'s axes.
[[nodiscard]] Status parse_axes(std::string_view text, text::FontDesc& desc) noexcept;

/// Parse the `features` option — `liga=0,tnum,ss01=1` (a bare tag is on) — into `desc`'s features.
[[nodiscard]] Status parse_features(std::string_view text, text::FontDesc& desc) noexcept;

/// The option schema the font importer declares.
[[nodiscard]] OptionsSchema font_options() noexcept;

/// `.ttf`, `.otf`, `.ttc` and `.woff` to `AssetKind::Font`. `.woff2` is claimed too, so it is
/// refused with the reason (Brotli is not built) rather than ignored.
class FontImporter final : public Importer {
public:
    /// Name "font", the extensions above, `AssetKind::Font`.
    [[nodiscard]] ImporterInfo info() const noexcept override;
    /// `font_options()`.
    [[nodiscard]] OptionsSchema schema() const noexcept override;
    /// Read the options, cook, and add the cooked font as the one primary sub-asset, "font".
    [[nodiscard]] Status import(const ImportRequest& request, ImportResult& out) noexcept override;
};

}  // namespace cy::import

#endif  // CY_IMPORT_FONT_H
