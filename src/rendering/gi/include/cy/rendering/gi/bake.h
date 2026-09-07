#pragma once
// The offline path tracer: ground truth, the bake, and the seeds it leaves. Task 9.2.
//
// `rendering-global-illumination` — "Offline path tracer and ground truth", "Lightmap baking"
// (partially — see the note below), "Irradiance volumes and light probes".
//
// ================================================================================================
// ONE TRACER, TWO JOBS, AND THAT IS THE POINT
// ================================================================================================
//
// "The offline path tracer that produces [baked lighting] is the same one that produces
// ground-truth references for measuring how wrong the real-time result is." So `PathTracer` is used
// for both, and it shares the GI scene, the lights and the sky with the real-time path — it holds a
// `SceneTracer`, which is the same interface the probe gather and the reflection capture hold, so
// the reference and the real-time answer are computed against the same world by construction.
//
// A reference computed against a second representation would measure the difference between the two
// representations, which is the one thing a ground truth must not do.
//
// ================================================================================================
// WHAT IS HERE AND WHAT IS NOT, SAID PLAINLY
// ================================================================================================
//
// HERE: the path tracer, ground-truth irradiance, the reference comparison, and seeding of the
// surface cache, the radiance cache and the reflection probes — which is
// "the bake SHALL additionally produce seeds for the dynamic caches... so a level in Hybrid mode is
// plausible before convergence", and it is what makes `Baked` and `Hybrid` real modes here.
//
// NOT HERE, AND NOT CLAIMED: lightmap atlases. UV2 unwrapping, chart packing, border dilation and
// the atlas format are a cook-side pipeline that `asset-import-pipeline` and the texture cook own,
// and none of it exists at M7. `rendering-global-illumination` is at **Working** for this milestone
// and the lightmap rows are what the remaining distance to Complete is made of. A `Lightmap` source
// enumerator exists and `exclusion_for()` handles it correctly, so a lightmap that arrives later
// slots into the resolve without changing it — but nothing in this module produces one today.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/reflections.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>

namespace cy::rendering::gi {

struct BakeSettings {
    /// Path length. Two is the usual bake and is enough for a room; one is direct plus one bounce.
    u32 bounces = 2;
    /// Samples per bake point. The ground-truth reference wants many; a seed wants few.
    u32 samples = 64;
    /// The sequence's seed. Fixed, so a bake is reproducible and a reference is a number rather
    /// than a draw.
    u32 seed = 0x9E3779B9U;
    f32 max_distance_metres = 60.0F;
};

/// The offline path tracer.
class PathTracer {
public:
    /// `occluder` shadows the direct term. It is passed rather than recovered from `tracer`
    /// because the engine compiles with -fno-rtti: a cross-cast from `SceneTracer` to `Occluder`
    /// would need a dynamic_cast this codebase cannot have, and every tier here implements both.
    PathTracer(const SceneTracer& tracer, const GiScene& scene, Span<const GiLight> lights,
               const SkyTerm& sky, const Occluder* occluder) noexcept
        : tracer_(&tracer), scene_(&scene), lights_(lights), sky_(sky), occluder_(occluder) {}

    /// Radiance along one ray, path traced to `bounces`.
    [[nodiscard]] Vec3 radiance(Vec3 origin, Vec3 direction, u32 bounces, f32 max_distance,
                                u32& sequence) const noexcept;

    /// Ground truth at a point: the mean incoming radiance over the cosine-weighted hemisphere
    /// about `normal`.
    ///
    /// Returned as a RADIANCE rather than an irradiance — the mean, not the integral — because that
    /// is what `RadianceCache::gather` and `decode_payload` return, and a reference in different
    /// units from the thing it measures is a reference that measures the unit conversion.
    [[nodiscard]] Vec3 irradiance(Vec3 position, Vec3 normal,
                                  const BakeSettings& settings) const noexcept;

    [[nodiscard]] u64 rays_traced() const noexcept { return rays_; }
    void reset_ray_count() noexcept { rays_ = 0; }

private:
    const SceneTracer* tracer_;
    const GiScene* scene_;
    Span<const GiLight> lights_;
    SkyTerm sky_;
    const Occluder* occluder_ = nullptr;
    mutable u64 rays_ = 0;
};

struct BakeReport {
    u32 surface_pages_seeded = 0;
    u32 probes_seeded = 0;
    u32 reflection_probes_seeded = 0;
    u64 rays = 0;
};

/// Seed the dynamic caches from the path tracer, so a level is plausible on its first frame.
///
/// `reflections` may be null. `cache` and `surfaces` are filled in place: this is the "bake seeds
/// the dynamic caches" scenario, and after it a `Hybrid` scene renders without converging from
/// black.
[[nodiscard]] Expected<BakeReport, Error> seed_caches(const PathTracer& tracer,
                                                      SurfaceCache& surfaces, RadianceCache& cache,
                                                      ReflectionProbeSet* reflections,
                                                      Span<const GiLight> lights,
                                                      const Occluder* occluder,
                                                      const BakeSettings& settings) noexcept;

/// How wrong the real-time answer is against the reference.
///
/// "WHEN comparing the real-time result against the reference THEN the difference SHALL be
/// measurable" — this struct is that measurement, and the numbers in it are what M7's tolerance is
/// expressed in.
struct ReferenceComparison {
    u32 samples = 0;
    f32 mean_absolute_error = 0.0F;
    f32 max_absolute_error = 0.0F;
    /// The mean absolute error divided by the mean reference magnitude. The figure to lead with:
    /// it is scale-free and it reproduces, where the maximum is a single draw.
    f32 relative_error = 0.0F;
    f32 mean_reference_magnitude = 0.0F;
};

[[nodiscard]] ReferenceComparison compare_against_reference(Span<const Vec3> measured,
                                                            Span<const Vec3> reference) noexcept;

}  // namespace cy::rendering::gi
