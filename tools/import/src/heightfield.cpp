// SPDX-License-Identifier: MIT
#include <cy/import/heightfield.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace cy::import {
namespace {

constexpr std::string_view kSampleFormats[] = {"unspecified", "u16-le", "i16-le", "f32-le"};
constexpr OptionSpec kOptions[] = {
    {"resolution-x",
     OptionType::Int,
     OptionValue::of_int(0),
     "Number of height samples along X. Required because a raw grid does not encode dimensions.",
     {},
     0.0,
     65537.0},
    {"resolution-z",
     OptionType::Int,
     OptionValue::of_int(0),
     "Number of height samples along Z. Required because a raw grid does not encode dimensions.",
     {},
     0.0,
     65537.0},
    {"sample-format", OptionType::Enumeration, OptionValue::of_enumeration("unspecified"),
     "Stored sample signedness and byte order. Must be declared; it is never guessed.",
     Span<const std::string_view>(kSampleFormats), 0.0, 0.0},
    {"sample-metres",
     OptionType::Float,
     OptionValue::of_float(0.0),
     "Horizontal distance in metres between adjacent samples. Must be greater than zero.",
     {},
     0.0,
     1000000.0},
    {"height-min-metres",
     OptionType::Float,
     OptionValue::of_float(0.0),
     "World-space height represented by the lowest stored sample.",
     {},
     -1000000.0,
     1000000.0},
    {"height-max-metres",
     OptionType::Float,
     OptionValue::of_float(0.0),
     "World-space height represented by the highest stored sample; must exceed the minimum.",
     {},
     -1000000.0,
     1000000.0},
    {"tile-quads",
     OptionType::Int,
     OptionValue::of_int(64),
     "Quads on one tile edge. CyberTerrain currently requires 64 and shared boundary samples.",
     {},
     1.0,
     4096.0},
};
constexpr std::string_view kExtensions[] = {".r16", ".raw"};
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Terrain};

void write_u32(u8* destination, u32 value) noexcept {
    for (u32 lane = 0; lane < 4; ++lane) {
        destination[lane] = static_cast<u8>((value >> (lane * 8U)) & 0xFFU);
    }
}

[[nodiscard]] u32 read_u32(const u8* source) noexcept {
    return static_cast<u32>(source[0]) | (static_cast<u32>(source[1]) << 8U) |
           (static_cast<u32>(source[2]) << 16U) | (static_cast<u32>(source[3]) << 24U);
}

void write_f32(u8* destination, f32 value) noexcept {
    write_u32(destination, std::bit_cast<u32>(value));
}

[[nodiscard]] f32 read_f32(const u8* source) noexcept {
    return std::bit_cast<f32>(read_u32(source));
}

[[nodiscard]] Status diagnostic(ImportResult& out, const char* code, std::string_view detail,
                                const ImportRequest& request) noexcept {
    return out.report(ImportSeverity::Error, code, detail, request.source.view());
}

[[nodiscard]] u32 bytes_per_sample(std::string_view format) noexcept {
    return format == "f32-le" ? 4U : 2U;
}

[[nodiscard]] u16 normalize_sample(const u8* source, std::string_view format,
                                   bool& valid) noexcept {
    if (format == "u16-le") {
        return static_cast<u16>(static_cast<u16>(source[0]) |
                                static_cast<u16>(static_cast<u16>(source[1]) << 8U));
    }
    if (format == "i16-le") {
        const u16 bits = static_cast<u16>(static_cast<u16>(source[0]) |
                                          static_cast<u16>(static_cast<u16>(source[1]) << 8U));
        const i16 value = std::bit_cast<i16>(bits);
        return static_cast<u16>(static_cast<i32>(value) -
                                static_cast<i32>(std::numeric_limits<i16>::min()));
    }
    const f32 value = read_f32(source);
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        valid = false;
        return 0;
    }
    return static_cast<u16>(std::lround(value * 65535.0F));
}

}  // namespace

OptionsSchema heightfield_options() noexcept {
    return OptionsSchema(Span<const OptionSpec>(kOptions));
}

ImporterInfo HeightfieldImporter::info() const noexcept {
    ImporterInfo result;
    result.name = "heightfield";
    result.version = 1;
    result.extensions = Span<const std::string_view>(kExtensions);
    result.produces = Span<const assets::AssetKind>(kProduces);
    result.description =
        "Imports an explicitly described single-channel height grid into a versioned, tiled "
        "CyberTerrain source asset without guessing dimensions, signedness, units, or range.";
    return result;
}

OptionsSchema HeightfieldImporter::schema() const noexcept {
    return heightfield_options();
}

