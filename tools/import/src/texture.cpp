#include <cy/import/texture.h>

#include <cmath>
#include <cstring>

namespace cy::import {
namespace {

// --- Options -------------------------------------------------------------------------------------

constexpr std::string_view kUsageChoices[] = {"colour", "normal-map", "data", "hdr", "ui"};
constexpr std::string_view kAlphaChoices[] = {"opaque", "straight", "premultiplied"};
constexpr std::string_view kWrapChoices[] = {"repeat", "clamp", "mirror"};
constexpr std::string_view kNormalConventionChoices[] = {"opengl", "directx"};

constexpr OptionSpec kTextureOptions[] = {
    {"usage", OptionType::Enumeration, OptionValue::of_enumeration("colour"),
     "What the texture is for. Decides the cooked format and the colour space, and is declared "
     "rather than guessed from the file's name.",
     Span<const std::string_view>(kUsageChoices), 0.0, 0.0},
    {"srgb",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether the source pixels are sRGB-encoded. Meaningful for colour and interface art; a "
     "normal "
     "map or a mask that claims sRGB is a mistake the importer reports.",
     {},
     0.0,
     0.0},
    {"max-resolution",
     OptionType::Int,
     OptionValue::of_int(4096),
     "The longest edge the cooked texture may have. A larger source is halved until it fits, which "
     "is how a platform budget is applied without editing the source.",
     {},
     1.0,
     16384.0},
    {"generate-mips",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce the full mip chain. Off for an interface texture drawn at one size, where "
     "the chain is memory nothing samples.",
     {},
     0.0,
     0.0},
    {"preserve-alpha-coverage",
     OptionType::Bool,
     OptionValue::of_bool(false),
     "Rescale each mip's alpha so that the share of pixels passing an alpha-test threshold matches "
     "the base level. Foliage cut-outs thin out and disappear in the distance without it.",
     {},
     0.0,
     0.0},
    {"alpha", OptionType::Enumeration, OptionValue::of_enumeration("straight"),
     "How the alpha channel relates to the colour channels: opaque (ignore it), straight, or "
     "already "
     "premultiplied.",
     Span<const std::string_view>(kAlphaChoices), 0.0, 0.0},
    {"wrap", OptionType::Enumeration, OptionValue::of_enumeration("repeat"),
     "How sampling behaves outside [0, 1]. Recorded with the cooked texture so a material need not "
     "restate it.",
     Span<const std::string_view>(kWrapChoices), 0.0, 0.0},
    {"normal-convention", OptionType::Enumeration, OptionValue::of_enumeration("opengl"),
     "Which way the green channel of a normal map points. DirectX-orientation maps are converted "
     "at "
     "import so that every mesh's generated tangents agree with every normal map.",
     Span<const std::string_view>(kNormalConventionChoices), 0.0, 0.0},
    {"streamable",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether the cooked texture may have its high mips streamed in on demand rather than being "
     "resident. Interface art and small masks are cheaper resident.",
     {},
     0.0,
     0.0},
};

constexpr std::string_view kExtensions[] = {".tga"};
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Texture};

// --- Colour --------------------------------------------------------------------------------------

/// sRGB to linear, on the [0, 1] scale. The piecewise definition rather than a 2.2 power: the
/// difference is only near black, and near black is exactly where a mip average is visibly wrong.
[[nodiscard]] f32 srgb_to_linear(f32 value) noexcept {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] f32 linear_to_srgb(f32 value) noexcept {
    return value <= 0.0031308f ? value * 12.92f : (1.055f * std::pow(value, 1.0f / 2.4f)) - 0.055f;
}

[[nodiscard]] u8 to_byte(f32 value) noexcept {
    f32 clamped = value;
    clamped = clamped < 0.0f ? 0.0f : clamped;
    clamped = clamped > 1.0f ? 1.0f : clamped;
    // `lround` rather than adding a half and truncating: the latter rounds a negative wrongly and
    // is the classic defect the check that flagged it exists for. Nothing here is negative, and a
    // rounding that is only correct because of an invariant elsewhere is one that breaks later.
    return static_cast<u8>(std::lround(clamped * 255.0f));
}

/// The share of pixels whose alpha passes the usual alpha-test threshold. What alpha-coverage
/// preservation matches across the chain.
constexpr f32 kAlphaTestThreshold = 0.5f;

[[nodiscard]] f32 alpha_coverage(const u8* pixels, usize pixel_count, u32 channels,
                                 f32 scale) noexcept {
    if (channels < 4 || pixel_count == 0) {
        return 1.0f;
    }
    usize passing = 0;
    for (usize index = 0; index < pixel_count; ++index) {
        const f32 alpha = (static_cast<f32>(pixels[(index * channels) + 3]) / 255.0f) * scale;
        passing += alpha >= kAlphaTestThreshold ? 1U : 0U;
    }
    return static_cast<f32>(passing) / static_cast<f32>(pixel_count);
}

// --- Targa ---------------------------------------------------------------------------------------

/// Read a Targa image. Uncompressed (type 2 and 3) and run-length encoded (type 10 and 11), at 8,
/// 24 and 32 bits per pixel.
///
/// Targa rather than PNG for the reason the header gives, and it is enough to exercise every step
/// that follows: it carries alpha, it carries a single channel, and it has an origin bit that is
/// the commonest way an importer ends up flipping everybody's textures.
[[nodiscard]] Expected<ImageData, Error> decode_targa(Span<const u8> bytes) noexcept {
    constexpr usize kHeaderBytes = 18;
    if (bytes.size() < kHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "shorter than a Targa header");
    }
    const u8 id_length = bytes[0];
    const u8 colour_map_type = bytes[1];
    const u8 image_type = bytes[2];
    const auto width = static_cast<u32>(bytes[12] | (static_cast<u32>(bytes[13]) << 8U));
    const auto height = static_cast<u32>(bytes[14] | (static_cast<u32>(bytes[15]) << 8U));
    const u8 bits = bytes[16];
    const u8 descriptor = bytes[17];

