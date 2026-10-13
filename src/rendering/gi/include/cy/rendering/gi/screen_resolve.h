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
#include <cy/rendering/gi/tracing.h>

namespace cy::rendering::gi {

/// One view's surfaces as the resolve reads them. Every span is `width * height`, row-major from
/// the top-left — `denoise::GuidanceBuffers`' convention, because these become it.
struct ScreenSurfaces {
    u32 width = 0;
    u32 height = 0;
    /// Linear view-space depth in metres, positive in front of the eye. Zero or negative is sky,
    /// and a sky pixel gathers nothing.
    Span<const f32> depth;
    /// World position and unit normal of the surface at each pixel.
    Span<const Vec3> position;
    Span<const Vec3> normal;
    /// Perceptual roughness. Empty is 0.5 everywhere.
    Span<const f32> roughness;
    Vec3 camera{0.0F, 0.0F, 0.0F};
};

struct ScreenResolveSettings {
    GiMode mode = GiMode::Dynamic;
    /// The fraction of pixels that trace a final-gather ray this frame: the `TracingResolution`
    /// lever's value squared (it is a linear scale). One traces every pixel; zero none.
    f32 ray_density = 1.0F;
    /// A final-gather ray's reach, in metres.
    f32 max_ray_distance_metres = 40.0F;
    /// How far off the surface a pixel's rays start, in metres, along its normal: the hardware
    /// tier's lift. The software tier lifts at least one finest voxel (`world_ray`).
    f32 origin_lift_metres = 0.02F;
    ErrorTargets error_targets{};
    ReflectionThresholds reflection_thresholds{};
    /// `GiRegionSettings::diffuse_intensity` and `specular_intensity` for the view.
    f32 diffuse_intensity = 1.0F;
    f32 specular_intensity = 1.0F;
    /// The length of the sky visibility sequence one ray a frame walks through.
    u32 sky_visibility_rays = 8;
};

/// What the resolve reads. The world tier is the one `TieredTracer` would select: hardware when
/// `hardware` is available, the software tier otherwise.
struct ScreenResolveSources {
    const RadianceCache* cache = nullptr;
    const DistanceField* field = nullptr;
    const SoftwareTracer* software = nullptr;
    const HardwareTracer* hardware = nullptr;
    SkyTerm sky{};
    /// The frame the hashes and the reflection lobe's rotation are keyed by.
    u64 frame = 0;
    /// The radiance cache's clock: a probe's staleness is this frame minus its last update.
    u64 cache_frame = 0;
};

/// One pixel's two signals before the denoiser.
struct ScreenPixel {
    ResolveResult diffuse{};
    ResolveResult specular{};
    bool traced_diffuse = false;
    bool traced_specular = false;
};

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

/// One pixel of the resolve. `view_direction` is from the eye to the surface.
[[nodiscard]] ScreenPixel resolve_screen_pixel(u32 pixel, Vec3 position, Vec3 normal,
                                               f32 roughness, f32 camera_distance,
                                               Vec3 view_direction,
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

private:
    u32 width_ = 0;
    u32 height_ = 0;
    u32 traced_ = 0;
    Array<Vec3> diffuse_;
    Array<Vec3> specular_;
    Array<f32> samples_;
};

/// The view's surfaces as `denoise::GuidanceBuffers`.
[[nodiscard]] denoise::GuidanceBuffers screen_guidance(const ScreenSurfaces& surfaces) noexcept;

/// What `screen_history` needs of the two frames.
struct ScreenHistoryInputs {
    /// This frame's and last frame's world-to-clip, both unjittered.
    Mat4 current_world_to_clip = Mat4::identity();
    Mat4 previous_world_to_clip = Mat4::identity();
    /// Last frame's linear depth, the same size as this frame's. Empty is no history: a first
    /// frame, a cut, a resize.
    Span<const f32> previous_depth;
    /// `classify_history`'s relative depth tolerance.
    f32 depth_tolerance = 0.05F;
    /// Regions whose illumination was invalidated this frame — a light that moved, an object that
    /// appeared — the same boxes the caller filed with `GiScene::invalidate`. A pixel inside one
    /// rejects its history, so the accumulation restarts from what the caches answer now rather
    /// than averaging the old lighting away one frame in thirty-two. At most
    /// `kScreenHistoryRegions` are read.
    Span<const Aabb> invalidated;
};

/// The most invalidated regions one frame's history reads.
inline constexpr u32 kScreenHistoryRegions = 4;

/// The reprojection's answer for each pixel, through `temporal-rendering`: the pixel of last frame
/// it lands on, or -1, and a confidence of 1 or 0. The surface is taken to be where it was — the
/// camera's motion alone — and `classify_history`'s depth test rejects whatever moved. A pixel
/// inside an invalidated region keeps no history.
[[nodiscard]] Status screen_history(const ScreenSurfaces& surfaces,
                                    const ScreenHistoryInputs& inputs, Span<i32> source,
                                    Span<f32> confidence) noexcept;

}  // namespace cy::rendering::gi
