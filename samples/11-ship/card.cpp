#include "card.h"

#include <cstdlib>
#include <cstring>

namespace cy::sample::ship {
namespace {

// --- The glyphs ----------------------------------------------------------------------------------
//
// 5x7, upper case, written as pictures rather than as hex so that a wrong pixel is visible in the
// diff that introduces it. A sample that drew its own label out of a font file would need the font
// file in the package, and the package is the subject of this artefact rather than its vehicle —
// so the label is code and the CARD is content (card.h's opening paragraph).

constexpr u32 kGlyphWidth = 5;
constexpr u32 kGlyphHeight = 7;
constexpr u32 kGlyphAdvance = 6;  // one column of air between glyphs, at scale 1

struct Glyph {
    char character;
    const char* rows[kGlyphHeight];
};

constexpr Glyph kGlyphs[] = {
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".####", "#....", "#....", "#....", "#....", "#....", ".####"}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#..##", "#...#", "#...#", ".###."}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#...#", "#.#.#", "##.##", "#...#"}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", "#####"}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".##..", ".##..", ".#..."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
    {'(', {"..##.", ".#...", "#....", "#....", "#....", ".#...", "..##."}},
    {')', {".##..", "...#.", "....#", "....#", "....#", "...#.", ".##.."}},
    {'\'', {"..#..", "..#..", ".....", ".....", ".....", ".....", "....."}},
    {'%', {"#...#", "....#", "...#.", "..#..", ".#...", "#....", "#...#"}},
    {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
};

[[nodiscard]] const Glyph& glyph_for(char character) noexcept {
    const char upper =
        (character >= 'a' && character <= 'z') ? static_cast<char>(character - 32) : character;
    for (const Glyph& candidate : kGlyphs) {
        if (candidate.character == upper) {
            return candidate;
        }
    }
    return kGlyphs[sizeof(kGlyphs) / sizeof(kGlyphs[0]) - 1];  // '?'
}

/// A content error, spelled as the engine spells one. `cy::fail` returns something convertible to
/// both `Status` and `Expected<T, Error>`, which is why this is a wrapper over it rather than a
/// bare `Error` — a bare `Error` is not a failed `Status`.
[[nodiscard]] auto card_error(const char* message) noexcept {
    return fail(ErrorCode::InvalidArgument, message);
}

// --- The line reader -----------------------------------------------------------------------------

constexpr usize kMaxWords = 10;

struct Words {
    std::string_view items[kMaxWords];
    usize count = 0;

    [[nodiscard]] std::string_view at(usize index) const noexcept {
        return index < count ? items[index] : std::string_view{};
    }
};

/// The words of one line, quotes honoured. Same shape as samples/06-open-world's splitter, and for
/// the same reason: a title is one word with spaces in it.
[[nodiscard]] Words split(std::string_view line) noexcept {
    Words words;
    usize index = 0;
    while (index < line.size() && words.count < kMaxWords) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        if (index >= line.size()) {
            break;
        }
        const bool quoted = line[index] == '"';
        index += quoted ? 1 : 0;
        const usize start = index;
        while (index < line.size() &&
               (quoted ? line[index] != '"' : line[index] != ' ' && line[index] != '\t')) {
            ++index;
        }
        words.items[words.count++] = line.substr(start, index - start);
        index += (quoted && index < line.size()) ? 1 : 0;
    }
    return words;
}

[[nodiscard]] i32 to_integer(std::string_view text) noexcept {
    i32 value = 0;
    bool negative = false;
    usize index = 0;
    if (index < text.size() && text[index] == '-') {
        negative = true;
        ++index;
    }
    for (; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') {
            break;
        }
        value = (value * 10) + (text[index] - '0');
    }
    return negative ? -value : value;
}

}  // namespace

// --- Drawing -------------------------------------------------------------------------------------

