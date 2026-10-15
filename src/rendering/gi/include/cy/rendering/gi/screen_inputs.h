// SPDX-License-Identifier: MIT
#pragma once
// What a frame's final gather reads and how it is configured: the view's surfaces, the settings,
// the sources and the history inputs, shared by the per-pixel resolve (screen_resolve.h) and the
// screen-probe gather (screen_probes.h). Issue #35, stage 4 and its follow-up.

#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
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
    /// The frame pixels one entry stands for each way: the resolve grid's divisor, one for the
    /// frame itself. The screen probes' spacing is in frame pixels and is divided by it.
    u32 divisor = 1;
};

/// WHICH FINAL GATHER A FRAME RUNS: the lever that keeps the per-pixel gather selectable.
enum class GiGather : u8 {
    /// Probes on a screen grid, traced, filtered over time and integrated per pixel
    /// (screen_probes.h). The default: the frame's rays scale with the probes, not the pixels.
    ScreenProbes = 0,
    /// Every pixel its own traced cache query and final-gather ray (`resolve_screen_pixel`).
    PerPixel,
};

[[nodiscard]] const char* gi_gather_name(GiGather gather) noexcept;

/// The screen-probe gather's settings. See screen_probes.h for what each one buys.
struct ScreenProbeSettings {
    /// Frame pixels between uniform probes: 8 or 16. Adaptive probes are placed on the half-tile
    /// cells between them where the uniform ones cannot be interpolated.
    u32 spacing_pixels = 16;
    /// Rays each probe traces a frame, at most `kScreenProbeMaxRays`. The resolve's `ray_density`
    /// scales them, as it scales the per-pixel gather's rays.
    u32 rays = 32;
    /// A half-tile cell takes an adaptive probe where the uniform probes' interpolation weights
    /// about its centre sum below this.
    f32 adaptive_threshold = 0.5F;
    /// The plane-distance weight's reach: a surface this fraction of its view depth off a probe's
    /// plane takes nothing from it.
    f32 plane_tolerance = 0.05F;
    /// The share of a probe's rays drawn from the guide (the previous radiance, or the cache's),
    /// at most 0.9; the rest are half uniform and half cosine over the hemisphere.
    f32 importance_fraction = 0.5F;
    /// The most frames a probe's temporal blend averages: its weight is never below one over this.
    /// Two: the denoiser's own accumulation follows, and every frame more a probe averages is a
    /// frame later the image settles after a light moves (four took a moved light from 13 frames
    /// to 18 in the GI test room, for no measurable gain in stillness).
    u32 history_frames = 2;
    /// Whether each pixel also combines the radiance cache's answer, interpolated from the probes,
    /// by its confidence — as the per-pixel gather combines it at the pixel. Off, the cache only
    /// stands in for a ray it does not trust.
    bool cache_blend = false;
    /// The samples one pixel's integrated value carries into the denoiser's accumulation. One is
    /// the per-pixel gather's single ray; a probe-integrated value is an average of many rays and
    /// earns more, so the accumulation follows a change sooner.
    f32 pixel_samples = 1.0F;
    /// The most a-trous passes the shared denoiser's spatial filter runs over the integrated
    /// signal: it is an average of many rays, and its frame-to-frame noise is a fifth to a
    /// twentieth of the one-ray signal's, so the cascade's later passes — 1.4 ms each at 1080p —
    /// buy little. See `screen_signal_config`.
    u32 filter_passes = 1;
};

struct ScreenResolveSettings {
    GiMode mode = GiMode::Dynamic;
    /// Which final gather runs. The per-pixel one stays selectable.
    GiGather gather = GiGather::ScreenProbes;
    ScreenProbeSettings probes{};
    /// The fraction of pixels that trace a final-gather ray this frame: the `TracingResolution`
    /// lever's value squared (it is a linear scale). One traces every pixel; zero none. The probe
    /// gather traces this fraction of each probe's rays, at least four.
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

/// The most invalidated regions one frame's history reads.
inline constexpr u32 kScreenHistoryRegions = 4;

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
    /// The two frames' matrices and invalidated regions the screen probes reproject through. Null,
    /// or with no previous depth, is no history: every probe starts from its own rays.
    const ScreenHistoryInputs* history = nullptr;
};

/// One pixel's two signals before the denoiser.
struct ScreenPixel {
    ResolveResult diffuse{};
    ResolveResult specular{};
    bool traced_diffuse = false;
    bool traced_specular = false;
};

}  // namespace cy::rendering::gi