    if (colour_map_type != 0) {
        return fail(ErrorCode::Unsupported, "a colour-mapped Targa; export it as true colour");
    }
    if (image_type != 2 && image_type != 3 && image_type != 10 && image_type != 11) {
        return fail(ErrorCode::Unsupported, "a Targa image type this importer does not read");
    }
    if (bits != 8 && bits != 24 && bits != 32) {
        return fail(ErrorCode::Unsupported, "a Targa depth other than 8, 24 or 32 bits");
    }
    if (width == 0 || height == 0) {
        return fail(ErrorCode::InvalidArgument, "a Targa with a zero dimension");
    }

    const u32 channels = bits / 8U;
    const usize pixel_count = static_cast<usize>(width) * height;
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    if (Status resized = image.pixels.resize(pixel_count * channels); !resized) {
        return make_unexpected(resized.error());
    }

    usize cursor = kHeaderBytes + id_length;
    const bool compressed = image_type >= 10;
    usize written = 0;
    while (written < pixel_count) {
        if (compressed) {
            if (cursor >= bytes.size()) {
                return fail(ErrorCode::InvalidArgument, "a Targa that ends inside its pixel data");
            }
            const u8 packet = bytes[cursor++];
            const usize run = static_cast<usize>(packet & 0x7FU) + 1;
            if (written + run > pixel_count) {
                return fail(ErrorCode::InvalidArgument, "a Targa run past the end of the image");
            }
            if ((packet & 0x80U) != 0) {
                if (cursor + channels > bytes.size()) {
                    return fail(ErrorCode::InvalidArgument,
                                "a Targa that ends inside its pixel data");
                }
                for (usize repeat = 0; repeat < run; ++repeat) {
                    std::memcpy(image.pixels.data() + ((written + repeat) * channels),
                                bytes.data() + cursor, channels);
                }
                cursor += channels;
            } else {
                if (cursor + (run * channels) > bytes.size()) {
                    return fail(ErrorCode::InvalidArgument,
                                "a Targa that ends inside its pixel data");
                }
                std::memcpy(image.pixels.data() + (written * channels), bytes.data() + cursor,
                            run * channels);
                cursor += run * channels;
            }
            written += run;
        } else {
            if (cursor + (pixel_count * channels) > bytes.size()) {
                return fail(ErrorCode::InvalidArgument, "a Targa that ends inside its pixel data");
            }
            std::memcpy(image.pixels.data(), bytes.data() + cursor, pixel_count * channels);
            written = pixel_count;
        }
    }

