// SPDX-License-Identifier: MIT
#include <cy/import/font.h>

#include <cy_features.h>

#if defined(CY_TEXT)
#    include <cy/backends/text/complete_backend.h>
#    include <cy/servers/text/server.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cy::import {
namespace {

constexpr std::string_view kModes[] = {"distance-field", "grayscale", "monochrome"};
constexpr std::string_view kHinting[] = {"light", "none", "full"};

constexpr OptionSpec kOptions[] = {
    {"mode", OptionType::Enumeration, OptionValue::of_enumeration("distance-field"),
     "How glyphs are rasterised: a multi-channel distance field that draws at any size, grayscale "
     "coverage for one fixed size, or monochrome.",
     Span<const std::string_view>(kModes), 0.0, 0.0},
    {"size-pixels",
     OptionType::Float,
     OptionValue::of_float(32.0),
     "The size the face is rasterised at, in pixels. A distance field draws at any size from it.",
     {},
     4.0,
     512.0},
    {"distance-range",
     OptionType::Float,
     OptionValue::of_float(4.0),
     "For a distance field: atlas pixels the field spans each side of the outline, which bounds "
     "how "
     "far it magnifies and how thick an outline effect can be.",
     {},
     1.0,
     32.0},
    {"hinting", OptionType::Enumeration, OptionValue::of_enumeration("light"),
     "Grid fitting for a coverage or monochrome face. A distance field is never hinted.",
     Span<const std::string_view>(kHinting), 0.0, 0.0},
    {"prerender",
     OptionType::Text,
     OptionValue::of_text("latin"),
     "Codepoints to pre-render into the cooked atlas: presets ascii and latin, or U+XXXX-U+YYYY "
     "ranges, comma-separated. Shaping substitutions reachable from them are pre-rendered too.",
     {},
     0.0,
     0.0},
    {"fallbacks",
     OptionType::Text,
     OptionValue::of_text(""),
     "Families to search after this one for a missing codepoint, comma-separated, in order.",
     {},
     0.0,
     0.0},
    {"features",
     OptionType::Text,
     OptionValue::of_text(""),
     "OpenType feature defaults over the font's own: liga=0,tnum — a bare tag turns one on.",
     {},
     0.0,
     0.0},
    {"axes",
     OptionType::Text,
     OptionValue::of_text(""),
     "The variable-font instance: wght=700,wdth=90. Unnamed axes keep the font's defaults.",
     {},
     0.0,
     0.0},
    {"face-index",
     OptionType::Int,
     OptionValue::of_int(0),
     "Which face of a TrueType collection.",
     {},
     0.0,
     255.0},
    {"family",
     OptionType::Text,
     OptionValue::of_text(""),
     "The family name recorded in the cooked font. Empty takes the source file's stem.",
     {},
     0.0,
     0.0},
    {"synthetic-bold",
     OptionType::Bool,
     OptionValue::of_bool(false),
     "Embolden the outlines, for a family with no bold face.",
     {},
     0.0,
     0.0},
    {"synthetic-italic",
     OptionType::Bool,
     OptionValue::of_bool(false),
     "Shear the outlines, for a family with no italic face.",
     {},
     0.0,
     0.0},
};
constexpr std::string_view kExtensions[] = {".ttf", ".otf", ".ttc", ".woff", ".woff2"};
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Font};

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// The next comma-separated item of `text`, trimmed, advancing `text` past it.
[[nodiscard]] std::string_view next_item(std::string_view& text) noexcept {
    const usize comma = text.find(',');
    const std::string_view item = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    return trim(item);
}

[[nodiscard]] bool parse_hex(std::string_view text, u32& out) noexcept {
    if (text.size() >= 2 && (text[0] == 'U' || text[0] == 'u') && text[1] == '+') {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 6) {
        return false;
    }
    u32 value = 0;
    for (const char digit : text) {
        u32 nibble = 0;
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<u32>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            nibble = static_cast<u32>(digit - 'a') + 10U;
        } else if (digit >= 'A' && digit <= 'F') {
            nibble = static_cast<u32>(digit - 'A') + 10U;
        } else {
            return false;
        }
        value = (value << 4U) | nibble;
    }
    out = value;
    return value <= 0x10FFFF;
}

