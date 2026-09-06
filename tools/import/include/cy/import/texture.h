#ifndef CY_IMPORT_TEXTURE_H
#define CY_IMPORT_TEXTURE_H
// Texture import: what an image becomes, and the mistakes an importer must catch. M5 task 5.1.
//
// `asset-import-pipeline` — "Texture import": "The texture importer SHALL produce cooked textures
// with: format selected from declared **usage** (colour, normal, data, HDR, UI) and target
// platform, a full mip chain generated in the correct colour space, and optional alpha-coverage
// preservation
// ... The importer SHALL detect common mistakes: a normal map marked sRGB, a colour texture marked
// linear, a non-power-of-two texture where the target format requires it, and unnecessary alpha
// channels."
//
// --- USAGE DECIDES, PLATFORM ADJUSTS -------------------------------------------------------------
//
// The single most consequential line in a texture importer is the one that picks a format, and the
// reason it is worth being strict about is that every wrong choice is invisible until it is
// expensive. A normal map in BC1 bands; a colour map in BC5 loses its blue channel; a mask atlas
// treated as sRGB has its midtones bent by a curve nobody asked for and the artist compensates by
// authoring a wrong texture.
//
// So `TextureUsage` is a declared property of the asset, not a guess from its name, and
// `select_format` is a pure function of (usage, has alpha, platform family). What a name CAN do is
// raise a diagnostic — a file called `rock_normal.tga` imported as `Colour` is worth a warning —
// and that is where naming conventions belong: in an advisory that a human resolves, never in the
// decision itself.
//
// --- WHAT IS HERE AND WHAT IS NOT ----------------------------------------------------------------
//
// Here: decoding, resizing, mip generation in the correct colour space, alpha-coverage
// preservation, format selection, the mistake detector, and a cooked payload carrying the mip
// chain.
//
// NOT here: the block compressors themselves. BC7 and ASTC encoding is a third-party dependency
// (`thirdparty-dependencies` names "texture encoders" among the tool-time set) and none is
// integrated at M5. `select_format` therefore names the format the cook WOULD produce, the payload
// carries uncompressed mips, and the cooked header records both — so a build that later links an
// encoder produces a different derivation key and re-cooks, rather than silently serving
// uncompressed pixels from the cache. The honest statement of that is `CookedTexture::encoded`,
// which is false today and is written into the payload.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/importer.h>

#include <string_view>

namespace cy::import {

/// What the texture is FOR. Declared by the asset, never inferred; see the header.
enum class TextureUsage : u8 {
    /// Albedo, emissive, anything a human perceives as a colour. sRGB.
    Colour = 0,
    /// A tangent-space normal map. Linear, two channels are enough, and never sRGB.
    NormalMap = 1,
    /// Roughness, metalness, occlusion, masks. Linear, and usually one channel.
    Data = 2,
    /// A high-dynamic-range image: a sky, a light probe, an emissive plate.
    Hdr = 3,
    /// Interface art. sRGB like colour, but never streamed and never resized by platform budget.
    UserInterface = 4,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* texture_usage_name(TextureUsage usage) noexcept;
[[nodiscard]] Expected<TextureUsage, Error> texture_usage_from_name(std::string_view name) noexcept;

/// The cooked pixel format a variant would use.
///
/// Persistent: the numbers reach a cooked payload, so an enumerator is appended and never
/// renumbered.
enum class TextureFormat : u16 {
    Unknown = 0,
    /// Eight bits, one channel. A mask.
    R8 = 1,
    /// Eight bits, two channels. A tangent-space normal's X and Y.
    RG8 = 2,
    /// Eight bits, four channels, no compression. The fallback and the UI format.
    RGBA8 = 3,
    /// Half float, four channels. HDR without a compressor.
    RGBA16F = 4,
    /// Desktop block compression for colour, with or without alpha.
    BC7 = 5,
    /// Desktop block compression for two-channel data — a normal map.
    BC5 = 6,
    /// Desktop block compression for one channel — a mask.
    BC4 = 7,
    /// Desktop block compression for HDR.
    BC6H = 8,
    /// Mobile block compression, four by four.
    ASTC_4x4 = 9,
    /// Mobile block compression, six by six. Half the bit rate of 4x4.
    ASTC_6x6 = 10,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* texture_format_name(TextureFormat format) noexcept;

/// Whether a format stores block-compressed data, which is what makes a non-multiple-of-four
/// dimension a problem worth a diagnostic.
[[nodiscard]] bool is_block_compressed(TextureFormat format) noexcept;

/// Which family of hardware a variant targets. Derived from the variant key rather than declared
/// separately, so a cook cannot ask for a desktop format under a mobile variant key.
enum class PlatformFamily : u8 { Desktop = 0, Mobile = 1 };

/// The family a variant key names. `desktop-*` and an empty key are desktop; `mobile-*` is mobile.
[[nodiscard]] PlatformFamily platform_family_of(std::string_view variant) noexcept;

/// Pick the cooked format.
///
/// A pure function of its three arguments, which is what makes an import reproducible and what lets
/// a test state the whole policy in a table.
[[nodiscard]] TextureFormat select_format(TextureUsage usage, bool has_alpha,
                                          PlatformFamily platform) noexcept;

/// A decoded image, eight bits per channel, top row first.
///
/// One layout rather than a general pixel-format matrix. The importer decodes into this, processes
/// in it, and encodes out of it; a general layout would mean every processing step below carried a
/// switch, and the formats that matter for a SOURCE image are all eight-bit.
struct ImageData {
    u32 width = 0;
    u32 height = 0;
    /// 1, 2, 3 or 4. Always interleaved.
    u32 channels = 0;
    Array<u8> pixels;