    // Targa stores blue first. Swap into the engine's channel order in place.
    if (channels >= 3) {
        for (usize index = 0; index < pixel_count; ++index) {
            u8* pixel = image.pixels.data() + (index * channels);
            const u8 blue = pixel[0];
            pixel[0] = pixel[2];
            pixel[2] = blue;
        }
    }

    // Bit 5 of the descriptor sets the origin: clear means the first row in the file is the BOTTOM
    // row. Getting this wrong flips every texture in a project and is the commonest Targa defect,
    // which is why it is handled rather than assumed.
    const bool top_origin = (descriptor & 0x20U) != 0;
    if (!top_origin) {
        const usize row_bytes = static_cast<usize>(width) * channels;
        for (u32 row = 0; row < height / 2; ++row) {
            u8* top = image.pixels.data() + (static_cast<usize>(row) * row_bytes);
            u8* bottom = image.pixels.data() + (static_cast<usize>(height - 1 - row) * row_bytes);
            for (usize byte = 0; byte < row_bytes; ++byte) {
                const u8 held = top[byte];
                top[byte] = bottom[byte];
                bottom[byte] = held;
            }
        }
    }
    return image;
}

/// Halve an image's dimensions with a box filter, in the colour space `srgb` names.
[[nodiscard]] Expected<ImageData, Error> downsample(const ImageData& source, bool srgb) noexcept {
    ImageData result;
    result.width = source.width > 1 ? source.width / 2 : 1;
    result.height = source.height > 1 ? source.height / 2 : 1;
    result.channels = source.channels;
    if (Status resized = result.pixels.resize(static_cast<usize>(result.width) * result.height *
                                              result.channels);
        !resized) {
        return make_unexpected(resized.error());
    }

    for (u32 y = 0; y < result.height; ++y) {
        for (u32 x = 0; x < result.width; ++x) {
            for (u32 channel = 0; channel < source.channels; ++channel) {
                // Alpha is coverage rather than colour and is always averaged linearly. Running it
                // through a gamma curve is a defect that shows up as haloes around cut-outs.
                const bool linear_channel = !srgb || channel >= 3;
                f32 total = 0.0f;
                u32 taps = 0;
                for (u32 dy = 0; dy < 2; ++dy) {
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 sx =
                            (x * 2) + dx < source.width ? (x * 2) + dx : source.width - 1;
                        const u32 sy =
                            (y * 2) + dy < source.height ? (y * 2) + dy : source.height - 1;
                        const usize offset =
                            (((static_cast<usize>(sy) * source.width) + sx) * source.channels) +
                            channel;
                        const f32 value = static_cast<f32>(source.pixels[offset]) / 255.0f;
                        total += linear_channel ? value : srgb_to_linear(value);
                        ++taps;
                    }
                }
                const f32 average = total / static_cast<f32>(taps);
                const usize offset =
                    (((static_cast<usize>(y) * result.width) + x) * result.channels) + channel;
                result.pixels[offset] = to_byte(linear_channel ? average : linear_to_srgb(average));
            }
        }
    }
    return result;
}

/// Scale every alpha value by `scale`, clamped. Used by coverage preservation.
void scale_alpha(ImageData& image, f32 scale) noexcept {
    if (image.channels < 4) {
        return;
    }
    const usize pixel_count = static_cast<usize>(image.width) * image.height;
    for (usize index = 0; index < pixel_count; ++index) {
        u8& alpha = image.pixels[(index * image.channels) + 3];
        alpha = to_byte((static_cast<f32>(alpha) / 255.0f) * scale);
    }
}

}  // namespace

