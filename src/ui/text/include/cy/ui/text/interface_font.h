// SPDX-License-Identifier: MIT
#ifndef CY_UI_TEXT_INTERFACE_FONT_H
#define CY_UI_TEXT_INTERFACE_FONT_H
// CyberUI's interface typeface: Noto Sans, Latin, with its weight axis — drawn from a distance
// field.
//
// --- WHY IT REPLACES THE BUILT-IN FONT, AND WHY THAT ONE STAYS ----------------------------------
//
// #102 gave the interface a 6x13 bitmap font because the engine could not yet load a TrueType face
// (builtin_font.h). With the complete text backend (`CY_TEXT`, issue #86) it can, and an interface
// that scales with the display wants a face that does too: this one is rasterised once into a
// multi-channel distance field and drawn sharp at any size, with outlines and shadows from the same
// atlas entry.
//
// The built-in font stays, for two reasons. It is the WHOLE of the interface's text in a build with
// `CY_TEXT` off, where `has_interface_font()` is false and nothing else changes; and in a build
// with it on, it is the last face of the interface's fallback chain, so a codepoint the subset
// lacks draws as a terminal glyph or a visible box rather than as nothing.
//
// --- WHERE THE BYTES COME FROM
// --------------------------------------------------------------------
//
// deps/fonts/NotoSans-Latin-VF.ttf, compiled in by cmake/embed.cmake when `CY_TEXT` is on; the
// font's licence and provenance are deps/fonts/PROVENANCE.md and THIRD_PARTY.md.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/font.h>

namespace cy::ui {

/// Whether this build carries the interface font — true exactly when `CY_TEXT` built the complete
/// text backend that can draw it.
[[nodiscard]] bool has_interface_font() noexcept;

/// The interface font's OpenType bytes, static for the life of the program. Empty when
/// `has_interface_font()` is false.
[[nodiscard]] Span<const u8> interface_font_bytes() noexcept;

/// The face CyberUI draws its text with: the interface font as a distance field, rasterised at 32
/// pixels with a range of four — enough to draw from 8 to 128 pixels and an outline up to about
/// four pixels at the 32-pixel size — at the regular weight.
[[nodiscard]] cy::text::FontDesc interface_font_desc() noexcept;

}  // namespace cy::ui

#endif  // CY_UI_TEXT_INTERFACE_FONT_H