Status HeightfieldImporter::import(const ImportRequest& request, ImportResult& out) noexcept {
    const OptionsSchema options = schema();
    auto width = request.option(options, "resolution-x");
    auto height = request.option(options, "resolution-z");
    auto format = request.option(options, "sample-format");
    auto spacing = request.option(options, "sample-metres");
    auto minimum = request.option(options, "height-min-metres");
    auto maximum = request.option(options, "height-max-metres");
    auto tile = request.option(options, "tile-quads");
    if (!width || !height || !format || !spacing || !minimum || !maximum || !tile) {
        return fail(ErrorCode::Internal,
                    "the heightfield importer's option schema is inconsistent");
    }
    if (width.value().as_int() < 2 || height.value().as_int() < 2 ||
        spacing.value().as_float() <= 0.0 || format.value().as_text() == "unspecified") {
        return diagnostic(out, "heightfield-metadata-required",
                          "declare resolution-x, resolution-z, sample-format, and positive "
                          "sample-metres; raw heightfields do not encode them",
                          request);
    }
    if (maximum.value().as_float() <= minimum.value().as_float()) {
        return diagnostic(out, "heightfield-range",
                          "height-max-metres must be greater than height-min-metres; the vertical "
                          "range is never inferred from samples",
                          request);
    }
    constexpr i64 kRuntimeTileQuads = 64;
    const i64 tile_quads = tile.value().as_int();
    const i64 width_samples = width.value().as_int();
    const i64 height_samples = height.value().as_int();
    if (tile_quads != kRuntimeTileQuads || ((width_samples - 1) % tile_quads) != 0 ||
        ((height_samples - 1) % tile_quads) != 0) {
        return diagnostic(out, "heightfield-tiling",
                          "tile-quads must be 64 and each resolution minus one must be divisible "
                          "by 64 so adjacent tiles share their boundary samples",
                          request);
    }

    const usize sample_count =
        static_cast<usize>(width_samples) * static_cast<usize>(height_samples);
    const usize expected_bytes = sample_count * bytes_per_sample(format.value().as_text());
    if (request.bytes.size() != expected_bytes) {
        return diagnostic(out, "heightfield-size",
                          "source byte count does not match the declared resolution and sample "
                          "format",
                          request);
    }

    Array<u8> payload;
    if (Status resized = payload.resize(CookedHeightfield::kHeaderBytes + (sample_count * 2U));
        !resized) {
        return resized;
    }
    write_u32(payload.data(), CookedHeightfield::kVersion);
    write_u32(payload.data() + 4, static_cast<u32>(width_samples));
    write_u32(payload.data() + 8, static_cast<u32>(height_samples));
    write_u32(payload.data() + 12, static_cast<u32>(tile_quads));
    write_u32(payload.data() + 16, static_cast<u32>((width_samples - 1) / tile_quads));
    write_u32(payload.data() + 20, static_cast<u32>((height_samples - 1) / tile_quads));
    write_f32(payload.data() + 24, static_cast<f32>(spacing.value().as_float()));
    write_f32(payload.data() + 28, static_cast<f32>(minimum.value().as_float()));
    write_f32(payload.data() + 32, static_cast<f32>(maximum.value().as_float()));
    write_u32(payload.data() + 36, 0U);

    const u32 stride = bytes_per_sample(format.value().as_text());
    bool samples_valid = true;
    for (usize index = 0; index < sample_count; ++index) {
        const u16 normalized = normalize_sample(request.bytes.data() + (index * stride),
                                                format.value().as_text(), samples_valid);
        payload[CookedHeightfield::kHeaderBytes + (index * 2U)] =
            static_cast<u8>(normalized & 0xFFU);
        payload[CookedHeightfield::kHeaderBytes + (index * 2U) + 1U] =
            static_cast<u8>((normalized >> 8U) & 0xFFU);
    }
    if (!samples_valid) {
        return diagnostic(out, "heightfield-sample-range",
                          "f32-le height samples must be finite normalized values in [0, 1]",
                          request);
    }
    return out.add(assets::AssetKind::Terrain, "terrain", std::move(payload), true);
}

Expected<CookedHeightfield, Error> read_cooked_heightfield(Span<const u8> payload) noexcept {
    if (payload.size() < CookedHeightfield::kHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "a cooked heightfield header is truncated");
    }
    if (read_u32(payload.data()) != CookedHeightfield::kVersion) {
        return fail(ErrorCode::Unsupported, "unsupported cooked heightfield schema version");
    }
    CookedHeightfield result;
    result.width = read_u32(payload.data() + 4);
    result.height = read_u32(payload.data() + 8);
    result.tile_quads = read_u32(payload.data() + 12);
    result.tiles_x = read_u32(payload.data() + 16);
    result.tiles_z = read_u32(payload.data() + 20);
    result.sample_metres = read_f32(payload.data() + 24);
    result.height_min_metres = read_f32(payload.data() + 28);
    result.height_max_metres = read_f32(payload.data() + 32);
    const usize samples = static_cast<usize>(result.width) * result.height;
    if (result.width < 2 || result.height < 2 || result.tile_quads == 0 ||
        payload.size() != CookedHeightfield::kHeaderBytes + (samples * 2U)) {
        return fail(ErrorCode::InvalidArgument, "a cooked heightfield payload is inconsistent");
    }
    return result;
}

}  // namespace cy::import
