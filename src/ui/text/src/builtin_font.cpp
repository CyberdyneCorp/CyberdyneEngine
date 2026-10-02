// SPDX-License-Identifier: MIT
#include <cy/ui/text/builtin_font.h>

#include "builtin_font_data.h"

#include <array>

namespace cy::ui {
namespace {

using namespace builtin_font_data;

static_assert(kCellWidth == kBuiltinFontCellWidth && kCellHeight == kBuiltinFontCellHeight,
              "the generated rows and the published cell disagree");

/// Sixteen cells across: printable ASCII is six rows of them.
constexpr u32 kColumns = 16;
constexpr u32 kGridRows = (kGlyphCount + kColumns - 1U) / kColumns;
constexpr u32 kImageWidth = kColumns * kCellWidth;
constexpr u32 kImageHeight = kGridRows * kCellHeight;

/// The generated one-bit rows expanded to the coverage grid, at compile time.
constexpr std::array<u8, static_cast<usize>(kImageWidth) * kImageHeight> expand() noexcept {
    std::array<u8, static_cast<usize>(kImageWidth) * kImageHeight> pixels{};
    for (u32 glyph = 0; glyph < kGlyphCount; ++glyph) {
        const u32 left = (glyph % kColumns) * kCellWidth;
        const u32 top = (glyph / kColumns) * kCellHeight;
        for (u32 y = 0; y < kCellHeight; ++y) {
            const u32 bits = kRows[(glyph * kCellHeight) + y];
            for (u32 x = 0; x < kCellWidth; ++x) {
                const bool lit = ((bits >> (kCellWidth - 1U - x)) & 1U) != 0U;
                pixels[(static_cast<usize>(top + y) * kImageWidth) + left + x] = lit ? 255U : 0U;
            }
        }
    }
    return pixels;
}

constexpr std::array<u8, static_cast<usize>(kImageWidth) * kImageHeight> kPixels = expand();

}  // namespace

const cy::text::ImageGridFont& builtin_font() noexcept {
    static const cy::text::ImageGridFont font = [] {
        cy::text::ImageGridFont grid;
        grid.pixels = Span<const u8>(kPixels.data(), kPixels.size());
        grid.image_width = kImageWidth;
        grid.image_height = kImageHeight;
        grid.cell_width = kCellWidth;
        grid.cell_height = kCellHeight;
        grid.columns = kColumns;
        grid.first_codepoint = kFirstCodepoint;
        grid.glyph_count = kGlyphCount;
        grid.ascent = static_cast<f32>(kAscent);
        grid.advance = 0.0F;
        return grid;
    }();
    return font;
}

}  // namespace cy::ui
