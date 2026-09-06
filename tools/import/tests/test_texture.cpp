// Texture import: the format policy, the colour space, and the mistakes worth catching. M5
// task 5.1.

#include <cy/core/math/scalar.h>
#include <cy/import/texture.h>
#include <cy/test/test.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace cy::import;
using cy::u32;
using cy::u8;
using cy::usize;

namespace {

/// An uncompressed 32-bit Targa with a top-left origin, built by hand.
///
/// The header is written out rather than taken from a fixture file for the reason every format test
/// in this tree gives: a fixture is a file somebody has to keep, and a header written here says
/// what each byte means.
std::vector<u8> targa(u32 width, u32 height, bool top_origin, const std::vector<u8>& bgra_pixels) {
    std::vector<u8> bytes(18, 0);
    bytes[2] = 2;  // uncompressed true colour
    bytes[12] = static_cast<u8>(width & 0xFFU);
    bytes[13] = static_cast<u8>((width >> 8U) & 0xFFU);
    bytes[14] = static_cast<u8>(height & 0xFFU);
    bytes[15] = static_cast<u8>((height >> 8U) & 0xFFU);
    bytes[16] = 32;
    bytes[17] = top_origin ? 0x20 : 0x00;
    bytes.insert(bytes.end(), bgra_pixels.begin(), bgra_pixels.end());
    return bytes;
}

/// A solid image, in Targa's blue-first channel order.
std::vector<u8> solid(u32 width, u32 height, u8 red, u8 green, u8 blue, u8 alpha) {
    std::vector<u8> pixels;
    pixels.reserve(static_cast<usize>(width) * height * 4);
    for (usize index = 0; index < static_cast<usize>(width) * height; ++index) {
        pixels.push_back(blue);
        pixels.push_back(green);
        pixels.push_back(red);
        pixels.push_back(alpha);
    }
    return pixels;
}

ImportResult import_targa(const std::vector<u8>& bytes, const ImportOptions* options,
                          std::string_view variant = "") {
    TextureImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("textures/stone.tga").value();
    request.bytes = cy::Span<const u8>(bytes.data(), bytes.size());
    request.options = options;
    if (!variant.empty()) {
        request.variant = cy::assets::VariantKey::parse(variant).value();
    }
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    return result;
}

bool reported(const ImportResult& result, std::string_view code) {
    return std::ranges::any_of(result.diagnostics(),
                               [code](const ImportDiagnostic& diagnostic) noexcept {
                                   return std::string_view(diagnostic.code) == code;
                               });
}

}  // namespace

CY_TEST_CASE("texture: a Targa decodes, and the origin bit is honoured") {
    // The commonest Targa defect there is: bit 5 clear means the first row in the file is the
    // BOTTOM row, and getting it wrong flips every texture in a project.
    std::vector<u8> pixels = solid(2, 2, 0, 0, 0, 255);
    // Make the top-left pixel red and the rest black, so a flip is visible.
    pixels[2] = 255;
    const std::vector<u8> top = targa(2, 2, true, pixels);
    const std::vector<u8> bottom = targa(2, 2, false, pixels);

    auto decoded_top = decode_image(cy::Span<const u8>(top.data(), top.size()), ".tga");
    CY_REQUIRE(decoded_top.has_value());
    CY_CHECK(decoded_top.value().width == 2);
    CY_CHECK(decoded_top.value().channels == 4);
    // Channel order is red-first after decoding, whatever Targa stored.
    CY_CHECK(decoded_top.value().pixels[0] == 255);

    auto decoded_bottom = decode_image(cy::Span<const u8>(bottom.data(), bottom.size()), ".tga");
    CY_REQUIRE(decoded_bottom.has_value());
    // The same file with the origin bit clear puts that red pixel on the last row.
    CY_CHECK(decoded_bottom.value().pixels[0] == 0);
    CY_CHECK(decoded_bottom.value().pixels[(2 * 4) + 0] == 255);
}

CY_TEST_CASE("texture: a format this build cannot read says which dependency would read it") {
    const std::vector<u8> nothing(64, 0);
    auto refused = decode_image(cy::Span<const u8>(nothing.data(), nothing.size()), ".png");
    CY_REQUIRE(!refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);
    CY_CHECK(std::string_view(refused.error().message).find("DEFLATE") != std::string_view::npos);
}

