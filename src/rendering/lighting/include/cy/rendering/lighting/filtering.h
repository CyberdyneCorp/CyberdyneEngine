#pragma once
// Shadow filtering and the bias model. Task 4.4.2.
//
// `rendering-lighting-and-shadows` — "Shadow filtering": hardware comparison sampling with
// reversed-Z comparison, PCF with a rotated Poisson disk, PCSS with a blocker search, sample counts
// as specialization constants, and a bias that combines "a constant depth bias, a slope-scaled
// bias, and a **normal offset** that displaces the lookup position along the surface normal
// proportionally to shadow texel size".
//
// ================================================================================================
// WHAT IS HERE AND WHAT IS IN cy/shadow.slang, AND WHY THE SPLIT IS WHERE IT IS
// ================================================================================================
//
// The TAPS are the shader's: `samplePercentageCloserFilter` is already in `cy/shadow.slang`, with
// `kShadowSampleCount` as a specialization constant exactly as the requirement asks. What is here
// is everything a shader cannot compute for itself or should not compute per pixel:
//
//   * the per-quality sample counts, so the specialization value has one source;
//   * the filter radius in UV, which depends on the atlas tile's size and is a constant per light;
//   * the bias terms, which a test can assert on without a GPU — and acne is a numerical failure,
//     so a numerical test is the right kind.
//
// ================================================================================================
// NORMAL OFFSET IS NOT AN ALTERNATIVE TO DEPTH BIAS, IT IS THE ONE THAT AVOIDS PETER-PANNING
// ================================================================================================
//
// A constant depth bias large enough to remove acne at grazing angles detaches contact shadows —
// "peter-panning", the scenario the specification names. A normal offset moves the LOOKUP rather
// than the DEPTH: the sample is taken from a point displaced along the surface normal by about a
// texel's world size, so the comparison lands on the same surface the fragment is on. It costs
// nothing at contact, where the offset is a texel and the geometry is a metre.
//
// The three terms are complementary and all three are needed. The constant handles depth-buffer
// quantisation, the slope-scaled term handles the surface tilting away from the light, and the
// normal offset handles the shadow map's own texel footprint.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// Which filter a light's shadow uses.
enum class ShadowFilter : u8 {
    /// One hardware-compared tap. The cheapest, and correct for a light whose shadow is tiny on
    /// screen.
    Hard = 0,
    /// A rotated Poisson disk of comparison taps.
    PercentageCloser,
    /// A blocker search, then a kernel sized from the estimated penumbra. Contact-hardening.
    PercentageCloserSoft,
    Count,
};

[[nodiscard]] const char* shadow_filter_name(ShadowFilter filter) noexcept;

/// The quality tier a project's settings select. Drives the specialization constant, and nothing
/// else — two tiers are two pipelines from one SPIR-V module, which is the whole point of the
/// constant being a specialization one.
enum class ShadowQuality : u8 {
    Low = 0,
    Medium,
    High,
    Ultra,
    Count,
};

/// The value `cy/shadow.slang`'s `kShadowSampleCount` is specialised with.
[[nodiscard]] u32 shadow_sample_count(ShadowQuality quality) noexcept;

/// The blocker-search tap count for PCSS at a quality. Separate from the filter count because the
/// search is a coarse estimate and does not need the filter's resolution.
[[nodiscard]] u32 blocker_search_sample_count(ShadowQuality quality) noexcept;

struct ShadowBiasSettings {
    /// In depth units, applied directly to the comparison reference. Small: it exists for
    /// quantisation, not for geometry.
    f32 constant = 0.0005F;
    /// Multiplies `tan(angle between the surface and the light)`, clamped so a surface edge-on to
    /// the light does not produce an unbounded bias.
    f32 slope_scaled = 0.002F;
    /// In units of the shadow texel's world size. About one texel is right; more detaches contact
    /// shadows, which is the thing the normal offset exists to avoid.
    f32 normal_offset_texels = 1.4F;
};

/// The depth bias for one lookup: the constant plus the slope-scaled term.
///
/// REVERSED-Z, so the bias is SUBTRACTED from the reference depth by the caller — nearer is
/// greater. This function returns a positive magnitude and says so here rather than returning a
/// signed value whose sign a reader has to reconstruct from the convention.
[[nodiscard]] f32 shadow_depth_bias(const ShadowBiasSettings& settings,
                                    f32 normal_dot_light) noexcept;

/// The world-space offset added to the lookup position, along the surface normal.
///
/// It grows as the surface turns away from the light, because the texel's footprint on the surface
/// grows with exactly that: a texel seen at a grazing angle covers more surface, so the sample can
/// land further from the fragment.
[[nodiscard]] Vec3 shadow_normal_offset(const ShadowBiasSettings& settings, Vec3 surface_normal,
                                        f32 normal_dot_light, f32 texel_world_size) noexcept;

/// The PCF kernel radius in atlas UV, from a world-space radius and the tile it samples.
///
/// A radius in UV is what the shader wants and a radius in metres is what an artist sets, and the
/// conversion needs the tile's size and the projection's extent — neither of which a shader has.
[[nodiscard]] f32 filter_radius_uv(f32 world_radius, f32 projection_extent, u32 atlas_size,
                                   u32 tile_size) noexcept;

/// The PCSS penumbra radius, in the same units as the depths.
///
/// The similar-triangles estimate: `(receiver − blocker) / blocker · light_size`. Returns zero when
/// the search found no blocker, which is the fully-lit case and must not produce a kernel.
///
/// REVERSED-Z AGAIN, and it matters: the depths are in the engine's reversed space, so a blocker is
/// NEARER than the receiver and therefore GREATER. Written as a function so that comparison appears
/// once rather than in every shading model.
[[nodiscard]] f32 pcss_penumbra_radius(f32 receiver_depth, f32 average_blocker_depth,
                                       f32 light_size_uv) noexcept;

}  // namespace cy::rendering
