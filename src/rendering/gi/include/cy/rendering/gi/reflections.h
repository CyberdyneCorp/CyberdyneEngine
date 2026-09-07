#pragma once
// Reflections, on the same infrastructure as diffuse GI. Task 9.2.
//
// `rendering-global-illumination` — "Reflections share the illumination infrastructure",
// "Reflection probes".
//
// ================================================================================================
// THERE IS NOTHING HERE BUT A RAY DISTRIBUTION AND A ROUGHNESS RULE
// ================================================================================================
//
// "Specular indirect lighting SHALL use the same GI scene, caches, tracing tiers, resolve, and
// denoiser as diffuse indirect lighting, differing only in ray distribution and roughness
// handling." So this file contains no scene, no cache and no denoiser. It contains:
//
//   * `reflection_strategy()` — the roughness table, which is the ray distribution;
//   * `reflection_ray_count()` — "broader lobes... reduce ray count", as a number;
//   * `ReflectionProbeSet` — one SOURCE within the hierarchy, consulted when the tiers report low
//     confidence and primary in `Baked` and `Probe` modes.
//
// A `ReflectionProbeSet` is not a parallel cache. It is a captured environment with an influence
// volume, and it is combined by `resolve.h`'s `combine()` alongside every other source — which is
// why "probe as a confidence fallback" is a weight rather than a branch.
//
// ================================================================================================
// BOX PROJECTION IS THE REASON A PROBE LOOKS RIGHT IN A ROOM
// ================================================================================================
//
// An environment map is captured at a point and sampled by direction, so every surface in a room
// reflects the same thing and the reflection slides as the camera moves. Intersecting the
// reflection ray with the probe's box and re-aiming at the hit point fixes that, and it is the
// difference between a probe that reads as a reflection and one that reads as a shiny fog.
//
// ================================================================================================
// A REALTIME PROBE IS AMORTISED, WHICH IS WHY CAPTURE IS A STATE MACHINE
// ================================================================================================
//
// "WHEN a realtime probe updates THEN its faces and filtering mips SHALL be spread across several
// frames under the GI budget." `capture()` therefore does a bounded number of texels per call and
// the probe becomes valid only when the last one lands — a probe half-captured is a probe with its
// previous contents, not a probe with half a room in it.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

/// How a reflection at this roughness is answered.
enum class ReflectionStrategy : u8 {
    /// Near-mirror: dedicated rays at the highest available tier.
    DedicatedRays = 0,
    /// Moderate: sparse traced rays combined with the radiance cache.
    SparseRaysAndCache,
    /// Rough: the radiance cache alone, with no dedicated reflection rays.
    CacheOnly,
    Count,
};

[[nodiscard]] const char* reflection_strategy_name(ReflectionStrategy strategy) noexcept;

/// The thresholds the table is read at. Per quality tier, which is why they are data.
struct ReflectionThresholds {
    /// At or below this roughness a surface is near-mirror.
    f32 mirror_max_roughness = 0.10F;
    /// At or above this roughness dedicated rays buy nothing the cache does not already have.
    f32 cache_min_roughness = 0.60F;
    /// Rays a near-mirror surface gets. The count falls with roughness from here to one.
    u32 max_rays = 8;
};

[[nodiscard]] ReflectionStrategy reflection_strategy(
    f32 roughness, const ReflectionThresholds& thresholds) noexcept;

/// "Broader lobes SHALL widen the filter and reduce ray count." This is the second half; the first
/// is `denoise::SignalConfig::roughness_widening`.
[[nodiscard]] u32 reflection_ray_count(f32 roughness,
                                       const ReflectionThresholds& thresholds) noexcept;

/// A direction in the reflection lobe around `reflection`, for sample `index` of `count`.
/// Deterministic, and it spreads with roughness — which is the whole of "differing only in ray
/// distribution".
[[nodiscard]] Vec3 reflection_lobe_direction(Vec3 reflection, Vec3 normal, f32 roughness, u32 index,
                                             u32 count, u32 rotation) noexcept;

enum class ProbeCaptureMode : u8 {
    /// Captured once, offline.
    Baked = 0,
    /// Captured when something asks.
    OnDemand,
    /// Re-captured continuously, amortised across frames under the GI budget.
    Realtime,
    Count,
};

/// Roughness-filtered levels of a probe's radiance. All three are the same resolution: it is the
/// FILTER that changes and not the resolution, which costs a little more memory and makes a sample
/// one fetch and a lerp instead of a mip chain with a bias nobody can tune.
inline constexpr u32 kReflectionLevels = 3;
inline constexpr u32 kReflectionEdge = 4;
inline constexpr u32 kReflectionTexels = kReflectionEdge * kReflectionEdge;
inline constexpr u32 kReflectionFloats = kReflectionLevels * kReflectionTexels * 3;

