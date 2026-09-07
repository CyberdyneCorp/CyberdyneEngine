#pragma once
// The resolve: combine sources by confidence, without double counting; and convergence. Task 9.2.
//
// `rendering-global-illumination` — "GI strategy layers" (no double counting), "Sample confidence"
// (blending, not switching), "Far-field illumination", "Convergence and capture".
//
// ================================================================================================
// COMBINING BY CONFIDENCE IS NOT THE SAME AS PICKING THE BEST
// ================================================================================================
//
// `combine()` is a confidence-weighted mean, and that is the requirement: "Resolve SHALL combine
// sources weighted by confidence rather than selecting one, so transitions between sources are not
// visible." A max would be one line shorter and would pop every time the ranking changed — which is
// every time the camera turns past a screen edge.
//
// ================================================================================================
// NO DOUBLE COUNTING IS AN EXCLUSION MASK, DECIDED BY THE SURFACE
// ================================================================================================
//
// "WHEN a lightmapped surface is inside a dynamic GI region THEN it SHALL take indirect diffuse
// from the lightmap only, and the dynamic contribution SHALL be excluded for it." The decision is
// per surface, not per region, so it travels as a bitmask of `RadianceSource` on the resolve call.
// `exclusion_for()` builds it from the GI mode and what the surface has, in one place, because two
// call sites that computed it separately would disagree in `Hybrid`, which is the only mode where
// it is interesting.
//
// ================================================================================================
// CONVERGENCE IS A MEASUREMENT, NOT A FEELING
// ================================================================================================
//
// A hybrid renderer's image keeps changing after the world stops. A golden-image test that does not
// know that is a flaky test, and "this lighting change has not finished converging" is a question
// somebody asks at three in the morning. `ConvergenceTracker` answers both: it holds a per-region
// exponential trace of how much the illumination state is still moving, and `converged()` is what a
// capture waits on.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

struct ResolveResult {
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    /// The best confidence among the sources that contributed. Zero when nothing did.
    f32 confidence = 0.0F;
    /// Which source contributed most of the weight. For the "which source answered this pixel"
    /// diagnostic and for nothing that decides what to render.
    RadianceSource dominant = RadianceSource::None;
    /// One bit per source that contributed. The other half of the same diagnostic.
    u32 sources_used = 0;
    u32 sources_excluded = 0;
};

/// Combine samples by confidence, skipping any source in `excluded_sources`.
[[nodiscard]] ResolveResult combine(Span<const RadianceSample> samples,
                                    u32 excluded_sources) noexcept;

/// The exclusion mask a surface declares, from the GI mode and what it carries.
///
/// The one place the double-counting rule is written. `Hybrid` is the only interesting row: a
/// lightmapped surface in a dynamic region takes the lightmap and excludes the dynamic diffuse
/// sources, and a surface without a lightmap in the same region takes them all.
[[nodiscard]] u32 exclusion_for(GiMode mode, bool surface_has_lightmap,
                                bool surface_has_irradiance_volume) noexcept;

/// How much of a query at `distance_metres` from the camera should come from the far field.
///
/// Zero inside the near field, one well beyond it, and a smooth ramp between: "no visible
/// boundary" is that ramp, and the width of it is the gap between the two error targets.
[[nodiscard]] f32 far_field_weight(f32 distance_metres, const ErrorTargets& targets) noexcept;

/// Per-region convergence.
///
/// A region is a cube of `region_size_metres`. The tracker holds an exponential trace of the
/// relative change observed in it, so one noisy update does not report a region unconverged and a
/// region that keeps moving does not report converged.
class ConvergenceTracker {
public:
    ConvergenceTracker() noexcept;

    void configure(f32 region_size_metres, f32 smoothing) noexcept;

    /// Record that the illumination state in the region containing `position` changed by
    /// `relative_error` — the number the surface and radiance caches already produce.
    void observe(Vec3 position, f32 relative_error) noexcept;

    /// 1 when the region containing `position` has settled and 0 when it is moving as much as it
    /// ever was. A region nothing has observed reports 0: unknown is not converged.
    [[nodiscard]] f32 convergence(Vec3 position) const noexcept;

    /// The least converged region, over everything observed. What a capture waits on.
    [[nodiscard]] f32 worst_convergence() const noexcept;
    [[nodiscard]] bool converged(f32 threshold) const noexcept;
    [[nodiscard]] u32 unconverged_regions(f32 threshold) const noexcept;
    [[nodiscard]] u32 region_count() const noexcept;

    void reset() noexcept;

private:
    [[nodiscard]] u64 key_of(Vec3 position) const noexcept;

    f32 region_size_ = 4.0F;
    f32 smoothing_ = 0.25F;
    HashMap<u64, f32> regions_;
};

}  // namespace cy::rendering::gi
