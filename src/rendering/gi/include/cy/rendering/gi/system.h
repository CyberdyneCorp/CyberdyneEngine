#pragma once
// The composition: the named subsystems, wired, and one frame of illumination. Tasks 9.1 and 9.2.
//
// ================================================================================================
// THIS IS NOT THE SINGLE ILLUMINATION MODULE THE SPECIFICATION FORBIDS
// ================================================================================================
//
// `rendering-global-illumination` requires the system to be "decomposed into named subsystems with
// separate ownership — GI scene, surface cache, distance field, radiance cache, tracers (screen,
// software, hardware), resolve, reflections, denoising, baking, and diagnostics — rather than a
// single illumination module."
//
// Every one of those is a type of its own in a file of its own, and this class holds them and wires
// them. It contains no illumination logic: `update()` is a sequence of calls into the subsystems in
// the order their data flows, and `indirect_diffuse()` gathers samples and hands them to
// `resolve.h`'s `combine`. The test that keeps that honest is that the caches reach each other only
// through the abstract seams in lighting.h — replace `RadianceCache` with a baked irradiance volume
// and this file changes by one line.
//
// ================================================================================================
// THE ORDER IN `update()` IS THE DATA FLOW AND NOT A PREFERENCE
// ================================================================================================
//
//   1. the budget, because every step below reads a lever value from it;
//   2. invalidations, so nothing below updates something that is about to be invalidated;
//   3. the distance field, because the software tier traces it;
//   4. the acceleration structures, because the hardware tier traces them;
//   5. probe placement, because the probes updated in 7 must exist;
//   6. the surface cache, because the probes gather from it;
//   7. the radiance cache, because the surface cache's next update gathers from it — which is the
//      feedback loop that makes the bounces accumulate;
//   8. the reflection probes, which capture through the same tracer and the same surface cache;
//   9. convergence, which observes what 6 and 7 reported.
//
// Swapping 6 and 7 costs one frame of latency in the bounce and is not otherwise wrong; every other
// pair is ordered by a real dependency.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/gi/budget.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/reflections.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/tracing.h>
#include <cy/rendering/raytracing/acceleration.h>

namespace cy::rendering::gi {

struct IlluminationSettings {
    GiMode mode = GiMode::Dynamic;
    ErrorTargets error_targets{};
    ClipmapSettings field{};
    ProbeCacheSettings probes{};
    ReflectionThresholds reflection_thresholds{};
    GiBudgetSettings budget{};
    SkyTerm sky{};
    rt::AccelerationService::ServiceConfig ray_tracing{};
    /// How far a world ray reaches, in metres.
    f32 max_ray_distance_metres = 40.0F;
    /// The convergence tracker's region size.
    f32 convergence_region_metres = 4.0F;
    /// Rays a probe gathers per update at lever position 0. The budget scales it down from here.
    u32 rays_per_probe = 32;
};

struct FrameContext {
    Vec3 camera{0.0F, 0.0F, 0.0F};
    Span<const GiLight> lights;
    u64 frame = 0;
    /// THIS SYSTEM'S OWN measured cost in milliseconds. Never a frame time — see budget.h.
    f32 measured_gi_ms = 0.0F;
    /// Set by the renderer budget arbiter when it grants this system a step back up.
    bool permit_relaxation = false;
    /// The screen tier's buffers, or null on a view that has none.
    const ScreenView* screen = nullptr;
    Aabb importance_region{};
    bool has_importance_region = false;
    /// Forces every scheduler to full rates. `advance_to_convergence` sets it; a frame does not.
    bool converged_mode = false;
};

struct IlluminationFrameReport {
    ScrollReport field{};
    rt::FrameReport acceleration{};
    ProbePlacementReport placement{};
    SurfaceUpdateReport surfaces{};
    ProbeUpdateReport probes{};
    ReflectionCaptureReport reflections{};
    GiBudgetReport budget{};
    /// Which world tier answered this frame's rays. For the diagnostics, and for the fallback test.
    RadianceSource world_tier = RadianceSource::None;
    /// Invalidation records serviced this frame, and what they cost.
    u32 invalidations_serviced = 0;
    u32 invalidated_probes = 0;
    u32 invalidated_surface_pages = 0;
    u32 invalidated_field_bricks = 0;
    /// The least converged region, in [0, 1].
    f32 convergence = 0.0F;
};

/// What the surface being resolved carries. The double-counting decision needs it.
struct SurfaceProperties {
    bool has_lightmap = false;
    bool has_irradiance_volume = false;
    Vec3 lightmap_radiance{0.0F, 0.0F, 0.0F};
    Vec3 irradiance_volume_radiance{0.0F, 0.0F, 0.0F};
    /// Distance from the camera, for the far-field transition.
    f32 camera_distance_metres = 0.0F;
};

/// The subsystems, wired.
class IlluminationSystem {
public:
    IlluminationSystem() noexcept;