    [[nodiscard]] bool is_empty() const noexcept { return width == 0 || height == 0; }
    /// Whether the arrays agree with the dimensions.
    [[nodiscard]] Status validate() const noexcept;
    /// Whether any pixel's alpha is below 255. What the "unnecessary alpha channel" check asks.
    [[nodiscard]] bool has_meaningful_alpha() const noexcept;
};

/// Decode a source image.
///
/// Supports **Targa** (`.tga`), uncompressed and run-length encoded, 8, 24 and 32 bits per pixel.
/// That is the whole of it at M5 and it is a deliberate floor rather than an accident: PNG needs a
/// DEFLATE decoder and JPEG needs a DCT one, both are third-party dependencies
/// (`thirdparty-dependencies` names the tool-time set), and neither is integrated here. An
/// unsupported format fails with `Unsupported` and a message naming what would read it, so a
/// project that hits it learns which dependency to integrate rather than which file to convert.
[[nodiscard]] Expected<ImageData, Error> decode_image(Span<const u8> bytes,
                                                      std::string_view extension) noexcept;

/// A cooked texture: the header the payload begins with, then the mips back to back.
///
/// Written by `write_cooked_texture` and read by the texture loader. It is not `CookedAssetHeader`
/// — that is the loader's envelope and this is the texture kind's own payload format, which is
/// exactly the split `cooked.h` argues for.
struct CookedTexture {
    static constexpr u32 kVersion = 1;

    TextureFormat format = TextureFormat::Unknown;
    u32 width = 0;
    u32 height = 0;
    u32 mip_count = 0;
    /// True when the pixels are actually in `format`. False when `format` names what the cook WOULD
    /// have produced and the payload carries uncompressed RGBA8 because no encoder is linked. See
    /// the note at the head of this file — it is recorded rather than assumed, so nothing
    /// downstream has to guess.
    bool encoded = false;
    /// Whether the colour channels are sRGB-encoded. Alpha never is.
    bool srgb = false;
};

/// What `import_texture` decided, for the report.
struct TextureImportReport {
    u32 source_width = 0;
    u32 source_height = 0;
    u32 cooked_width = 0;
    u32 cooked_height = 0;
    u32 mip_count = 0;
    TextureFormat format = TextureFormat::Unknown;
    usize payload_bytes = 0;
    bool alpha_dropped = false;
};

/// The texture importer's option schema. A caller reads it to build a dialog or to set an option by
/// name; the importer reads it to know its own defaults.
[[nodiscard]] OptionsSchema texture_options() noexcept;

/// The built-in texture importer.
///
/// Holds no state, so one instance serves every worker; `import` is re-entrant.
class TextureImporter final : public Importer {
public:
    [[nodiscard]] ImporterInfo info() const noexcept override;
    [[nodiscard]] OptionsSchema schema() const noexcept override;
    [[nodiscard]] Status import(const ImportRequest& request, ImportResult& out) noexcept override;
};

/// Generate the mip chain for `image` in the colour space `srgb` names, appending each level's
/// pixels to `out` after the base level.
///
/// The colour space is the point of this function. Averaging four sRGB-encoded values gives a mip
/// that is too dark — the classic "textures get darker in the distance" artefact — because sRGB is
/// a curve and the average of a curve's outputs is not the curve of the average. So a colour mip is
/// decoded to linear, averaged, and re-encoded, and a data mip is averaged as it is.
///
/// Alpha is ALWAYS averaged linearly, whatever the colour channels do: alpha is coverage, not
/// colour, and running it through a gamma curve is a bug that shows up as haloes.
[[nodiscard]] Expected<u32, Error> generate_mips(const ImageData& image, bool srgb,
                                                 bool preserve_alpha_coverage,
                                                 Array<u8>& out) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_TEXTURE_H
