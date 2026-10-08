// SPDX-License-Identifier: MIT
#ifndef CY_SERVERS_TEXT_COOKED_FONT_H
#define CY_SERVERS_TEXT_COOKED_FONT_H
// The cooked font: what the font importer writes and what the text server reads. M11.e, issue #86.
//
// `text-and-fonts` — "Font import and diagnostics": "Fonts SHALL be imported with configurable
// rendering mode, pre-rendered glyph ranges baked into atlases, fallback chain, OpenType feature
// defaults, and variable-font instances." The importer in tools/import/ makes those decisions once,
// at cook time; this file is the record of them, and `TextServer::create_face(const CookedFont&)`
// is what turns the record back into a face whose pre-rendered glyphs are already in the atlas.
//
// --- WHY THE FORMAT LIVES AT LAYER 2 -------------------------------------------------------------
//
// Both halves need it and the lower one decides: the server reads it at runtime with no importer
// present, and a format defined in tools/ would make the runtime depend on a tool. It is pure data
// — no FreeType, no HarfBuzz — so a build with `CY_TEXT=OFF` still reads one, and refuses only to
// make an outline face of it, with a message saying why.
//
// --- WHAT IS IN ONE
// -------------------------------------------------------------------------------
//
//   * the face description the importer chose: size, render mode, hinting, synthetic styles, the
//     variable-font instance and the OpenType feature defaults;
//   * the font file itself, byte for byte, because shaping a string the cook did not anticipate
//   needs
//     the font's tables and an atlas is not a font;
//   * the codepoint ranges that were pre-rendered, and every glyph they reach — including the ones
//     only GSUB reaches, a ligature or a contextual form — with its metrics and its rectangle in
//     one of three pages, one per `PixelFormat`;
//   * the fallback chain, as family names, for whoever loads the cooked fonts those name.
//
// Little-endian, versioned, and validated field by field on read: a truncated or hostile file is
// refused with the field it failed on rather than read out of bounds.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/font.h>
#include <cy/servers/text/text.h>

#include <string_view>

namespace cy::text {

/// The version `write_cooked_font` writes and `CookedFont::parse` accepts. Bumped on any layout
/// change; a cooked font of another version is refused, and the asset pipeline's derivation key
/// re-cooks it.
inline constexpr u32 kCookedFontVersion = 1;

/// A closed codepoint range.
struct CodepointRange {
    Codepoint first = 0;
    Codepoint last = 0;
};

/// One pre-rendered glyph: what it measures and where it is in its page.
struct CookedGlyph {
    GlyphIndex glyph = kNotdef;
    GlyphMetrics metrics;
    PixelFormat format = PixelFormat::Coverage;
    /// The top-left of the glyph's pixels in the page of `format`.
    u32 x = 0;
    u32 y = 0;
};

/// One page of pre-rendered pixels: `extent * extent * bytes_per_pixel(format)` bytes, or empty
/// when no pre-rendered glyph has this format.
struct CookedPage {
    u32 extent = 0;
    Span<const u8> pixels;
};

/// What the importer hands `write_cooked_font`.
struct CookedFontContent {
    /// The face as it will be created. `family` is written; the string need only outlive the call.
    FontDesc desc;
    FontSource source;
    Span<const CodepointRange> ranges;
    /// Family names of the faces to search after this one, in order.
    Span<const std::string_view> fallbacks;
    Span<const CookedGlyph> glyphs;
    CookedPage pages[kPixelFormatCount] = {};
};

/// Serialise a cooked font. `out` is cleared first.
[[nodiscard]] Status write_cooked_font(const CookedFontContent& content, Array<u8>& out) noexcept;

/// A cooked font, read.
///
/// A VIEW over the bytes it was parsed from: the family name, the font file and the pages all point
/// into them, so those bytes must outlive this object and every face made from it — which a cooked
/// font asset held by the asset system satisfies, as `FontSource` requires.
class CookedFont {
public:
    CookedFont() noexcept = default;

    CookedFont(const CookedFont&) = delete;
    CookedFont& operator=(const CookedFont&) = delete;
    CookedFont(CookedFont&&) noexcept = default;
    CookedFont& operator=(CookedFont&&) noexcept = default;

    /// Read `bytes`. Fails with `InvalidArgument` naming the field that does not fit, and with
    /// `Unsupported` for a version this build does not read.
    [[nodiscard]] Status parse(Span<const u8> bytes) noexcept;

    [[nodiscard]] const FontDesc& desc() const noexcept { return desc_; }
    [[nodiscard]] const FontSource& source() const noexcept { return source_; }
    [[nodiscard]] Span<const CodepointRange> ranges() const noexcept { return ranges_.span(); }
    [[nodiscard]] Span<const std::string_view> fallbacks() const noexcept {
        return fallbacks_.span();
    }
    [[nodiscard]] Span<const CookedGlyph> glyphs() const noexcept { return glyphs_.span(); }
    [[nodiscard]] const CookedPage& page(PixelFormat format) const noexcept {
        return pages_[static_cast<u32>(format)];
    }
    /// Whether `codepoint` falls in a pre-rendered range.
    [[nodiscard]] bool covers(Codepoint codepoint) const noexcept;

private:
    FontDesc desc_{};
    FontSource source_{};
    Array<CodepointRange> ranges_;
    Array<std::string_view> fallbacks_;
    Array<CookedGlyph> glyphs_;
    CookedPage pages_[kPixelFormatCount] = {};
};

}  // namespace cy::text

#endif  // CY_SERVERS_TEXT_COOKED_FONT_H