[[nodiscard]] bool parse_number(std::string_view text, f32& out) noexcept {
    // A small decimal reader rather than std::from_chars: AppleClang's libc++ has no floating-point
    // from_chars, and an axis value is a handful of digits.
    if (text.empty()) {
        return false;
    }
    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    f64 value = 0.0;
    f64 scale = 0.0;
    bool digits = false;
    for (const char c : text) {
        if (c == '.' && scale == 0.0) {
            scale = 1.0;
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        digits = true;
        if (scale == 0.0) {
            value = (value * 10.0) + static_cast<f64>(c - '0');
        } else {
            scale *= 0.1;
            value += static_cast<f64>(c - '0') * scale;
        }
    }
    out = static_cast<f32>(negative ? -value : value);
    return digits;
}

/// `tag=value` or a bare `tag`, with a four-character tag.
[[nodiscard]] bool split_setting(std::string_view item, char (&tag)[4],
                                 std::string_view& value) noexcept {
    const usize equals = item.find('=');
    const std::string_view name = trim(item.substr(0, equals));
    value = equals == std::string_view::npos ? std::string_view{} : trim(item.substr(equals + 1));
    if (name.size() != 4) {
        return false;
    }
    std::memcpy(tag, name.data(), 4);
    return true;
}

[[nodiscard]] Status error_of(ImportResult& out, const ImportRequest& request, const char* code,
                              std::string_view detail) noexcept {
    return out.report(ImportSeverity::Error, code, detail, request.source.view());
}

/// The source path's file name without its extension: the family a cook records by default.
[[nodiscard]] std::string_view stem_of(std::string_view path) noexcept {
    const usize slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const usize dot = name.find_last_of('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

/// Every option the cook reads, gathered so the import is one call to `cook_font`.
struct GatheredOptions {
    FontCookOptions cook;
    Array<text::CodepointRange> ranges;
    Array<std::string_view> fallbacks;
};

[[nodiscard]] Status gather(const ImportRequest& request, const OptionsSchema& schema,
                            GatheredOptions& gathered, const char*& failed) noexcept {
    text::FontDesc& desc = gathered.cook.desc;
    const auto text_of = [&](std::string_view name) {
        Expected<OptionValue, Error> value = request.option(schema, name);
        return value ? value->as_text() : std::string_view{};
    };
    const auto real_of = [&](std::string_view name, f64 fallback) {
        Expected<OptionValue, Error> value = request.option(schema, name);
        return static_cast<f32>(value ? value->as_float() : fallback);
    };
    const auto flag_of = [&](std::string_view name) {
        Expected<OptionValue, Error> value = request.option(schema, name);
        return value && value->as_bool();
    };
    const std::string_view mode = text_of("mode");
    desc.mode = mode == "grayscale"    ? text::RenderMode::Grayscale
                : mode == "monochrome" ? text::RenderMode::Monochrome
                                       : text::RenderMode::SignedDistanceField;
    const std::string_view hinting = text_of("hinting");
    desc.hinting = hinting == "none"   ? text::Hinting::None
                   : hinting == "full" ? text::Hinting::Full
                                       : text::Hinting::Light;
    desc.size_pixels = real_of("size-pixels", 32.0);
    desc.distance_range = real_of("distance-range", 4.0);
    desc.synthetic_bold = flag_of("synthetic-bold");
    desc.synthetic_italic = flag_of("synthetic-italic");
    desc.family = text_of("family");
    if (desc.family.empty()) {
        desc.family = stem_of(request.source.view());
    }
    if (Expected<OptionValue, Error> index = request.option(schema, "face-index"); index) {
        gathered.cook.face_index = static_cast<u32>(index->as_int());
    }
    if (Status parsed = parse_codepoint_ranges(text_of("prerender"), gathered.ranges); !parsed) {
        failed = "font-prerender";
        return parsed;
    }
    if (Status parsed = parse_axes(text_of("axes"), desc); !parsed) {
        failed = "font-axes";
        return parsed;
    }
    if (Status parsed = parse_features(text_of("features"), desc); !parsed) {
        failed = "font-features";
        return parsed;
    }
    std::string_view fallbacks = text_of("fallbacks");
    while (!fallbacks.empty()) {
        const std::string_view family = next_item(fallbacks);
        if (!family.empty()) {
            if (Status pushed = gathered.fallbacks.push_back(family); !pushed) {
                return pushed;
            }
        }
    }
    gathered.cook.ranges = gathered.ranges.span();
    gathered.cook.fallbacks = gathered.fallbacks.span();
    return ok();
}

#if defined(CY_TEXT)

/// The pre-rendered glyph set: the character map's glyphs for the ranges, the substitutions
/// HarfBuzz reaches from them, and `.notdef` — sorted and each once.
[[nodiscard]] Status glyph_set(text::CompleteTextBackend& backend, const FontCookOptions& options,
                               const text::FontSource& source, Array<text::GlyphIndex>& glyphs,
                               FontCookReport& report) noexcept {
    Expected<text::BackendFace, Error> face = backend.open_face(options.desc, source);
    if (!face) {
        return make_unexpected(face.error());
    }
    Array<text::Codepoint> present;
    Array<text::GlyphIndex> mapped;
    Status status = mapped.push_back(text::kNotdef);
    for (const text::CodepointRange& range : options.ranges) {
        for (text::Codepoint codepoint = range.first; status && codepoint <= range.last;
             ++codepoint) {
            ++report.codepoints;
            const text::GlyphIndex glyph = backend.glyph_for(face.value(), codepoint);
            if (glyph == text::kNotdef) {
                ++report.missing;
                continue;
            }
            status = present.push_back(codepoint);
            status = status ? mapped.push_back(glyph) : status;
        }
    }
    if (status) {
        status = backend.glyph_closure(face.value(), present.span(), glyphs);
    }
    backend.close_face(face.value());
    if (!status) {
        return status;
    }
    std::sort(mapped.begin(), mapped.end());
    for (const text::GlyphIndex glyph : mapped) {
        if (!std::binary_search(glyphs.begin(), glyphs.end(), glyph)) {
            if (Status pushed = glyphs.push_back(glyph); !pushed) {
                return pushed;
            }
        }
    }
    std::sort(glyphs.begin(), glyphs.end());
    const auto unique_end = std::unique(glyphs.begin(), glyphs.end());
    while (glyphs.end() != unique_end) {
        glyphs.pop_back();
    }
    report.glyphs = static_cast<u32>(glyphs.size());
    for (const text::GlyphIndex glyph : glyphs) {
        if (!std::binary_search(mapped.begin(), mapped.end(), glyph)) {
            ++report.substituted;
        }
    }
    return ok();
}

/// Rasterise every glyph into the server's atlases and record where each landed.
[[nodiscard]] Status render_glyphs(text::TextServer& server, text::FontHandle face,
                                   Span<const text::GlyphIndex> glyphs,
                                   Array<text::CookedGlyph>& out) noexcept {
    for (const text::GlyphIndex glyph : glyphs) {
        Expected<const text::GlyphSlot*, Error> slot = server.glyph_slot(face, glyph);
        if (!slot) {
            return make_unexpected(slot.error());
        }
    }
    if (server.diagnostics().glyphs_evicted != 0) {
        // An eviction means the set did not fit the largest page allowed, so a glyph rendered
        // early is no longer in it. Cooking it anyway would ship a range that rasterises.
        return fail(ErrorCode::OutOfRange,
                    "the pre-rendered glyphs do not fit the largest page allowed; narrow the "
                    "ranges, lower size-pixels, or raise the page limit");
    }
    // Read back after every glyph is in: a growth repacks, so a rectangle read during the loop
    // could already be stale.
    for (const text::GlyphIndex glyph : glyphs) {
        const text::GlyphSlot* slot = server.glyph_slot(face, glyph).value();
        text::CookedGlyph cooked;
        cooked.glyph = glyph;
        cooked.metrics = slot->metrics;
        cooked.format = static_cast<text::PixelFormat>(slot->page);
        cooked.x = static_cast<u32>(slot->rect.position.x);
        cooked.y = static_cast<u32>(slot->rect.position.y);
        if (Status pushed = out.push_back(cooked); !pushed) {
            return pushed;
        }
    }
    return ok();
}

#endif

}  // namespace

bool font_cooking_available() noexcept {
#if defined(CY_TEXT)
    return true;
#else
    return false;
#endif
}

Status parse_codepoint_ranges(std::string_view text, Array<text::CodepointRange>& out) noexcept {
    while (!text.empty()) {
        const std::string_view item = next_item(text);
        if (item.empty()) {
            continue;
        }
        Status status = ok();
        if (item == "ascii") {
            status = out.push_back(text::CodepointRange{0x20, 0x7E});
        } else if (item == "latin") {
            status = out.push_back(text::CodepointRange{0x20, 0x7E});
            status = status ? out.push_back(text::CodepointRange{0xA0, 0xFF}) : status;
        } else {
            const usize dash = item.find('-');
            text::CodepointRange range;
            u32 first = 0;
            u32 last = 0;
            const bool parsed =
                parse_hex(trim(item.substr(0, dash)), first) &&
                (dash == std::string_view::npos ? (last = first, true)
                                                : parse_hex(trim(item.substr(dash + 1)), last));
            if (!parsed || last < first) {
                return fail(
                    ErrorCode::InvalidArgument,
                    "a codepoint range is a preset (ascii, latin), U+XXXX or U+XXXX-U+YYYY");
            }
            range.first = first;
            range.last = last;
            status = out.push_back(range);
        }
        if (!status) {
            return status;
        }
    }
    return ok();
}

Status parse_axes(std::string_view text, text::FontDesc& desc) noexcept {
    desc.axis_count = 0;
    while (!text.empty()) {
        const std::string_view item = next_item(text);
        if (item.empty()) {
            continue;
        }
        if (desc.axis_count >= text::kMaxFontAxes) {
            return fail(ErrorCode::OutOfRange, "more variable-font axes than a face may pin");
        }
        text::FontAxis& axis = desc.axes[desc.axis_count];
        std::string_view value;
        if (!split_setting(item, axis.tag, value) || !parse_number(value, axis.value)) {
            return fail(ErrorCode::InvalidArgument,
                        "an axis is a four-letter tag=number, wght=700");
        }
        ++desc.axis_count;
    }
    return ok();
}

Status parse_features(std::string_view text, text::FontDesc& desc) noexcept {
    desc.feature_count = 0;
    while (!text.empty()) {
        const std::string_view item = next_item(text);
        if (item.empty()) {
            continue;
        }
        if (desc.feature_count >= text::kMaxFontFeatures) {
            return fail(ErrorCode::OutOfRange, "more feature settings than a face may carry");
        }
        text::FontFeature& feature = desc.features[desc.feature_count];
        std::string_view value;
        f32 number = 1.0f;
        if (!split_setting(item, feature.tag, value) ||
            (!value.empty() && (!parse_number(value, number) || number < 0.0f))) {
            return fail(ErrorCode::InvalidArgument,
                        "a feature is a four-letter tag, or tag=value with a whole number");
        }
        feature.value = static_cast<u32>(number);
        ++desc.feature_count;
    }
    return ok();
}

Status cook_font(Span<const u8> font, const FontCookOptions& options, Array<u8>& out,
                 FontCookReport& report) noexcept {
    report = FontCookReport{};
    out.clear();
#if defined(CY_TEXT)
    text::CompleteTextBackend backend;
    if (Status started = backend.start(); !started) {
        return started;
    }
    const text::FontSource source{font, options.face_index};
    Array<text::GlyphIndex> glyphs;
    if (Status gathered = glyph_set(backend, options, source, glyphs, report); !gathered) {
        return gathered;
    }

    // The cook's own server: its atlases ARE the cooked pages, packed by the same packer the
    // runtime uses, so a cooked page and a live one are laid out alike.
    text::TextServer server;
    text::TextServerConfig config;
    config.atlas.initial_extent = 64;
    config.atlas.maximum_extent = options.maximum_page_extent;
    if (Status started = server.start_with(config, backend); !started) {
        return started;
    }
    Expected<text::FontHandle, Error> face = server.create_face(options.desc, source);
    if (!face) {
        return make_unexpected(face.error());
    }
    Array<text::CookedGlyph> cooked;
    if (Status rendered = render_glyphs(server, face.value(), glyphs.span(), cooked); !rendered) {
        return rendered;
    }

    text::CookedFontContent content;
    content.desc = options.desc;
    content.source = source;
    content.ranges = options.ranges;
    content.fallbacks = options.fallbacks;
    content.glyphs = cooked.span();
    // A page is written only when a glyph is on it: the coverage atlas always exists in a server,
    // and a distance-field cook that shipped an empty one would carry its pixels for nothing.
    for (const text::CookedGlyph& glyph : cooked) {
        const auto format = static_cast<u32>(glyph.format);
        const text::GlyphAtlas& atlas = server.atlas(glyph.format);
        content.pages[format] = text::CookedPage{atlas.extent(), atlas.pixels()};
    }
    return text::write_cooked_font(content, out);
#else
    (void)font;
    (void)options;
    return fail(ErrorCode::Unsupported,
                "cooking a font needs the complete text backend; build with CY_TEXT=ON");
#endif
}

OptionsSchema font_options() noexcept {
    return OptionsSchema(Span<const OptionSpec>(kOptions));
}

ImporterInfo FontImporter::info() const noexcept {
    ImporterInfo result;
    result.name = "font";
    result.version = 1;
    result.extensions = Span<const std::string_view>(kExtensions);
    result.produces = Span<const assets::AssetKind>(kProduces);
    result.description =
        "Cooks a TrueType, OpenType, collection or WOFF font: the face as the text server will "
        "create it, its pre-rendered glyph ranges baked into atlas pages, its fallback chain, its "
        "OpenType feature defaults and its variable-font instance.";
    return result;
}

OptionsSchema FontImporter::schema() const noexcept {
    return font_options();
}

Status FontImporter::import(const ImportRequest& request, ImportResult& out) noexcept {
    if (!font_cooking_available()) {
        return error_of(out, request, "font-backend-missing",
                        "this build has no complete text backend to rasterise fonts with; "
                        "configure with -D CY_TEXT=ON");
    }
    GatheredOptions gathered;
    const char* failed = nullptr;
    if (Status read = gather(request, schema(), gathered, failed); !read) {
        return failed != nullptr ? error_of(out, request, failed, read.error().message) : read;
    }
    Array<u8> payload;
    FontCookReport report;
    if (Status cooked = cook_font(request.bytes, gathered.cook, payload, report); !cooked) {
        return error_of(out, request, "font-cook", cooked.error().message);
    }
    if (report.missing != 0) {
        char detail[128];
        (void)std::snprintf(detail, sizeof(detail),
                            "%u of %u pre-rendered codepoints are not in the font and draw as "
                            ".notdef unless a fallback has them",
                            report.missing, report.codepoints);
        if (Status reported = out.report(ImportSeverity::Warning, "font-range-missing", detail,
                                         request.source.view());
            !reported) {
            return reported;
        }
    }
    return out.add(assets::AssetKind::Font, "font", std::move(payload), true);
}

}  // namespace cy::import