CY_TEST_CASE("texture: usage and platform decide the format, and nothing else does") {
    // The whole policy as a table, which is what makes it reviewable. Every wrong choice here is
    // invisible until it is expensive: a normal map in a colour format bands, a colour map in a
    // two-channel format loses its blue.
    CY_CHECK(select_format(TextureUsage::Colour, true, PlatformFamily::Desktop) ==
             TextureFormat::BC7);
    CY_CHECK(select_format(TextureUsage::NormalMap, false, PlatformFamily::Desktop) ==
             TextureFormat::BC5);
    CY_CHECK(select_format(TextureUsage::Data, false, PlatformFamily::Desktop) ==
             TextureFormat::BC4);
    CY_CHECK(select_format(TextureUsage::Data, true, PlatformFamily::Desktop) ==
             TextureFormat::BC7);
    CY_CHECK(select_format(TextureUsage::Hdr, false, PlatformFamily::Desktop) ==
             TextureFormat::BC6H);
    // Interface art is never block compressed: it is read at one-to-one scale, where a block
    // artefact is a visible seam rather than an imperceptible error.
    CY_CHECK(select_format(TextureUsage::UserInterface, true, PlatformFamily::Desktop) ==
             TextureFormat::RGBA8);
    CY_CHECK(select_format(TextureUsage::Colour, true, PlatformFamily::Mobile) ==
             TextureFormat::ASTC_4x4);
    CY_CHECK(select_format(TextureUsage::Data, false, PlatformFamily::Mobile) ==
             TextureFormat::ASTC_6x6);
}

CY_TEST_CASE("texture: the variant key decides the platform family") {
    // "WHEN a texture is cooked for desktop and mobile THEN BC7 and ASTC variants SHALL be
    // produced" — and the variant key is the whole of how the two cooks differ.
    CY_CHECK(platform_family_of("desktop-bc7") == PlatformFamily::Desktop);
    CY_CHECK(platform_family_of("mobile-astc") == PlatformFamily::Mobile);
    CY_CHECK(platform_family_of("") == PlatformFamily::Desktop);
}

CY_TEST_CASE("texture: a mip chain reaches one by one and averages in the right space") {
    ImageData image;
    image.width = 4;
    image.height = 4;
    image.channels = 4;
    CY_REQUIRE(image.pixels.resize(static_cast<usize>(4) * 4 * 4).has_value());
    // Half the pixels black and half white, so the average is exactly the midpoint and the colour
    // space is visible in the result.
    for (usize index = 0; index < 16; ++index) {
        const u8 value = (index % 2) == 0 ? 0 : 255;
        for (usize channel = 0; channel < 3; ++channel) {
            image.pixels[(index * 4) + channel] = value;
        }
        image.pixels[(index * 4) + 3] = 255;
    }

    cy::Array<u8> linear_levels;
    auto linear_mips = generate_mips(image, false, false, linear_levels);
    CY_REQUIRE(linear_mips.has_value());
    CY_CHECK(linear_mips.value() == 3);  // 4x4, 2x2, 1x1

    cy::Array<u8> srgb_levels;
    auto srgb_mips = generate_mips(image, true, false, srgb_levels);
    CY_REQUIRE(srgb_mips.has_value());
    CY_CHECK(srgb_mips.value() == 3);

    // The first mip of a linear average of 0 and 255 is 128 (near enough); the sRGB-correct average
    // of the same two values is markedly brighter, because averaging encoded values is what makes
    // textures go dark in the distance.
    const usize first_mip = static_cast<usize>(4) * 4 * 4;
    CY_CHECK(linear_levels[first_mip] < srgb_levels[first_mip]);
    CY_CHECK(srgb_levels[first_mip] > 180);
}

CY_TEST_CASE("texture: alpha coverage is preserved when asked for") {
    // Foliage cut-outs thin out and vanish in the distance without this, because a box filter over
    // a mostly-transparent neighbourhood produces alpha below the test threshold.
    ImageData image;
    image.width = 8;
    image.height = 8;
    image.channels = 4;
    CY_REQUIRE(image.pixels.resize(static_cast<usize>(8) * 8 * 4).has_value());
    for (usize index = 0; index < 64; ++index) {
        for (usize channel = 0; channel < 3; ++channel) {
            image.pixels[(index * 4) + channel] = 255;
        }
        // A sparse lattice of opaque pixels in a transparent field, which is what a foliage cut-out
        // looks like: one pixel in sixteen survives, and a box filter drops every one of them below
        // the alpha-test threshold.
        const usize x = index % 8;
        const usize y = index / 8;
        image.pixels[(index * 4) + 3] = (x % 4) == 0 && (y % 4) == 0 ? 255 : 0;
    }

    const auto coverage_of = [](const cy::Array<u8>& levels, usize offset, usize count) {
        usize passing = 0;
        for (usize index = 0; index < count; ++index) {
            passing += levels[offset + (index * 4) + 3] >= 128 ? 1U : 0U;
        }
        return static_cast<float>(passing) / static_cast<float>(count);
    };

    cy::Array<u8> plain;
    CY_REQUIRE(generate_mips(image, false, false, plain).has_value());
    cy::Array<u8> preserved;
    CY_REQUIRE(generate_mips(image, false, true, preserved).has_value());

    const usize base_bytes = static_cast<usize>(8) * 8 * 4;
    const usize mip_pixels = static_cast<usize>(4) * 4;
    const float plain_coverage = coverage_of(plain, base_bytes, mip_pixels);
    const float preserved_coverage = coverage_of(preserved, base_bytes, mip_pixels);
    // Without preservation the first mip has NO pixel passing the threshold — the cut-out has
    // vanished one level down, which is the artefact in one number.
    CY_CHECK(plain_coverage == 0.0f);
    CY_CHECK(preserved_coverage > plain_coverage);
}

