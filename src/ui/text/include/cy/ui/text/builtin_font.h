// SPDX-License-Identifier: MIT
#ifndef CY_UI_TEXT_BUILTIN_FONT_H
#define CY_UI_TEXT_BUILTIN_FONT_H
// The built-in interface font: a TEMPORARY stand-in until the engine imports real fonts.
//
// --- WHY THERE IS A FONT COMPILED INTO THE ENGINE AT ALL -----------------------------------------
//
// `src/servers/text/README.md` says there is no built-in font, and for the text server that stays
// true: a font is data a caller supplies. This header is such a caller. The runtime interface needs
// glyphs to put a developer console and a HUD on screen, and the engine cannot yet load a TrueType
// face, rasterise it or generate a signed distance field (`text-and-fonts`, issue #86). Until it
// can, CyberUI supplies the cheapest font the text server accepts — an `ImageGridFont` — built from
// the X Window System's public-domain `misc-fixed` 6x13 terminal font, ASCII 32 to 126.
//
// It goes through the real path: the text server makes a face of it, shapes and lays out with it,
// and rasterises its glyphs into the glyph atlas the interface renderer samples. When #86 lands, a
// cooked font replaces this grid and nothing downstream of the face changes. Delete this file then.

#include <cy/servers/text/font.h>

namespace cy::ui {

/// The built-in font's cell, in pixels. Monospace: every glyph advances by the cell width.
inline constexpr u32 kBuiltinFontCellWidth = 6;
inline constexpr u32 kBuiltinFontCellHeight = 13;

/// The grid: printable ASCII, coverage 0 or 255, baseline eleven rows down. The pixels are static
/// and outlive every face made from them, which `ImageGridFont` requires.
[[nodiscard]] const cy::text::ImageGridFont& builtin_font() noexcept;

}  // namespace cy::ui

#endif  // CY_UI_TEXT_BUILTIN_FONT_H