    [[nodiscard]] Status configure(const IlluminationSettings& settings) noexcept;
    [[nodiscard]] const IlluminationSettings& settings() const noexcept { return settings_; }

    void set_mode(GiMode mode) noexcept { settings_.mode = mode; }
    [[nodiscard]] GiMode mode() const noexcept { return settings_.mode; }

    // The named subsystems. Public because they are separate subsystems and not private state:
    // a renderer ingests cells into the scene, a residency system places distance fields, and an
    // editor inspects the caches, and none of that should go through a forwarding method here.
    [[nodiscard]] GiScene& scene() noexcept { return scene_; }
    [[nodiscard]] const GiScene& scene() const noexcept { return scene_; }
    [[nodiscard]] DistanceField& field() noexcept { return field_; }
    [[nodiscard]] const DistanceField& field() const noexcept { return field_; }
    [[nodiscard]] SurfaceCache& surfaces() noexcept { return surfaces_; }
    [[nodiscard]] const SurfaceCache& surfaces() const noexcept { return surfaces_; }
    [[nodiscard]] RadianceCache& radiance() noexcept { return radiance_; }
    [[nodiscard]] const RadianceCache& radiance() const noexcept { return radiance_; }
    [[nodiscard]] ReflectionProbeSet& reflection_probes() noexcept { return reflections_; }
    [[nodiscard]] const ReflectionProbeSet& reflection_probes() const noexcept {
        return reflections_;
    }
    [[nodiscard]] GiBudget& budget() noexcept { return budget_; }
    [[nodiscard]] const GiBudget& budget() const noexcept { return budget_; }
    [[nodiscard]] denoise::Denoiser& denoiser() noexcept { return denoiser_; }
    [[nodiscard]] rt::AccelerationService& acceleration() noexcept { return acceleration_; }
    [[nodiscard]] TieredTracer& tracer() noexcept { return tracer_; }
    [[nodiscard]] const WorldTracer& world_tracer() const noexcept { return world_; }
    [[nodiscard]] const ConvergenceTracker& convergence() const noexcept { return convergence_; }

    /// One frame. See the header comment for the order and why it is that order.
    IlluminationFrameReport update(const FrameContext& context) noexcept;

    /// Indirect diffuse at a surface, every source combined by confidence and the double-counting
    /// rule applied.
    [[nodiscard]] ResolveResult indirect_diffuse(Vec3 position, Vec3 normal,
                                                 const SurfaceProperties& surface) const noexcept;

    /// Indirect specular. The same scene, caches, tiers and resolve; a different ray distribution
    /// and a roughness rule, which is the whole difference.
    [[nodiscard]] ResolveResult indirect_specular(Vec3 position, Vec3 normal, Vec3 view_direction,
                                                  f32 roughness, u32 frame,
                                                  TraceBudget& budget) noexcept;

    /// Converged mode: advance frames at full update rates until the convergence metric passes
    /// `threshold` or `max_frames` have run. Returns the frames it took.
    ///
    /// "Converged mode SHALL be used by golden-image tests, cinematic capture, and reference
    /// comparison, so temporal convergence does not make rendering tests unreliable."
    u32 advance_to_convergence(const FrameContext& context, f32 threshold, u32 max_frames) noexcept;

private:
    void service_invalidations(IlluminationFrameReport& report, u64 frame) noexcept;
    [[nodiscard]] bool dynamic_enabled() const noexcept;

    IlluminationSettings settings_{};
    GiScene scene_;
    DistanceField field_;
    SurfaceCache surfaces_;
    RadianceCache radiance_;
    ReflectionProbeSet reflections_;
    GiBudget budget_;
    ConvergenceTracker convergence_;
    denoise::Denoiser denoiser_;
    rt::AccelerationService acceleration_;
    SoftwareTracer software_;
    HardwareTracer hardware_;
    WorldTracer world_;
    TieredTracer tracer_;
};

}  // namespace cy::rendering::gi