// --- Names ---------------------------------------------------------------------------------------

const char* texture_usage_name(TextureUsage usage) noexcept {
    switch (usage) {
        case TextureUsage::Colour:
            return "colour";
        case TextureUsage::NormalMap:
            return "normal-map";
        case TextureUsage::Data:
            return "data";
        case TextureUsage::Hdr:
            return "hdr";
        case TextureUsage::UserInterface:
            return "ui";
    }
    return "colour";
}

Expected<TextureUsage, Error> texture_usage_from_name(std::string_view name) noexcept {
    constexpr TextureUsage kAll[] = {TextureUsage::Colour, TextureUsage::NormalMap,
                                     TextureUsage::Data, TextureUsage::Hdr,
                                     TextureUsage::UserInterface};
    for (const TextureUsage usage : kAll) {
        if (name == texture_usage_name(usage)) {
            return usage;
        }
    }
    return fail(ErrorCode::InvalidArgument, "not a texture usage this build defines");
}

const char* texture_format_name(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::Unknown:
            return "unknown";
        case TextureFormat::R8:
            return "r8";
        case TextureFormat::RG8:
            return "rg8";
        case TextureFormat::RGBA8:
            return "rgba8";
        case TextureFormat::RGBA16F:
            return "rgba16f";
        case TextureFormat::BC7:
            return "bc7";
        case TextureFormat::BC5:
            return "bc5";
        case TextureFormat::BC4:
            return "bc4";
        case TextureFormat::BC6H:
            return "bc6h";
        case TextureFormat::ASTC_4x4:
            return "astc-4x4";
        case TextureFormat::ASTC_6x6:
            return "astc-6x6";
    }
    return "unknown";
}

bool is_block_compressed(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::BC7:
        case TextureFormat::BC5:
        case TextureFormat::BC4:
        case TextureFormat::BC6H:
        case TextureFormat::ASTC_4x4:
        case TextureFormat::ASTC_6x6:
            return true;
        default:
            return false;
    }
}

PlatformFamily platform_family_of(std::string_view variant) noexcept {
    return variant.starts_with("mobile") ? PlatformFamily::Mobile : PlatformFamily::Desktop;
}

TextureFormat select_format(TextureUsage usage, bool has_alpha, PlatformFamily platform) noexcept {
    if (platform == PlatformFamily::Mobile) {
        switch (usage) {
            // ASTC 4x4 for anything whose detail matters, 6x6 for data whose error budget is
            // wider. Colour and a normal map take the same format for different reasons — one needs
            // the chroma and the other the precision — and they are one case here rather than two
            // because a switch with two identical arms is a switch a reader has to check twice.
            case TextureUsage::Colour:
            case TextureUsage::NormalMap:
                return TextureFormat::ASTC_4x4;
            case TextureUsage::Data:
                return TextureFormat::ASTC_6x6;
            case TextureUsage::Hdr:
                return TextureFormat::RGBA16F;
            // Interface art is never block compressed: it is read at one-to-one scale where a
            // block artefact is a visible seam rather than an imperceptible error.
            case TextureUsage::UserInterface:
                return TextureFormat::RGBA8;
        }
        return TextureFormat::RGBA8;
    }
    switch (usage) {
        case TextureUsage::Colour:
            return TextureFormat::BC7;
        // BC5 stores two channels at full quality, which is exactly a tangent-space normal: Z is
        // reconstructed in the shader from X and Y, so storing it would cost a third of the budget
        // for a value the shader already knows.
        case TextureUsage::NormalMap:
            return TextureFormat::BC5;
        case TextureUsage::Data:
            return has_alpha ? TextureFormat::BC7 : TextureFormat::BC4;
        case TextureUsage::Hdr:
            return TextureFormat::BC6H;
        case TextureUsage::UserInterface:
            return TextureFormat::RGBA8;
    }
    return TextureFormat::RGBA8;
}

