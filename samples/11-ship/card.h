#pragma once
// The card: cooked content in, an RGBA8 image out. M11.d task 8.1 and 8.3.
//
// Everything this file draws came out of the installation. `project/card/palette.cycard` and
// `project/card/ship.cycard` are imported, cooked into one stream and packaged by `cy_build`, and
// `main.cpp` reads that stream back BY LOGICAL NAME through `build::Installation::read`. Nothing
// here has a colour or a string compiled into it except the glyphs — which is what makes "the
// content decided the pixels" a claim rather than a decoration.
//
// The one thing the card does NOT decide is the coverage footer. What legs ran, which device
// answered and what was not evaluated are facts about THIS run, and content that could state them
// would be content that could state them wrongly. `Card::draw_footer` takes them as arguments from
// the program that measured them (task 8.3).

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace cy::sample::ship {

struct Rgba {
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
    u8 a = 255;
};

/// A tightly packed RGBA8 image, rows top to bottom. The layout `tests/render/golden.h`'s
/// `write_png` and `rhi::Format::Rgba8Unorm` both use, so it is uploaded and photographed without a
/// conversion in between.
struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels;

    [[nodiscard]] usize byte_size() const noexcept {
        return static_cast<usize>(width) * height * 4U;
    }
};

/// One line of the coverage footer: a label, a verdict, and what answered.
///
/// `verdict` is deliberately one of three words and not a boolean — "NOT EVALUATED is never a
/// pass" is the rung's own rule, and a boolean has nowhere to put the third answer.
struct CoverageLine {
    std::string label;
    std::string verdict;  // "RAN", "NOT EVALUATED", "ABSENT"
    std::string detail;
};

/// The parsed card, and the image it composes.
class Card {
public:
    /// Parse a cooked stream: the palette and the card, concatenated in that order by `cook:card`.
    /// Two `cycard 1` headers is the expected shape, not an error.
    [[nodiscard]] Status parse(std::string_view document) noexcept;

    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    /// How many draw directives the content carried. Reported so that "the content decided the
    /// pixels" is a number rather than an assertion.
    [[nodiscard]] usize directive_count() const noexcept { return directives_.size(); }

    /// Compose the image the content describes.
    [[nodiscard]] Expected<Image, Error> compose() const noexcept;

    /// Write the coverage lines under the card's own "COVERAGE" heading. The program supplies
    /// them; the content supplies the space they go in.
    void draw_footer(Image& image, const std::vector<CoverageLine>& lines) const noexcept;

private:
    enum class Kind : u8 { Bar, Text };

    struct Directive {
        Kind kind = Kind::Bar;
        i32 x = 0;
        i32 y = 0;
        i32 w = 0;
        i32 h = 0;
        u32 scale = 1;
        Rgba colour;
        std::string text;
    };

    [[nodiscard]] const Rgba* colour(std::string_view name) const noexcept;

    struct NamedColour {
        std::string name;
        Rgba value;
    };

    std::string id_;
    u32 width_ = 0;
    u32 height_ = 0;
    Rgba background_;
    i32 footer_y_ = 0;
    std::vector<NamedColour> palette_;
    std::vector<Directive> directives_;
};

/// Draw one run of upper-case text into an image. Exposed because the footer is drawn by the
/// program and the card body by the content, and both need the same glyphs.
void draw_text(Image& image, i32 x, i32 y, u32 scale, Rgba colour, std::string_view text) noexcept;
void fill_rect(Image& image, i32 x, i32 y, i32 w, i32 h, Rgba colour) noexcept;

}  // namespace cy::sample::ship