void fill_rect(Image& image, i32 x, i32 y, i32 w, i32 h, Rgba colour) noexcept {
    if (image.pixels.empty() || w <= 0 || h <= 0) {
        return;
    }
    const i32 x0 = x < 0 ? 0 : x;
    const i32 y0 = y < 0 ? 0 : y;
    const i32 x1 = (x + w) > static_cast<i32>(image.width) ? static_cast<i32>(image.width) : x + w;
    const i32 y1 = (y + h) > static_cast<i32>(image.height) ? static_cast<i32>(image.height) : y + h;
    for (i32 row = y0; row < y1; ++row) {
        u8* line = image.pixels.data() + (static_cast<usize>(row) * image.width * 4U);
        for (i32 column = x0; column < x1; ++column) {
            u8* texel = line + (static_cast<usize>(column) * 4U);
            texel[0] = colour.r;
            texel[1] = colour.g;
            texel[2] = colour.b;
            texel[3] = colour.a;
        }
    }
}

void draw_text(Image& image, i32 x, i32 y, u32 scale, Rgba colour, std::string_view text) noexcept {
    const i32 step = static_cast<i32>(kGlyphAdvance * (scale == 0 ? 1U : scale));
    i32 pen = x;
    for (const char character : text) {
        const Glyph& glyph = glyph_for(character);
        for (u32 row = 0; row < kGlyphHeight; ++row) {
            for (u32 column = 0; column < kGlyphWidth; ++column) {
                if (glyph.rows[row][column] != '#') {
                    continue;
                }
                fill_rect(image, pen + static_cast<i32>(column * scale),
                          y + static_cast<i32>(row * scale), static_cast<i32>(scale),
                          static_cast<i32>(scale), colour);
            }
        }
        pen += step;
    }
}

// --- Parsing -------------------------------------------------------------------------------------

const Rgba* Card::colour(std::string_view name) const noexcept {
    for (const NamedColour& entry : palette_) {
        if (entry.name == name) {
            return &entry.value;
        }
    }
    return nullptr;
}

Rgba Card::colour_or(std::string_view name, Rgba fallback) const noexcept {
    const Rgba* found = colour(name);
    return found != nullptr ? *found : fallback;
}

Status Card::parse(std::string_view document) noexcept {
    // TWO `cycard 1` HEADERS ARE THE EXPECTED SHAPE. `cook:card` concatenated the palette and the
    // card in declaration order, so the stream carries both files' headers — which is also why a
    // colour is always defined before the directive that names it.
    usize headers = 0;
    usize cursor = 0;
    while (cursor <= document.size()) {
        const usize newline = document.find('\n', cursor);
        const std::string_view line =
            document.substr(cursor, newline == std::string_view::npos ? std::string_view::npos
                                                                      : newline - cursor);
        cursor = (newline == std::string_view::npos) ? document.size() + 1 : newline + 1;

        const Words words = split(line);
        if (words.count == 0 || words.at(0).empty() || words.at(0)[0] == '#') {
            continue;
        }
        const std::string_view keyword = words.at(0);

        if (keyword == "cycard") {
            if (words.at(1) != "1") {
                return card_error("the card stream is not `cycard 1`");
            }
            ++headers;
        } else if (keyword == "colour" && words.count >= 5) {
            NamedColour entry;
            entry.name = std::string(words.at(1));
            entry.value = Rgba{static_cast<u8>(to_integer(words.at(2))),
                               static_cast<u8>(to_integer(words.at(3))),
                               static_cast<u8>(to_integer(words.at(4))), 255};
            palette_.push_back(std::move(entry));
        } else if (keyword == "card" && words.count >= 5) {
            id_ = std::string(words.at(1));
            width_ = static_cast<u32>(to_integer(words.at(2)));
            height_ = static_cast<u32>(to_integer(words.at(3)));
            const Rgba* background = colour(words.at(4));
            if (background == nullptr) {
                return card_error("the card's background names a colour the palette does not have");
            }
            background_ = *background;
        } else if (keyword == "bar" && words.count >= 6) {
            const Rgba* value = colour(words.at(5));
            if (value == nullptr) {
                return card_error("a bar names a colour the palette does not have");
            }
            Directive directive;
            directive.kind = Kind::Bar;
            directive.x = to_integer(words.at(1));
            directive.y = to_integer(words.at(2));
            directive.w = to_integer(words.at(3));
            directive.h = to_integer(words.at(4));
            directive.colour = *value;
            directives_.push_back(std::move(directive));
        } else if (keyword == "text" && words.count >= 6) {
            const Rgba* value = colour(words.at(4));
            if (value == nullptr) {
                return card_error("a text line names a colour the palette does not have");
            }
            Directive directive;
            directive.kind = Kind::Text;
            directive.x = to_integer(words.at(1));
            directive.y = to_integer(words.at(2));
            directive.scale = static_cast<u32>(to_integer(words.at(3)));
            directive.colour = *value;
            directive.text = std::string(words.at(5));
            // BEFORE the move, and that is not a style preference: the first draft read
            // `directive.text` after `std::move(directive)`, which is empty, so the heading was
            // never found and the footer fell back to its default position — visibly, on top of the
            // device line, in the first frame this sample ever presented.
            if (directive.text == "COVERAGE") {
                footer_y_ = directive.y + static_cast<i32>(kGlyphHeight * directive.scale) + 16;
            }
            directives_.push_back(std::move(directive));
        }
    }

    if (headers == 0) {
        return card_error("the card stream carried no `cycard 1` header");
    }
    if (width_ == 0 || height_ == 0) {
        return card_error("the card stream declared no `card` line, so it has no size");
    }
    return Status{};
}

