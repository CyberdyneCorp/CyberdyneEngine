// SPDX-License-Identifier: MIT
#pragma once
// The per-pixel resolve: the final gather a frame shades with, and the history its denoiser
// accumulates through. Issue #35, stage 4.
//
// `rendering-global-illumination` — "GI strategy layers", "Sample confidence", "Far-field
// illumination", "Ray-traced effects"; `denoising` — "Denoising pipeline".
//
// ================================================================================================
// WHAT A PIXEL GATHERS, AND WHY IT IS THREE SOURCES AND NOT ONE
// ================================================================================================
//
// `IlluminationSystem::indirect_diffuse` answers one query from the radiance cache and the sky. A
// frame asks two million of them, and the radiance cache is a lattice of probes a metre or more
// apart: every pixel between two probes gets their interpolation, which is right far from a surface
// and wrong at a contact, a crease or the foot of a thin wall — the places a probe cannot stand.
// So each pixel takes, by confidence, as `resolve.h`'s `combine()` takes every source:
//
//   the cache    `RadianceCache::sample_traced` at the pixel — smooth, complete, the lattice's
//                answer, with every probe that would contribute traced to through the field, so a
//                probe on the far side of a thin wall contributes nothing;
//   one ray      a cosine-distributed ray from the pixel's own surface through the world tier
//                (`SoftwareTracer` over the field, or `HardwareTracer` where the device has ray
//                queries), resolved through the surface cache like every traced hit — the
//                pixel's own view of what is next to it, which a probe on the other side of a
//                10 cm wall does not have;
//   the far sky  the sky through one of `DistanceField::sky_visibility`'s rays, weighted by how
//                far into the far field the pixel is — zero in the near field, where the cache's
//                probes and the ray already saw the sky through every opening there is.
//
// One ray a pixel a frame is a NOISY estimate, and that is deliberate: it is what
// `denoise::Denoiser` reconstructs, temporally and spatially, and what makes the result converge to
// the pixel's own irradiance rather than to the lattice's. `ray_density` is the GI budget's
// `TracingResolution` lever: the fraction of pixels that trace this frame; the rest take the cache
// and the sky alone, and the denoiser's history carries the traced ones' answer.
//
// Rough specular is the same three: the cache in the reflection direction, trusted as much as the
// surface is rough (`indirect_specular`'s rule), and — for a surface smooth enough that
// `reflection_ray_count` gives it rays — one ray of its lobe a frame.
//
// ================================================================================================
// THIS FILE IS THE ORACLE OF A DEVICE PASS, AND IS WRITTEN TO BE TRANSCRIBED
// ================================================================================================
//
// `cy::rendering-gi-gpu`'s resolve and denoiser passes are this file and `denoise::Denoiser`,
// expression for expression, and `render.gi_gpu_resolve` holds them to it pixel for pixel. Every
// random number is a hash of the pixel and the frame (`screen_hash`), so the host and the device
// draw the same rays, and a stochastic image is a regression test rather than a coin toss.
//
// The history is `temporal-rendering`'s: `screen_history` derives each pixel's motion with
// `derive_surface_motion` and classifies it with `classify_history`, and hands `Denoiser` the
// `HistoryGuidance` it consumes — the denoiser still computes no history of its own.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/reflections.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/screen_inputs.h>
#include <cy/rendering/gi/screen_probes.h>
#include <cy/rendering/gi/tracing.h>

