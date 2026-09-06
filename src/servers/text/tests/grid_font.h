#ifndef CY_SERVERS_TEXT_TESTS_GRID_FONT_H
#define CY_SERVERS_TEXT_TESTS_GRID_FONT_H
// A grid font built in memory, for the suites that need one.
//
// Every glyph is a filled block with a one-pixel notch whose position encodes the cell index, so
// that a test can tell one glyph's coverage from another's without carrying a real typeface around.
// That is enough for everything these suites assert — metrics, packing, eviction, layout — and it
// keeps the module's "a font is data the caller supplies" decision honest in the tests as well as
// in the interface.

#include <cy/servers/text/font.h>
#include <cy/test/test.h>

#include <vector>

namespace cy::text::test {

/// A grid of `count` cells, `cell` pixels square, starting at `first`.
class GridFont {
public:
    GridFont(u32 cell, u32 columns, u32 count, Codepoint first = 32)
        : cell_(cell), columns_(columns), count_(count), first_(first) {
        const u32 rows = (count + columns - 1) / columns;
        width_ = columns * cell;
        height_ = rows * cell;
        pixels_.assign(static_cast<usize>(width_) * height_, 0);
        for (u32 index = 0; index < count; ++index) {
            const u32 column = index % columns;
            const u32 row = index / columns;
            for (u32 y = 0; y < cell; ++y) {
                for (u32 x = 0; x < cell; ++x) {
                    // A filled block, minus one notch whose position is the cell's index modulo the
                    // cell size — so two cells' coverage differ and a test can say which it got.
                    const bool notch = x == (index % cell) && y == 0;
                    const usize offset = (static_cast<usize>((row * cell) + y) * width_) +
                                         (static_cast<usize>(column) * cell) + x;
                    pixels_[offset] = notch ? 0U : 255U;
                }
            }
        }
    }

    [[nodiscard]] ImageGridFont font() const noexcept {
        ImageGridFont grid;
        grid.pixels = Span<const u8>(pixels_.data(), pixels_.size());
        grid.image_width = width_;
        grid.image_height = height_;
        grid.cell_width = cell_;
        grid.cell_height = cell_;
        grid.columns = columns_;
        grid.first_codepoint = first_;
        grid.glyph_count = count_;
        grid.ascent = static_cast<f32>(cell_) * 0.75f;
        grid.advance = 0.0f;
        return grid;
    }

private:
    u32 cell_ = 0;
    u32 columns_ = 0;
    u32 count_ = 0;
    Codepoint first_ = 32;
    u32 width_ = 0;
    u32 height_ = 0;
    std::vector<u8> pixels_;
};

}  // namespace cy::text::test

#endif  // CY_SERVERS_TEXT_TESTS_GRID_FONT_H
