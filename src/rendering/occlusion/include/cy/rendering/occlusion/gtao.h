// SPDX-License-Identifier: MIT
#pragma once
// Ground-truth ambient occlusion: the settings, the constants a dispatch reads, and the CPU
// reference the device suite compares against.
//
// `rendering-post-processing` — "Ambient occlusion": "The engine SHALL implement ground-truth
// ambient occlusion (GTAO) — horizon-based visibility integration with a cosine-weighted term —
// computed from depth and normals at half or full resolution."
//
// ================================================================================================
// WHY GTAO AND NOT SSAO
// ================================================================================================
//
// The specification names it, and the reason it does is the reason it is right: SSAO counts depth
// samples that fall inside a hemisphere, so its answer is a sample ratio with no physical meaning —
// it has to be tuned per scene, and it darkens a flat open floor unless a bias is tuned against the
// depth precision. GTAO finds the two horizons in each slice and integrates the visible arc in
// closed form, weighted by the cosine against the normal, which is the ambient visibility the
// shading actually needs: an open plane integrates to exactly 1, a 90-degree inner corner to a
// fixed fraction, whatever the scene. It also costs fewer taps for the same noise, because every
// tap moves a horizon rather than voting.
//
// ================================================================================================
// WHY THE NORMALS COME FROM THE PREPASS AND NOT FROM DEPTH
// ================================================================================================
//
// `rendering-forward-clustered` already derives `PrepassMode::DepthNormal` from ambient occlusion
// being on — the target exists and the prepass fills it whether or not anything reads it, so
// reading it costs one load a pixel. A normal reconstructed from depth differences is wrong at
// every silhouette (the cross product straddles two surfaces) and at every contact, which is
// exactly where occlusion is decided; it is right only on the interior of a surface, where the
// answer is least interesting. The prepass normal is the geometric one, which is also the normal
// the horizon should be measured against: a normal map's detail is below the scale of the search.
//
// ================================================================================================
// WHAT IS NOT READ
// ================================================================================================
//
// `AmbientOcclusionSettings::resolution_scale`. The term is computed at FULL resolution, which the
// requirement allows ("half or full"); a half-resolution term needs a depth-aware upsample the
// filter cascade does not have. Stated rather than silently honoured at 1.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/post/effects.h>