// --- ImageData -----------------------------------------------------------------------------------

Status ImageData::validate() const noexcept {
    if (width == 0 || height == 0 || channels == 0 || channels > 4) {
        return fail(ErrorCode::InvalidArgument, "an image with a zero or impossible dimension");
    }
    if (pixels.size() != static_cast<usize>(width) * height * channels) {
        return fail(ErrorCode::InvalidArgument, "an image whose pixels do not fill its dimensions");
    }
    return ok();
}

bool ImageData::has_meaningful_alpha() const noexcept {
    if (channels < 4) {
        return false;
    }
    const usize pixel_count = static_cast<usize>(width) * height;
    for (usize index = 0; index < pixel_count; ++index) {
        if (pixels[(index * channels) + 3] != 255) {
            return true;
        }
    }
    return false;
}

Expected<ImageData, Error> decode_image(Span<const u8> bytes, std::string_view extension) noexcept {
    if (extension == ".tga" || extension == ".TGA") {
        return decode_targa(bytes);
    }
    return fail(ErrorCode::Unsupported,
                "this build reads Targa only; PNG needs a DEFLATE decoder and JPEG a DCT one, "
                "neither of which is an integrated dependency yet");
}

Expected<u32, Error> generate_mips(const ImageData& image, bool srgb, bool preserve_alpha_coverage,
                                   Array<u8>& out) noexcept {
    if (Status valid = image.validate(); !valid) {
        return make_unexpected(valid.error());
    }
    if (Status appended = out.append(Span<const u8>(image.pixels.data(), image.pixels.size()));
        !appended) {
        return make_unexpected(appended.error());
    }

    const f32 base_coverage = alpha_coverage(
        image.pixels.data(), static_cast<usize>(image.width) * image.height, image.channels, 1.0f);

    ImageData level;
    level.width = image.width;
    level.height = image.height;
    level.channels = image.channels;
    if (Status appended =
            level.pixels.append(Span<const u8>(image.pixels.data(), image.pixels.size()));
        !appended) {
        return make_unexpected(appended.error());
    }

    u32 mip_count = 1;
    while (level.width > 1 || level.height > 1) {
        Expected<ImageData, Error> smaller = downsample(level, srgb);
        if (!smaller) {
            return make_unexpected(smaller.error());
        }
        level = std::move(smaller.value());

        if (preserve_alpha_coverage && level.channels == 4) {
            // Binary search for the alpha scale that restores the base level's coverage. Four
            // iterations of bisection is within a percent, and the alternative — leaving it alone —
            // is the well-known artefact of foliage thinning out and vanishing with distance.
            f32 low = 0.0f;
            f32 high = 4.0f;
            for (u32 step = 0; step < 10; ++step) {
                const f32 midpoint = (low + high) * 0.5f;
                const f32 coverage = alpha_coverage(level.pixels.data(),
                                                    static_cast<usize>(level.width) * level.height,
                                                    level.channels, midpoint);
                if (coverage < base_coverage) {
                    low = midpoint;
                } else {
                    high = midpoint;
                }
            }
            // `high` and not the last midpoint. The invariant the bisection maintains is that
            // `high` reaches the base level's coverage and `low` does not, so the last midpoint is
            // on whichever side the final step happened to land — and half the time that is the
            // side that restores nothing. Taking `high` is the smallest scale known to work, which
            // is what "preserve the coverage" means.
            scale_alpha(level, high);
        }

        if (Status appended = out.append(Span<const u8>(level.pixels.data(), level.pixels.size()));
            !appended) {
            return make_unexpected(appended.error());
        }
        ++mip_count;
    }
    return mip_count;
}

// --- The importer --------------------------------------------------------------------------------

OptionsSchema texture_options() noexcept {
    return OptionsSchema(Span<const OptionSpec>(kTextureOptions));
}