namespace cy::rendering::gi {

/// A hash of a pixel, a frame and a stream: the whole of the resolve's randomness. The same
/// construction `signals.h` draws with, public so a device draws the same numbers.
[[nodiscard]] u32 screen_hash(u32 pixel, u64 frame, u32 stream) noexcept;
/// A unit float in [0, 1) from a hash: its low 24 bits.
[[nodiscard]] f32 screen_unit(u32 hash) noexcept;
/// A cosine-weighted direction about `normal` from two unit floats.
[[nodiscard]] Vec3 screen_cosine_direction(Vec3 normal, f32 u1, f32 u2) noexcept;

/// The hash streams, so the device and the host cannot disagree about which draw is which.
inline constexpr u32 kScreenStreamTrace = 5;
inline constexpr u32 kScreenStreamDirection = 6;
inline constexpr u32 kScreenStreamSpecular = 7;

/// Whether a pixel traces its final-gather ray this frame, at `density`.
[[nodiscard]] bool screen_traces(u32 pixel, u64 frame, f32 density) noexcept;

/// The orthonormal basis a cosine lobe is sampled in about `normal` (Duff et al.'s branchless
/// form).
void screen_basis(Vec3 normal, Vec3& tangent, Vec3& bitangent) noexcept;

/// The world tier `TieredTracer` would select, traced alone: what a final-gather ray is — from
/// `position` lifted along `normal` by `lift` (by at least one finest voxel on the software tier),
/// `reach` metres. A hit on a surface facing away from the ray answers nothing (source `None`).
[[nodiscard]] RadianceSample screen_world_ray(const ScreenResolveSources& sources, Vec3 position,
                                              Vec3 normal, Vec3 direction, f32 lift,
                                              f32 reach) noexcept;

/// A final-gather ray as one sample of its hemisphere: it counts in full whatever it met, and its
/// confidence says only how much of its VALUE is its own — `stand_in` supplies the rest where the
/// mask admits it. A ray that answered nothing takes the stand-in's value, or stays no sample.
[[nodiscard]] RadianceSample screen_gather_sample(const RadianceSample& ray,
                                                  const RadianceSample& stand_in,
                                                  u32 excluded_sources) noexcept;

/// The mirror direction of `view_direction` (eye to surface) about `normal`.
[[nodiscard]] Vec3 screen_reflect_view(Vec3 view_direction, Vec3 normal) noexcept;

/// Whether `point` lies in one of the first `kScreenHistoryRegions` invalidated boxes.
[[nodiscard]] bool screen_invalidated_at(Span<const Aabb> regions, Vec3 point) noexcept;

/// The landing cell of `position` in last frame's view of `width` x `height` cells: the camera's
/// motion alone, through `temporal-rendering`'s `derive_surface_motion`, as `screen_history` lands
/// a pixel. False when it lands off the screen or is not representable.
[[nodiscard]] bool screen_landing(Vec3 position, const ScreenHistoryInputs& inputs, u32 width,
                                  u32 height, u32& x, u32& y) noexcept;

/// The denoiser configuration a gather's signal is filtered with: `authored` itself for the
/// per-pixel gather, and for the screen probes with its a-trous cascade capped at
/// `probes.filter_passes`. The frame owner configures its denoiser with this; the device resolve
/// applies it to what it reads off the host denoiser.
[[nodiscard]] denoise::SignalConfig screen_signal_config(
    const denoise::SignalConfig& authored, const ScreenResolveSettings& settings) noexcept;

/// One pixel of the resolve. `view_direction` is from the eye to the surface.
[[nodiscard]] ScreenPixel resolve_screen_pixel(u32 pixel, Vec3 position, Vec3 normal, f32 roughness,
                                               f32 camera_distance, Vec3 view_direction,
                                               const ScreenResolveSettings& settings,
                                               const ScreenResolveSources& sources) noexcept;

/// The resolve over a view: the noisy diffuse and specular signals, ready for `Denoiser`.
class ScreenResolve {
public:
    ScreenResolve() noexcept = default;

    [[nodiscard]] Status resize(u32 width, u32 height) noexcept;

    /// Resolve every pixel of `surfaces`. A sky pixel gathers nothing and carries zero samples.
    [[nodiscard]] Status produce(const ScreenSurfaces& surfaces,
                                 const ScreenResolveSettings& settings,
                                 const ScreenResolveSources& sources) noexcept;

    [[nodiscard]] Span<const Vec3> diffuse() const noexcept { return diffuse_.span(); }
    [[nodiscard]] Span<const Vec3> specular() const noexcept { return specular_.span(); }
    /// Samples a pixel carries: one where there is a surface, zero on the sky.
    [[nodiscard]] Span<const f32> samples() const noexcept { return samples_.span(); }
    /// Pixels that traced a diffuse ray this frame, for the cost a lever buys.
    [[nodiscard]] u32 traced() const noexcept { return traced_; }