namespace cy::rendering::occlusion {

/// Everything the horizon search reads that is a decision rather than a property of the view.
struct GtaoSettings {
    /// Radius in metres, the final power and the direct-light option. `resolution_scale` is not
    /// read; see the header comment.
    AmbientOcclusionSettings shared{};
    /// Directions per pixel: an even share of gtao_common.slang's eight whole-pixel lattice
    /// directions — 1, 2, 4 or 8 — rotated per pixel by the dither, so a 4x4 neighbourhood sees all
    /// eight.
    u32 slices = 4;
    /// Depth taps per side of each slice, distributed quadratically toward the pixel.
    u32 steps = 6;
    /// The fraction of the radius over which a sample's weight falls to zero. The outer 60 % by
    /// default, which is what keeps a distant occluder from producing a hard ring.
    f32 falloff_fraction = 0.6F;
    /// Subtracted from every sample's horizon cosine. See gtao.slang: it is what makes the
    /// surface's own samples, a few ulps either side of the tangent in a 32-bit depth buffer, never
    /// read as an occluder of the surface they lie on.
    f32 horizon_bias = 0.03F;
    /// The largest screen-space radius, in pixels. A surface next to the near plane would
    /// otherwise march the whole image.
    f32 max_radius_pixels = 128.0F;
};

/// What one frame's view contributes.
struct GtaoView {
    /// The UNJITTERED projection the depth was written with, reversed-Z.
    Mat4 projection = Mat4::identity();
    /// Camera-relative space to view space. Only its rotation is read: the prepass normals are
    /// camera-relative, and the reconstruction is in view space.
    Mat4 relative_to_view = Mat4::identity();
    u32 width = 0;
    u32 height = 0;
    /// The sub-pixel jitter the depth was rasterised with, in pixels, in the temporal framework's
    /// convention (`AssemblyReport::jitter`: +y up). The search reconstructs with the unjittered
    /// projection and undoes this, or a jittered plane reconstructs as a slightly curved one.
    Vec2 jitter{0.0F, 0.0F};
};

/// `CyGtaoConstants` in gtao.slang, word for word.
struct alignas(16) GtaoConstants {
    f32 view_rows[3][4] = {};
    f32 projection[4] = {};
    f32 extent[4] = {};
    f32 radius[4] = {};
    f32 control[4] = {};
    f32 jitter[4] = {};
};
static_assert(sizeof(GtaoConstants) == 128, "CyGtaoConstants is eight float4");

/// `CyGtaoFilterConstants` in gtao_filter.slang, word for word.
struct alignas(16) GtaoFilterConstants {
    f32 projection[4] = {};
    f32 sigma[4] = {};
    u32 control[4] = {};
};
static_assert(sizeof(GtaoFilterConstants) == 48, "CyGtaoFilterConstants is three 16-byte rows");

/// The horizon search's constants for a view. Refuses an empty extent and a non-positive radius.
[[nodiscard]] Expected<GtaoConstants, Error> make_gtao_constants(const GtaoSettings& settings,
                                                                 const GtaoView& view) noexcept;

/// One pass of the filter cascade, from the shared denoiser's ambient occlusion configuration and
/// the first position of its quality ladder. `step` is the a-trous step in pixels.
[[nodiscard]] GtaoFilterConstants make_filter_constants(const GtaoView& view, u32 step) noexcept;

/// How many filter passes the cascade runs: the denoiser's `max_passes` for ambient occlusion,
/// capped by the ladder's first position.
[[nodiscard]] u32 filter_pass_count() noexcept;

/// The four words of `pipeline::FrameViewData::occlusion_control` for a term bound at `slot` of the
/// frame's texture table: the slot, then `apply_to_direct` and `direct_strength` as the frame reads
/// them. The direct term is untouched unless the settings ask for the non-physical option.
void write_occlusion_control(u32 slot, const AmbientOcclusionSettings& settings,
                             u32 out[4]) noexcept;

/// `cy/packing.slang`'s octahedral decode, transcribed.
[[nodiscard]] Vec3 decode_octahedral(Vec2 encoded) noexcept;
/// And its encode, for a test that has to write what the prepass would.
[[nodiscard]] Vec2 encode_octahedral(Vec3 normal) noexcept;

/// The inputs the horizon search reads, as the device reads them. All spans are `width * height`,
/// row-major from the top-left.
struct GtaoInputs {
    u32 width = 0;
    u32 height = 0;
    /// Reversed-Z depth in [0, 1]; 0 is the cleared far plane.
    Span<const f32> depth;
    /// The prepass target's `.xy`: the octahedral pair, camera-relative.
    Span<const Vec2> normals;
};

/// gtao.slang's `cyGtao`, on the host, expression for expression: (bent normal, visibility) per
/// pixel, in `out`. The device suite compares the two buffers.
[[nodiscard]] Status gtao_reference(const GtaoInputs& inputs, const GtaoConstants& constants,
                                    Span<Vec4> out) noexcept;

/// One pixel of `gtao_reference`, for a caller that samples the image rather than walking it. The
/// inputs must cover the view, as `gtao_reference` requires.
[[nodiscard]] Vec4 gtao_reference_at(const GtaoInputs& inputs, const GtaoConstants& constants,
                                     u32 x, u32 y) noexcept;

/// The positive view depth of a reversed-Z depth, from the constants' projection terms. What the
/// shaders compute, and what a caller hands the denoiser as guidance.
[[nodiscard]] f32 view_depth(const f32 projection[4], f32 depth) noexcept;

}  // namespace cy::rendering::occlusion