ImporterInfo TextureImporter::info() const noexcept {
    ImporterInfo info;
    info.name = "texture";
    // Moved whenever the output changes for the same input. It is part of every key this importer
    // produces, which is what makes a bump re-cook every texture in the project.
    info.version = 1;
    info.extensions = Span<const std::string_view>(kExtensions);
    info.produces = Span<const assets::AssetKind>(kProduces);
    info.description =
        "Imports an image into a cooked texture: format chosen from its declared usage and the "
        "target platform, a mip chain generated in the right colour space, and the usual "
        "mis-taggings reported.";
    return info;
}

OptionsSchema TextureImporter::schema() const noexcept {
    return texture_options();
}

Status TextureImporter::import(const ImportRequest& request, ImportResult& out) noexcept {
    const OptionsSchema options = schema();

    Expected<OptionValue, Error> usage_option = request.option(options, "usage");
    if (!usage_option) {
        return make_unexpected(usage_option.error());
    }
    Expected<TextureUsage, Error> usage = texture_usage_from_name(usage_option.value().as_text());
    if (!usage) {
        return make_unexpected(usage.error());
    }

    Expected<OptionValue, Error> srgb_option = request.option(options, "srgb");
    Expected<OptionValue, Error> max_option = request.option(options, "max-resolution");
    Expected<OptionValue, Error> mips_option = request.option(options, "generate-mips");
    Expected<OptionValue, Error> coverage_option =
        request.option(options, "preserve-alpha-coverage");
    Expected<OptionValue, Error> convention_option = request.option(options, "normal-convention");
    if (!srgb_option || !max_option || !mips_option || !coverage_option || !convention_option) {
        return fail(ErrorCode::Internal,
                    "the texture importer's own option schema is inconsistent");
    }

    Expected<ImageData, Error> decoded = decode_image(request.bytes, request.source.extension());
    if (!decoded) {
        // A source this build cannot read is a diagnostic on the asset, not a failure of the run: a
        // project with one unreadable texture must still cook.
        if (Status reported = out.report(ImportSeverity::Error, "unreadable-source",
                                         decoded.error().message, request.source.view());
            !reported) {
            return reported;
        }
        return ok();
    }
    ImageData image = std::move(decoded.value());

    const bool srgb_flag = srgb_option.value().as_bool();
    const bool has_alpha = image.has_meaningful_alpha();

    // --- The mistake detector. Each of these is a real defect that is invisible until it is
    // expensive, and each names the option that would fix it.
    if (usage.value() == TextureUsage::NormalMap && srgb_flag) {
        if (Status reported = out.report(
                ImportSeverity::Warning, "srgb-normal-map",
                "a normal map declared sRGB: its values are directions, not colours, and the "
                "curve bends them. Set srgb = false.",
                request.source.view());
            !reported) {
            return reported;
        }
    }
    if (usage.value() == TextureUsage::Data && srgb_flag) {
        if (Status reported = out.report(
                ImportSeverity::Warning, "srgb-data-texture",
                "a roughness, metalness or mask texture declared sRGB: the curve bends its "
                "midtones and an artist will compensate by authoring a wrong texture. Set "
                "srgb = false.",
                request.source.view());
            !reported) {
            return reported;
        }
    }
    if ((usage.value() == TextureUsage::Colour || usage.value() == TextureUsage::UserInterface) &&
        !srgb_flag) {
        if (Status reported =
                out.report(ImportSeverity::Warning, "linear-colour-texture",
                           "a colour texture declared linear: it will be washed out. Set "
                           "srgb = true unless it was authored in linear space deliberately.",
                           request.source.view());
            !reported) {
            return reported;
        }
    }
    if (image.channels == 4 && !has_alpha) {
        if (Status reported = out.report(
                ImportSeverity::Warning, "unnecessary-alpha",
                "every alpha value is opaque, so the fourth channel is a third more memory for "
                "nothing. Export without alpha, or set alpha = opaque.",
                request.source.view());
            !reported) {
            return reported;
        }
    }

    const PlatformFamily platform = platform_family_of(request.variant.view());
    const TextureFormat format = select_format(usage.value(), has_alpha, platform);
    if (is_block_compressed(format) && ((image.width % 4) != 0 || (image.height % 4) != 0)) {
        if (Status reported = out.report(
                ImportSeverity::Warning, "block-size-mismatch",
                "a block-compressed format needs dimensions that are multiples of four; the edge "
                "will be padded, which shows as a seam when the texture tiles.",
                request.source.view());
            !reported) {
            return reported;
        }
    }

    // A DirectX-orientation normal map has its green channel inverted. Converting the TEXTURE
    // rather than the mesh's tangents is deliberate — see the convention note in mesh.h — because a
    // per-mesh fix leaves the two disagreeing for any material shared between meshes.
    if (usage.value() == TextureUsage::NormalMap &&
        convention_option.value().as_text() == "directx" && image.channels >= 2) {
        const usize pixel_count = static_cast<usize>(image.width) * image.height;
        for (usize index = 0; index < pixel_count; ++index) {
            u8& green = image.pixels[(index * image.channels) + 1];
            green = static_cast<u8>(255U - green);
        }
    }

    // Halve until the longest edge fits the platform's budget. Interface art is exempt: it is drawn
    // at a fixed size and resizing it is a visible softening rather than a saving.
    const auto max_edge = static_cast<u32>(max_option.value().as_int());
    if (usage.value() != TextureUsage::UserInterface) {
        while (image.width > max_edge || image.height > max_edge) {
            Expected<ImageData, Error> smaller = downsample(image, srgb_flag);
            if (!smaller) {
                return make_unexpected(smaller.error());
            }
            image = std::move(smaller.value());
        }
    }

    Array<u8> payload;
    CookedTexture header;
    header.format = format;
    header.width = image.width;
    header.height = image.height;
    header.srgb = srgb_flag;
    // False, and recorded rather than assumed: no block encoder is linked at M5, so the payload
    // carries the uncompressed levels and `format` names what a build with an encoder would
    // produce. See the note at the head of texture.h.
    header.encoded = false;

    Array<u8> levels;
    if (mips_option.value().as_bool()) {
        Expected<u32, Error> mips =
            generate_mips(image, srgb_flag, coverage_option.value().as_bool(), levels);
        if (!mips) {
            return make_unexpected(mips.error());
        }
        header.mip_count = mips.value();
    } else {
        if (Status appended =
                levels.append(Span<const u8>(image.pixels.data(), image.pixels.size()));
            !appended) {
            return appended;
        }
        header.mip_count = 1;
    }

    // The payload's own header: sixteen bytes, then the levels. Little-endian, like every other
    // format this engine writes.
    u8 fixed[16] = {};
    const auto write_u32 = [](u8* destination, u32 value) noexcept {
        for (u32 index = 0; index < 4; ++index) {
            destination[index] = static_cast<u8>((value >> (index * 8U)) & 0xFFU);
        }
    };
    write_u32(fixed, CookedTexture::kVersion);
    fixed[4] = static_cast<u8>(static_cast<u16>(header.format) & 0xFFU);
    fixed[5] = static_cast<u8>((static_cast<u16>(header.format) >> 8U) & 0xFFU);
    fixed[6] = header.encoded ? 1U : 0U;
    fixed[7] = header.srgb ? 1U : 0U;
    write_u32(fixed + 8, header.width);
    write_u32(fixed + 12, header.height);
    if (Status appended = payload.append(Span<const u8>(fixed, sizeof(fixed))); !appended) {
        return appended;
    }
    u8 mip_bytes[4] = {};
    write_u32(mip_bytes, header.mip_count);
    if (Status appended = payload.append(Span<const u8>(mip_bytes, sizeof(mip_bytes))); !appended) {
        return appended;
    }
    if (Status appended = payload.append(Span<const u8>(levels.data(), levels.size())); !appended) {
        return appended;
    }

    return out.add(assets::AssetKind::Texture, "texture", std::move(payload), true);
}

}  // namespace cy::import