    /// The noisy signal as `Denoiser::denoise` takes it.
    [[nodiscard]] denoise::NoisySignal noisy(denoise::SignalKind kind) const noexcept;

    /// The screen probes the last `produce` placed and traced, with `GiGather::ScreenProbes`.
    [[nodiscard]] const ScreenProbeGather& probes() const noexcept { return probes_; }
    /// Forget the screen probes' history: a cut, a teleport.
    void reset_probe_history() noexcept { probes_.reset_history(); }

private:
    [[nodiscard]] Status produce_probes(const ScreenSurfaces& surfaces,
                                        const ScreenResolveSettings& settings,
                                        const ScreenResolveSources& sources) noexcept;

    u32 width_ = 0;
    u32 height_ = 0;
    u32 traced_ = 0;
    ScreenProbeGather probes_;
    Array<Vec3> diffuse_;
    Array<Vec3> specular_;
    Array<f32> samples_;
};

/// A view's surfaces on the resolve's GRID: every `divisor`-th pixel of the frame in each
/// direction, the block's top-left one. What the `TracingResolution` lever buys at its coarse end —
/// a quarter of the pixels resolved and denoised at a divisor of two, and `screen_upsample` back to
/// the frame's.
class ScreenGrid {
public:
    ScreenGrid() noexcept = default;

    /// The grid of `frame` at `divisor` (one is the frame itself). Idempotent in its buffers.
    [[nodiscard]] Status build(const ScreenSurfaces& frame, u32 divisor) noexcept;
    [[nodiscard]] ScreenSurfaces surfaces() const noexcept;
    [[nodiscard]] u32 divisor() const noexcept { return divisor_; }
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }

private:
    u32 divisor_ = 1;
    u32 width_ = 0;
    u32 height_ = 0;
    Vec3 camera_{0.0F, 0.0F, 0.0F};
    Array<f32> depth_;
    Array<Vec3> position_;
    Array<Vec3> normal_;
    Array<f32> roughness_;
};

/// The grid's size for a frame of `extent` pixels at `divisor`: ceil(extent / divisor).
[[nodiscard]] constexpr u32 screen_grid_extent(u32 extent, u32 divisor) noexcept {
    return divisor == 0 ? extent : (extent + divisor - 1U) / divisor;
}

/// The joint-bilateral upsample's two tolerances: a grid sample's view depth within this fraction
/// of the pixel's own, and its normal's cosine to the pixel's raised to this power (by squaring).
inline constexpr f32 kUpsampleDepthTolerance = 0.1F;
inline constexpr u32 kUpsampleNormalSquarings = 3;

/// The resolve's answer on its grid back on the frame's pixels: for each pixel, the four grid
/// samples about it weighted bilinearly, by depth and by normal, so an edge between two surfaces
/// is not smeared across; a pixel no sample agrees with takes the one whose depth is nearest its
/// own. At a divisor of one, every pixel is its own sample, exactly. A sky pixel is zero.
[[nodiscard]] Status screen_upsample(const ScreenSurfaces& frame, const ScreenSurfaces& grid,
                                     u32 divisor, Span<const Vec3> grid_values,
                                     Span<Vec3> out) noexcept;

/// The view's surfaces as `denoise::GuidanceBuffers`.
[[nodiscard]] denoise::GuidanceBuffers screen_guidance(const ScreenSurfaces& surfaces) noexcept;

/// The reprojection's answer for each pixel, through `temporal-rendering`: the pixel of last frame
/// it lands on, or -1, and a confidence of 1 or 0. The surface is taken to be where it was — the
/// camera's motion alone — and `classify_history`'s depth test rejects whatever moved. A pixel
/// inside an invalidated region keeps no history.
[[nodiscard]] Status screen_history(const ScreenSurfaces& surfaces,
                                    const ScreenHistoryInputs& inputs, Span<i32> source,
                                    Span<f32> confidence) noexcept;

}  // namespace cy::rendering::gi
