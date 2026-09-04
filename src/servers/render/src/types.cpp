// The renderer's value vocabulary: the format table and the enumerator spellings.
// See cy/servers/render/types.h.

#include <cy/servers/render/types.h>

#include <cy/core/base/assert.h>

#include <cstring>

namespace cy::render {
namespace {

// One row per enumerator, in enumerator order, so the lookup is an index and the static_assert
// below fails the build if a format is added without a row. The same shape as rhi::format_info().
constexpr TextureFormatInfo kFormatTable[] = {
    {"Undefined", 0, 1, 1, false, false, false},

    {"R8Unorm", 1, 1, 1, false, false, false},        {"Rg8Unorm", 2, 1, 1, false, false, false},
    {"Rgba8Unorm", 4, 1, 1, false, false, true},      {"Rgba8Srgb", 4, 1, 1, true, false, true},
    {"R16Sfloat", 2, 1, 1, false, false, false},      {"Rg16Sfloat", 4, 1, 1, false, false, false},
    {"Rgba16Sfloat", 8, 1, 1, false, false, true},    {"R32Sfloat", 4, 1, 1, false, false, false},
    {"B10G11R11Ufloat", 4, 1, 1, false, false, true}, {"Rgb9E5Ufloat", 4, 1, 1, false, false, true},

    {"Bc1RgbaUnorm", 8, 4, 4, false, true, true},     {"Bc1RgbaSrgb", 8, 4, 4, true, true, true},
    {"Bc3Unorm", 16, 4, 4, false, true, true},        {"Bc3Srgb", 16, 4, 4, true, true, true},
    {"Bc4Unorm", 8, 4, 4, false, true, false},        {"Bc5Unorm", 16, 4, 4, false, true, false},
    {"Bc6HUfloat", 16, 4, 4, false, true, true},      {"Bc7Unorm", 16, 4, 4, false, true, true},
    {"Bc7Srgb", 16, 4, 4, true, true, true},

    {"Astc4x4Unorm", 16, 4, 4, false, true, true},    {"Astc4x4Srgb", 16, 4, 4, true, true, true},
    {"Astc6x6Unorm", 16, 6, 6, false, true, true},    {"Astc6x6Srgb", 16, 6, 6, true, true, true},
    {"Astc8x8Unorm", 16, 8, 8, false, true, true},    {"Astc8x8Srgb", 16, 8, 8, true, true, true},
};

static_assert(sizeof(kFormatTable) / sizeof(kFormatTable[0]) ==
                  static_cast<usize>(TextureFormat::Count),
              "add a row to kFormatTable for the new TextureFormat, in enumerator order");

constexpr TextureFormatInfo kUnknownFormat{"<invalid>", 0, 1, 1, false, false, false};

constexpr const char* kBlendModeNames[] = {
    "Opaque", "Masked", "Translucent", "Additive", "Modulate", "PremultipliedAlpha",
};

constexpr const char* kShadingModelNames[] = {
    "Lit",   "Unlit", "ClearCoat", "Anisotropic", "SubsurfaceScattering",
    "Cloth", "Hair",  "Foliage",   "Water",
};
static_assert(sizeof(kShadingModelNames) / sizeof(kShadingModelNames[0]) == kShadingModelCount);

constexpr const char* kSortLayerNames[] = {
    "Background", "Opaque", "Masked", "Transparent", "Overlay",
};
static_assert(sizeof(kSortLayerNames) / sizeof(kSortLayerNames[0]) == kSortLayerCount);

constexpr const char* kDebugViewModeNames[] = {
    "Off",
    "Albedo",
    "Normals",
    "Roughness",
    "Metallic",
    "AmbientOcclusion",
    "WorldPosition",
    "Depth",
    "Overdraw",
    "Wireframe",
    "ShadingComplexity",
    "LightComplexity",
    "ClusterOccupancy",
    "LodLevel",
    "MipLevel",
    "MotionVectors",
    "GiContribution",
    "ShadowCascades",
    "BoundingVolumes",
};
static_assert(sizeof(kDebugViewModeNames) / sizeof(kDebugViewModeNames[0]) == kDebugViewModeCount);

}  // namespace

const TextureFormatInfo& texture_format_info(TextureFormat format) noexcept {
    const auto index = static_cast<usize>(format);
    if (index >= static_cast<usize>(TextureFormat::Count)) {
        return kUnknownFormat;
    }
    return kFormatTable[index];
}

const char* texture_format_name(TextureFormat format) noexcept {
    return texture_format_info(format).name;
}

TextureFormat desktop_format_for(TextureUsageClass usage) noexcept {
    switch (usage) {
        case TextureUsageClass::Color:
            // BC7 rather than BC1: the specification's own example is a normal map going to BC5,
            // and the colour default matching it in quality is what keeps an artist from reaching
            // for uncompressed the first time BC1 banding shows.
            return TextureFormat::Bc7Srgb;
        case TextureUsageClass::Normal:
            // Two channels, linear, the third reconstructed in the shader. The specification names
            // this one exactly.
            return TextureFormat::Bc5Unorm;
        case TextureUsageClass::Data:
            return TextureFormat::Bc7Unorm;
        case TextureUsageClass::Hdr:
            return TextureFormat::Bc6HUfloat;
    }
    return TextureFormat::Undefined;
}

TextureFormat mobile_format_for(TextureUsageClass usage) noexcept {
    switch (usage) {
        case TextureUsageClass::Color:
            return TextureFormat::Astc6x6Srgb;
        case TextureUsageClass::Normal:
            // Linear and finer-grained than colour: a normal map's error is a shading error, and
            // ASTC has no two-channel mode to fall back on the way BC5 is one.
            return TextureFormat::Astc4x4Unorm;
        case TextureUsageClass::Data:
            return TextureFormat::Astc6x6Unorm;
        case TextureUsageClass::Hdr:
            // No ASTC HDR profile is assumed: a device that lacks it would have no format at all,
            // and a half-float image that fits is better than a compressed one that cannot be
            // sampled. The cooker reports the size.
            return TextureFormat::Rgba16Sfloat;
    }
    return TextureFormat::Undefined;
}

u64 texture_format_byte_size(TextureFormat format, u32 width, u32 height) noexcept {
    const TextureFormatInfo& info = texture_format_info(format);
    if (info.bytes_per_block == 0) {
        return 0;
    }
    const u64 blocks_x = (static_cast<u64>(width) + info.block_width - 1U) / info.block_width;
    const u64 blocks_y = (static_cast<u64>(height) + info.block_height - 1U) / info.block_height;
    return blocks_x * blocks_y * info.bytes_per_block;
}

u32 full_mip_count(u32 width, u32 height) noexcept {
    u32 levels = 1;
    u32 extent = (width > height) ? width : height;
    while (extent > 1U) {
        extent >>= 1U;
        ++levels;
    }
    return levels;
}

u64 texture_mip_chain_byte_size(TextureFormat format, u32 width, u32 height,
                                u32 mip_levels) noexcept {
    if (width == 0 || height == 0 || mip_levels == 0) {
        return 0;
    }
    const u32 available = full_mip_count(width, height);
    const u32 levels = (mip_levels < available) ? mip_levels : available;
    u64 total = 0;
    for (u32 level = 0; level < levels; ++level) {
        const u32 mip_width = (width >> level) != 0U ? (width >> level) : 1U;
        const u32 mip_height = (height >> level) != 0U ? (height >> level) : 1U;
        total += texture_format_byte_size(format, mip_width, mip_height);
    }
    return total;
}

const char* output_color_space_name(OutputColorSpace space) noexcept {
    switch (space) {
        case OutputColorSpace::Srgb:
            return "Srgb";
        case OutputColorSpace::Rec2020Pq:
            return "Rec2020Pq";
        case OutputColorSpace::ScRgbLinear:
            return "ScRgbLinear";
    }
    return "<invalid>";
}

const char* blend_mode_name(BlendMode mode) noexcept {
    const auto index = static_cast<usize>(mode);
    constexpr usize kCount = sizeof(kBlendModeNames) / sizeof(kBlendModeNames[0]);
    return (index < kCount) ? kBlendModeNames[index] : "<invalid>";
}

const char* shading_model_name(ShadingModel model) noexcept {
    const auto index = static_cast<usize>(model);
    return (index < kShadingModelCount) ? kShadingModelNames[index] : "<invalid>";
}

const char* sort_layer_name(SortLayer layer) noexcept {
    const auto index = static_cast<usize>(layer);
    return (index < kSortLayerCount) ? kSortLayerNames[index] : "<invalid>";
}

const char* debug_view_mode_name(DebugViewMode mode) noexcept {
    const auto index = static_cast<usize>(mode);
    return (index < kDebugViewModeCount) ? kDebugViewModeNames[index] : "<invalid>";
}

DebugViewMode debug_view_mode_from_name(const char* name) noexcept {
    if (name == nullptr) {
        return DebugViewMode::Off;
    }
    for (u32 index = 0; index < kDebugViewModeCount; ++index) {
        if (std::strcmp(name, kDebugViewModeNames[index]) == 0) {
            return static_cast<DebugViewMode>(index);
        }
    }
    return DebugViewMode::Off;
}

}  // namespace cy::render