struct ReflectionProbe {
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// The influence volume. A box when `is_box`, otherwise a sphere of `radius` about `position`.
    Aabb box{};
    f32 radius = 5.0F;
    bool is_box = false;
    /// Metres over which the probe's influence fades out at the volume's edge.
    f32 blend_distance = 1.0F;
    /// Resolves overlap: the higher importance wins the larger share.
    f32 importance = 1.0F;
    /// An interior probe excludes the sky, so a room does not reflect a sky it cannot see.
    bool interior = false;
    /// Re-aim the reflection ray at the probe box. See the header comment.
    bool box_projection = true;
    ProbeCaptureMode capture_mode = ProbeCaptureMode::OnDemand;
    /// How many of the sharp level's texels have been captured. `kReflectionTexels` is complete.
    u32 captured_texels = 0;
    u64 last_capture_frame = 0;
    bool valid = false;
    bool live = false;
};

struct ReflectionCaptureReport {
    u32 probes_touched = 0;
    u32 texels_captured = 0;
    u32 probes_completed = 0;
    u32 rays = 0;
};

struct ReflectionDiagnostics {
    u32 probe_count = 0;
    u32 valid_probes = 0;
    u64 bytes = 0;
    u32 samples = 0;
    u32 sample_misses = 0;
    ReflectionCaptureReport last_capture{};
};

/// What a capture needs. `tracer` is the same `SceneTracer` the probe gather and the bake hold, and
/// `radiance` the same surface cache: a reflection probe is captured through the illumination
/// infrastructure and not through a second render of the scene.
struct ReflectionCaptureContext {
    const SceneTracer* tracer = nullptr;
    const RadianceLookup* radiance = nullptr;
    SkyTerm sky{};
    f32 max_distance_metres = 60.0F;
    /// Texels per call, across all probes. The amortisation.
    u32 texel_budget = 16;
    u64 frame = 0;
    /// A realtime probe is re-captured once this many frames have passed since it completed.
    u64 realtime_interval_frames = 60;
};

/// One source within the illumination hierarchy.
class ReflectionProbeSet {
public:
    ReflectionProbeSet() noexcept;

    [[nodiscard]] Expected<u32, Error> add(const ReflectionProbe& probe) noexcept;
    void remove(u32 handle) noexcept;
    [[nodiscard]] u32 probe_count() const noexcept { return live_probes_; }
    /// One past the largest handle ever issued. A caller walking every probe walks this and checks
    /// `probe(handle).live`, because handles are recycled and the live set has holes.
    [[nodiscard]] u32 handle_capacity() const noexcept { return static_cast<u32>(probes_.size()); }
    [[nodiscard]] const ReflectionProbe& probe(u32 handle) const noexcept {
        return probes_[handle];
    }

    /// Mark a probe for re-capture. What an on-demand probe's trigger and an invalidation both do.
    void request_capture(u32 handle) noexcept;
    u32 invalidate(const Aabb& region) noexcept;

    /// Capture up to `context.texel_budget` texels, spread across the probes that want them.
    ReflectionCaptureReport capture(const ReflectionCaptureContext& context) noexcept;

    /// The probes whose influence volume contains `position`, by descending influence weight.
    /// Returns how many were written. This is the cluster assignment's answer, computed here so a
    /// test can check the blend without a renderer.
    u32 assign(Vec3 position, Span<u32> out_handles, Span<f32> out_weights) const noexcept;

    /// Sample the set at a point, in a reflection direction, at a roughness. Blended by influence
    /// weight and returned with a confidence, so the resolve weights it in rather than switching.
    [[nodiscard]] RadianceSample sample(Vec3 position, Vec3 reflection,
                                        f32 roughness) const noexcept;

    [[nodiscard]] const ReflectionDiagnostics& diagnostics() const noexcept { return diagnostics_; }

    /// Seed a probe's levels directly, which is what a bake does.
    [[nodiscard]] Status seed(u32 handle, Span<const Vec3> directions,
                              Span<const Vec3> radiance) noexcept;

private:
    [[nodiscard]] f32* levels(u32 handle) noexcept;
    [[nodiscard]] const f32* levels(u32 handle) const noexcept;
    [[nodiscard]] static Vec3 capture_texel(const ReflectionProbe& probe, Vec3 direction,
                                            const ReflectionCaptureContext& context,
                                            ReflectionCaptureReport& report) noexcept;
    [[nodiscard]] static bool wants_capture(ReflectionProbe& probe,
                                            const ReflectionCaptureContext& context) noexcept;
    [[nodiscard]] static f32 influence(const ReflectionProbe& probe, Vec3 position) noexcept;
    void filter_levels(u32 handle) noexcept;
    void account() noexcept;

    Array<ReflectionProbe> probes_;
    Array<f32> payloads_;
    Array<u32> free_probes_;
    u32 live_probes_ = 0;
    mutable ReflectionDiagnostics diagnostics_{};
};

}  // namespace cy::rendering::gi