Expected<Image, Error> Card::compose() const noexcept {
    Image image;
    image.width = width_;
    image.height = height_;
    image.pixels.assign(image.byte_size(), 0);
    fill_rect(image, 0, 0, static_cast<i32>(width_), static_cast<i32>(height_), background_);

    for (const Directive& directive : directives_) {
        if (directive.kind == Kind::Bar) {
            fill_rect(image, directive.x, directive.y, directive.w, directive.h, directive.colour);
        } else {
            draw_text(image, directive.x, directive.y, directive.scale, directive.colour,
                      directive.text);
        }
    }
    return image;
}

void Card::draw_footer(Image& image, const std::vector<CoverageLine>& lines) const noexcept {
    // The three verdicts are three colours, and the colours come from the palette the CONTENT
    // declared — so a card with a different palette restyles the footer without a recompile, and a
    // card that forgot one gets a legible fallback rather than an invisible line.
    const Rgba fallback{200, 200, 200, 255};
    const Rgba* dim = colour("dim");

    // Four verdicts, four colours, and the mapping is explicit rather than "anything that is not a
    // pass is red": BUILT is not a pass and is not a failure either, and NOT EVALUATED is neither
    // and must not be able to look like either. The rung's own rule, applied to a footer.
    i32 y = footer_y_ == 0 ? static_cast<i32>(height_) - 160 : footer_y_;
    for (const CoverageLine& line : lines) {
        const Rgba* verdict_colour = colour("warn");
        if (line.verdict == "RAN" || line.verdict == "BUILT") {
            verdict_colour = colour("good");
        } else if (line.verdict == "NOT EVALUATED") {
            verdict_colour = dim;
        } else if (line.verdict == "ABSENT") {
            verdict_colour = colour("absent");
        }
        draw_text(image, 48, y, 2, dim != nullptr ? *dim : fallback, line.label);
        draw_text(image, 520, y, 2, verdict_colour != nullptr ? *verdict_colour : fallback,
                  line.verdict);
        draw_text(image, 760, y, 2, dim != nullptr ? *dim : fallback, line.detail);
        y += 28;
    }
}

}  // namespace cy::sample::ship
