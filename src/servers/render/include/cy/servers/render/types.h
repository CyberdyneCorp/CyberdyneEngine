#pragma once
// The renderer's value vocabulary, at the layer every consumer can see. Tasks 4.1.1, 4.1.3, 4.2.2.
//
// --- WHY THERE IS A SECOND FORMAT ENUM IN THE ENGINE
// ----------------------------------------------
//
// `rhi::Format` exists and this file declares `TextureFormat`, and the duplication is deliberate
// rather than an oversight. They are two vocabularies about two different things:
//
//   rhi::Format      what a DEVICE can be asked for. Its set is "what M3 needs" and it grows when a
//                    backend can do something new. It lives at layer 3.
//   TextureFormat    what a COOKED ASSET is stored in. Its set is what `rendering-geometry-and-
//                    resources` names — BC1..BC7 on desktop, ASTC on mobile, "selected per platform
//                    at cook time" — and it contains formats no desktop device will ever be handed
//                    (every ASTC block size) while containing none of the transient render-target
//                    formats the graph invents for itself.
//
// The render server is layer 2 and may not name an RHI type at all, so the choice was never between
// one enum and two: it was between two enums and a texture record that carries an opaque integer
// nobody can read. Exactly one function translates — `cy::rendering::to_rhi_format()` in
// src/rendering/scene/ — and it contains no decisions, which is the same shape the RHI used for
// `Access` and for the same reason.
//
// --- WHY THE MATERIAL VOCABULARY IS HERE AND NOT IN src/rendering/material/
// -----------------------
//
// `BlendMode`, `ShadingModel` and `SortLayer` are read by the deterministic sort key (sort.h),
// which is the render server's, and written by the material model, which is layer 4. Putting them
// in the lower of the two modules means one definition; putting them in the higher one would mean
// the server holding a second copy for sorting, and two enumerations of blend modes that must agree
// is the failure this file exists to avoid one instance of.

#include <cy/core/base/types.h>

namespace cy::render {

// --- Layers ---------------------------------------------------------------------------------

/// Which rendering layers an instance belongs to and a view draws. A mask rather than an index
/// because an instance is commonly in several — "the world" and "casts shadows" and "reflects".
using LayerMask = u32;

inline constexpr LayerMask kNoLayers = 0;
inline constexpr LayerMask kAllLayers = ~0U;
inline constexpr LayerMask kDefaultLayer = 1U << 0U;

// --- Cooked texture formats -----------------------------------------------------------------

/// `rendering-geometry-and-resources`, "Texture formats and compression". See the header comment
/// for why this is not `rhi::Format`.
enum class TextureFormat : u16 {
    Undefined = 0,

    // Uncompressed, the set the specification names.
    R8Unorm,
    Rg8Unorm,
    Rgba8Unorm,
    Rgba8Srgb,
    R16Sfloat,
    Rg16Sfloat,
    Rgba16Sfloat,
    R32Sfloat,
    B10G11R11Ufloat,
    Rgb9E5Ufloat,

    // Desktop block compression.
    Bc1RgbaUnorm,
    Bc1RgbaSrgb,
    Bc3Unorm,
    Bc3Srgb,
    Bc4Unorm,
    Bc5Unorm,
    Bc6HUfloat,
    Bc7Unorm,
    Bc7Srgb,

    // Mobile block compression. Present here and absent from `rhi::Format` on purpose: a cooked
    // asset for a mobile target is stored in one of these whether or not the machine doing the
    // cooking has a device that can sample it.
    Astc4x4Unorm,
    Astc4x4Srgb,
    Astc6x6Unorm,
    Astc6x6Srgb,
    Astc8x8Unorm,
    Astc8x8Srgb,