CY_TEST_CASE("texture: a normal map marked sRGB is reported with the setting that would fix it") {
    // `asset-import-pipeline`: "WHEN a texture bound to a roughness slot is flagged sRGB THEN the
    // importer SHALL warn with the specific slot and the likely-intended setting."
    const std::vector<u8> bytes = targa(4, 4, true, solid(4, 4, 128, 128, 255, 255));
    ImportOptions options;
    const OptionsSchema schema = texture_options();
    CY_REQUIRE(options.set(schema, "usage", OptionValue::of_enumeration("normal-map")).has_value());
    // srgb is left at its default of true, which for a normal map is the mistake.
    const ImportResult result = import_targa(bytes, &options);
    CY_CHECK(reported(result, "srgb-normal-map"));
    CY_CHECK(result.warning_count() >= 1);
    CY_CHECK(!result.has_errors());
}

CY_TEST_CASE("texture: a colour texture marked linear and a data texture marked sRGB") {
    const std::vector<u8> bytes = targa(4, 4, true, solid(4, 4, 200, 100, 50, 255));
    const OptionsSchema schema = texture_options();

    ImportOptions linear_colour;
    CY_REQUIRE(linear_colour.set(schema, "srgb", OptionValue::of_bool(false)).has_value());
    CY_CHECK(reported(import_targa(bytes, &linear_colour), "linear-colour-texture"));

    ImportOptions srgb_data;
    CY_REQUIRE(srgb_data.set(schema, "usage", OptionValue::of_enumeration("data")).has_value());
    CY_CHECK(reported(import_targa(bytes, &srgb_data), "srgb-data-texture"));
}

CY_TEST_CASE("texture: an alpha channel that is entirely opaque is reported as waste") {
    const std::vector<u8> bytes = targa(4, 4, true, solid(4, 4, 10, 20, 30, 255));
    const ImportResult result = import_targa(bytes, nullptr);
    CY_CHECK(reported(result, "unnecessary-alpha"));
}

CY_TEST_CASE("texture: a dimension a block format cannot tile is reported") {
    const std::vector<u8> bytes = targa(6, 6, true, solid(6, 6, 10, 20, 30, 128));
    const ImportResult result = import_targa(bytes, nullptr);
    CY_CHECK(reported(result, "block-size-mismatch"));
}

CY_TEST_CASE("texture: an unreadable source is a diagnostic on the asset, not a failed run") {
    // A project with one broken texture must still cook the other nine hundred.
    const std::vector<u8> truncated(8, 0);
    const ImportResult result = import_targa(truncated, nullptr);
    CY_CHECK(result.has_errors());
    CY_CHECK(reported(result, "unreadable-source"));
    CY_CHECK(result.assets().empty());
}

CY_TEST_CASE("texture: the maximum resolution halves the image until it fits") {
    const std::vector<u8> bytes = targa(16, 16, true, solid(16, 16, 10, 20, 30, 200));
    ImportOptions options;
    const OptionsSchema schema = texture_options();
    CY_REQUIRE(options.set(schema, "max-resolution", OptionValue::of_int(4)).has_value());
    CY_REQUIRE(options.set(schema, "generate-mips", OptionValue::of_bool(false)).has_value());

    const ImportResult result = import_targa(bytes, &options);
    CY_REQUIRE(result.assets().size() == 1);
    // The payload's header records the cooked dimensions: sixteen halved twice is four.
    const cy::Span<const u8> payload = result.assets()[0].payload;
    CY_REQUIRE(payload.size() > 20);
    const u32 width = payload[8] | (static_cast<u32>(payload[9]) << 8U);
    CY_CHECK(width == 4);
}

CY_TEST_CASE("texture: an import is a pure function of its inputs") {
    // The determinism `asset-import-pipeline` requires: "WHEN the same source and options are
    // imported twice THEN the cooked output SHALL be byte-identical."
    const std::vector<u8> bytes = targa(8, 8, true, solid(8, 8, 30, 60, 90, 255));
    const ImportResult first = import_targa(bytes, nullptr);
    const ImportResult second = import_targa(bytes, nullptr);
    CY_REQUIRE(first.assets().size() == 1);
    CY_REQUIRE(second.assets().size() == 1);
    CY_REQUIRE(first.assets()[0].payload.size() == second.assets()[0].payload.size());
    for (usize index = 0; index < first.assets()[0].payload.size(); ++index) {
        CY_CHECK(first.assets()[0].payload[index] == second.assets()[0].payload[index]);
    }
}