    Count,
};

/// What a texture is FOR, which is what drives the cooked format and the colour space.
///
/// `rendering-geometry-and-resources`: "Textures SHALL declare their usage (colour, normal, data,
/// HDR) so the cooker selects an appropriate format and colour space automatically." Declaring the
/// usage rather than the format is what makes one asset cook correctly for two platforms.
enum class TextureUsageClass : u8 {
    Color,   // sRGB-encoded albedo, emissive, UI
    Normal,  // a tangent-space normal map: two channels, linear, +Y up
    Data,    // roughness, metallic, masks, packed ORM: linear
    Hdr,     // a captured or authored high dynamic range image
};

/// What the engine knows about a cooked format without asking a device.
struct TextureFormatInfo {
    const char* name = "";
    u32 bytes_per_block = 0;
    u8 block_width = 1;
    u8 block_height = 1;
    bool is_srgb = false;
    bool is_compressed = false;
    /// True for the formats whose channels are a colour rather than a measurement. Used by
    /// validation: a data slot fed an sRGB texture is decoded by hardware and is then wrong by a
    /// gamma, which is the defect `rendering-materials-and-shading` asks to be caught at import.
    bool is_color = false;
};

[[nodiscard]] const TextureFormatInfo& texture_format_info(TextureFormat format) noexcept;
[[nodiscard]] const char* texture_format_name(TextureFormat format) noexcept;

/// The format the cooker picks for a usage on a platform that has block compression, and the one it
/// picks where only ASTC is available. Both are pure functions of the declared usage, which is what
/// "the cooker selects automatically" means when it is code rather than a sentence.
[[nodiscard]] TextureFormat desktop_format_for(TextureUsageClass usage) noexcept;
[[nodiscard]] TextureFormat mobile_format_for(TextureUsageClass usage) noexcept;

/// Tightly packed bytes for one mip of `width` x `height`, block size accounted for. Zero for
/// `Undefined`, which is an answer a caller can test rather than a number it would then multiply.
[[nodiscard]] u64 texture_format_byte_size(TextureFormat format, u32 width, u32 height) noexcept;

/// Bytes for a full mip chain down to 1x1. What a residency budget is charged.
[[nodiscard]] u64 texture_mip_chain_byte_size(TextureFormat format, u32 width, u32 height,
                                              u32 mip_levels) noexcept;

/// Mip levels in a complete chain for an extent.
[[nodiscard]] u32 full_mip_count(u32 width, u32 height) noexcept;

// --- Output ---------------------------------------------------------------------------------

/// How the composed frame is encoded for the display. `rendering-architecture`, "Render targets and
/// formats": SDR (sRGB) and HDR (Rec.2020 PQ / scRGB) "where the display and platform support it".
enum class OutputColorSpace : u8 {
    Srgb = 0,
    Rec2020Pq,
    ScRgbLinear,
};

[[nodiscard]] const char* output_color_space_name(OutputColorSpace space) noexcept;

// --- Materials, as far as sorting is concerned ----------------------------------------------

/// `rendering-materials-and-shading`, "Material model". See the header comment for why the
/// enumeration lives at this layer.
enum class BlendMode : u8 {
    Opaque = 0,
    Masked,
    Translucent,
    Additive,
    Modulate,
    PremultipliedAlpha,
};

[[nodiscard]] const char* blend_mode_name(BlendMode mode) noexcept;

/// Whether a blend mode composites against what is already in the target. The three that do must be
/// drawn back to front and cannot use `Equal` depth testing; the two that do not are sorted front
/// to back for early-Z. That one question is the whole reason the sort key branches.
[[nodiscard]] constexpr bool blend_mode_is_transparent(BlendMode mode) noexcept {
    return mode != BlendMode::Opaque && mode != BlendMode::Masked;
}

/// `rendering-materials-and-shading`, "Shading models". The lowered form of a material's closure
/// set; the material compiler that produces it is M7's, and M3 selects one directly.
enum class ShadingModel : u8 {
    Lit = 0,
    Unlit,
    ClearCoat,
    Anisotropic,
    SubsurfaceScattering,
    Cloth,
    Hair,
    Foliage,
    Water,
    Count,
};

inline constexpr u32 kShadingModelCount = static_cast<u32>(ShadingModel::Count);

[[nodiscard]] const char* shading_model_name(ShadingModel model) noexcept;

/// The coarse buckets draws are grouped into before anything finer is considered. The order is the
/// order they are submitted in, and it is the most significant field of every sort key.
enum class SortLayer : u8 {
    Background = 0,  // sky, and anything that must be behind the world
    Opaque,
    Masked,  // alpha-tested: opaque depth behaviour, but a discard, so it sorts after plain opaque
    Transparent,
    Overlay,  // debug draw, in-world UI
    Count,
};

inline constexpr u32 kSortLayerCount = static_cast<u32>(SortLayer::Count);

[[nodiscard]] const char* sort_layer_name(SortLayer layer) noexcept;

/// The layer a blend mode draws in. A material never chooses its own bucket: the mode decides, so
/// two materials with the same blending cannot end up in different buckets by accident.
[[nodiscard]] constexpr SortLayer sort_layer_for(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Opaque:
            return SortLayer::Opaque;
        case BlendMode::Masked:
            return SortLayer::Masked;
        case BlendMode::Translucent:
        case BlendMode::Additive:
        case BlendMode::Modulate:
        case BlendMode::PremultipliedAlpha:
            return SortLayer::Transparent;
    }
    return SortLayer::Opaque;
}

// --- Debug view modes -------------------------------------------------------------------------

/// `rendering-architecture`, "Debug visualisation": the modes selectable per view.
///
/// One enumeration rather than a set of booleans, because they are mutually exclusive — a view
/// shows one of them — and because a per-view integer is what a shader specialization constant
/// takes. `Off` is 0 so a zeroed view is a normally shaded one.
enum class DebugViewMode : u8 {
    Off = 0,
    Albedo,
    Normals,
    Roughness,
    Metallic,
    AmbientOcclusion,
    WorldPosition,
    Depth,
    Overdraw,
    Wireframe,
    ShadingComplexity,
    LightComplexity,
    ClusterOccupancy,
    LodLevel,
    MipLevel,
    MotionVectors,
    GiContribution,
    ShadowCascades,
    BoundingVolumes,
    Count,
};

inline constexpr u32 kDebugViewModeCount = static_cast<u32>(DebugViewMode::Count);

[[nodiscard]] const char* debug_view_mode_name(DebugViewMode mode) noexcept;

/// Parse a mode from the spelling `debug_view_mode_name` produces. `Off` for anything else, which
/// is the safe answer for a console command with a typo in it.
[[nodiscard]] DebugViewMode debug_view_mode_from_name(const char* name) noexcept;

/// Whether debug visualisation is compiled at all.
///
/// `rendering-architecture`: "WHEN the engine is built for shipping THEN debug view modes and debug
/// draw SHALL be compiled out." The engine's gate for that class of thing is `CY_DEVELOPMENT`
/// (cmake/profiles.cmake), defined in Debug and Development and absent from Profile and Shipping —
/// which is STRICTER than the requirement, because it also removes debug views from a Profile
/// build. That is deliberate and it is the same line assertions and diagnostics are drawn on: a
/// Profile build exists to measure the shipping frame, and a frame carrying debug-view branches is
/// not the frame that ships.
#if defined(CY_DEVELOPMENT)
inline constexpr bool kDebugVisualisationEnabled = true;
#else
inline constexpr bool kDebugVisualisationEnabled = false;
#endif

}  // namespace cy::render
