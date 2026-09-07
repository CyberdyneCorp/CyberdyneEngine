#pragma once
// Stochastic many-light direct lighting: reservoir sampling with temporal and spatial reuse.
// Task 10.3.
//
// `rendering-lighting-and-shadows` — "Stochastic many-light direct lighting".
//
// ================================================================================================
// WHAT A RESERVOIR IS, AND WHY THE ESTIMATOR IS THE THING TO GET RIGHT
// ================================================================================================
//
// Resampled importance sampling: draw M candidate lights from a cheap distribution, keep ONE of
// them with probability proportional to a better target function, and carry the weight that makes
// the result unbiased. A reservoir is that one sample plus the running weight sum, so it can be
// updated in constant memory and — crucially — COMBINED with another reservoir. Combining is what
// makes temporal reuse (this pixel's reservoir from last frame) and spatial reuse (a neighbour's)
// almost free: a pixel ends up having effectively considered thousands of candidates while having
// evaluated a handful.
//
// The estimator's weight is `weight_sum / (M * target(sample))`. Getting that expression wrong does
// not crash and does not look obviously wrong — it looks like a scene that is slightly too bright
// or slightly too dark in the places with the most lights. `tests/test_many_light.cpp` measures it
// against a reference sum over every light, which is the only way to find out.
//
// ================================================================================================
// TWO RULES THAT ARE CONFIGURATION, NOT CODE
// ================================================================================================
//
// 1. "Clustered lighting SHALL remain the **default shipping path**. The stochastic path SHALL be a
//    profile and scene decision." So `ManyLightSettings::enabled` defaults to false and
//    `active_lighting_path()` answers what is actually in force.
//
// 2. "This path SHALL NOT be enabled without denoising... that dependency SHALL be enforced by
//    configuration validation rather than discovered visually." `validate_many_light()` is that
//    enforcement, and it refuses rather than quietly falling back — a silent fallback is exactly
//    the "discovered visually" the requirement rules out.
//
// ================================================================================================
// AND ONE THAT IS ARCHITECTURE
// ================================================================================================
//
// "Direct and indirect illumination SHALL remain separate solvers. Many-light direct lighting SHALL
// NOT be implemented inside the GI system." Nothing in this file names `cy::rendering::gi`, and
// this module does not depend on it. The visibility rays this path needs are asked for through a
// budget (`ManyLightSettings::visibility_rays_per_pixel`) that the caller spends against whatever
// tracer it has — which is how the two solvers share a ray budget without sharing a lighting model.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::rendering {

/// Which path is in force. Reported alongside light statistics because
/// `rendering-lighting-and-shadows` says so outright: "The active path SHALL be reported alongside
/// light statistics, since a light limit that does not apply is more confusing than one that does."
enum class LightingPath : u8 {
    /// The default shipping path: a bounded number of lights per cluster, iterated.
    Clustered = 0,
    /// Lights importance-sampled per pixel; the per-cluster bound does not apply.
    StochasticManyLight,
    Count,
};

[[nodiscard]] const char* lighting_path_name(LightingPath path) noexcept;

/// One candidate light, as the sampler sees it. Deliberately not `GpuLight`: the sampler needs a
/// weight and an index and nothing else, and taking the full record here would make the estimator
/// impossible to test without building a light.
struct LightCandidate {
    /// Index into the frame's light array. What the reservoir carries and what visibility is traced
    /// against.
    u32 index = 0;
    /// The unshadowed contribution this light would make at the shading point — the target function
    /// the reservoir resamples towards. Radiance times the geometric term; visibility is
    /// deliberately NOT in it, because visibility is what the selected sample pays for.
    f32 unshadowed = 0.0F;
};

/// A reservoir: one selected sample and the weight that makes the estimate unbiased.
struct Reservoir {
    /// The selected candidate's light index, or `kNoLight`.
    u32 light = 0xFFFFFFFFU;
    /// The target function's value at the selected sample.
    f32 target = 0.0F;
    /// The running sum of candidate weights.
    f32 weight_sum = 0.0F;
    /// How many candidates this reservoir stands for, INCLUDING those it inherited by combination.
    /// Capped by `ManyLightSettings::max_history_samples`, which is what stops a temporal reservoir
    /// becoming so confident that it stops responding to a light being switched off.
    u32 sample_count = 0;

    [[nodiscard]] bool valid() const noexcept { return light != 0xFFFFFFFFU && target > 0.0F; }

    /// The unbiased contribution weight: `weight_sum / (M * target)`. Multiplying the selected
    /// light's shadowed contribution by this is the estimate.
    [[nodiscard]] f32 contribution_weight() const noexcept;

    void reset() noexcept;
};

inline constexpr u32 kNoLight = 0xFFFFFFFFU;

/// The sampler's own deterministic stream. Not `<random>`: two runs of one frame must select the
/// same lights, and a standard library engine is not a fixed sequence across platforms.
class SampleStream {
public:
    /// Seeded from the pixel and the frame, so a pixel's sequence is a pure function of where and
    /// when it is — which is what makes a stochastic frame reproducible.
    SampleStream(u32 pixel_x, u32 pixel_y, u32 frame) noexcept;

    [[nodiscard]] f32 next_unit() noexcept;

private:
    u32 state_;
};

/// Update a reservoir with one candidate. Returns whether the candidate was selected.
bool reservoir_update(Reservoir& reservoir, const LightCandidate& candidate, f32 source_pdf,
                      SampleStream& stream) noexcept;

/// Combine `other` into `reservoir`, as if `other`'s candidates had been offered to it.
///
/// `other_target_at_here` is `other`'s selected light's target function evaluated AT THIS pixel,
/// not at the neighbour's. That distinction is the whole of spatial reuse being correct: reusing
/// the neighbour's own target value biases the estimate towards whatever the neighbour was looking
/// at, and the symptom is a soft halo of the wrong colour around every geometric edge.
void reservoir_combine(Reservoir& reservoir, const Reservoir& other, f32 other_target_at_here,
                       u32 max_sample_count, SampleStream& stream) noexcept;

/// The path's settings. A profile and scene decision, per the requirement.
struct ManyLightSettings {
    bool enabled = false;
    /// Candidates drawn per pixel per frame, before any reuse. The bound that replaces the
    /// per-cluster light limit.
    u32 candidates_per_pixel = 32;
    /// Shadow rays or shadow-map lookups spent on the selected samples. The second half of the
    /// bound, and the number that actually costs.
    u32 visibility_rays_per_pixel = 1;
    /// How many spatial neighbours are combined in.
    u32 spatial_neighbours = 3;
    /// The cap on a reservoir's `sample_count` after temporal combination. Without it a reservoir
    /// accumulates confidence forever and stops responding to the world changing — a light switched
    /// off stays lit for seconds.
    u32 max_history_samples = 500;
    /// The path requires denoising. Set by configuration, checked by `validate_many_light`.
    bool denoising_available = false;
};

/// The refusal, with its reason, so a diagnostic names what is missing.
[[nodiscard]] Status validate_many_light(const ManyLightSettings& settings) noexcept;

/// Which path is in force given the settings. `Clustered` whenever the stochastic path is off or
/// unusable — and `validate_many_light` is what turns "unusable" into a diagnostic rather than a
/// silent downgrade.
[[nodiscard]] LightingPath active_lighting_path(const ManyLightSettings& settings) noexcept;

/// What a frame reports about the path that is actually running. The requirement is that the limit
/// REPORTED is the one in force, so the two halves are mutually exclusive by construction: a
/// clustered frame reports a per-cluster bound and no ray budget, and a stochastic frame the other
/// way round.
struct LightingPathStats {
    LightingPath path = LightingPath::Clustered;
    /// Clustered only. Zero under the stochastic path, where it does not apply.
    u32 max_lights_per_cluster = 0;
    u32 lights_dropped_by_bound = 0;
    /// Stochastic only. Zero under the clustered path.
    u32 candidates_per_pixel = 0;
    u32 visibility_rays_per_pixel = 0;
};

[[nodiscard]] LightingPathStats lighting_path_stats(const ManyLightSettings& settings,
                                                    u32 max_lights_per_cluster,
                                                    u32 lights_dropped_by_bound) noexcept;

/// Draw `settings.candidates_per_pixel` candidates from `candidates` into a fresh reservoir. The
/// source distribution is uniform over the candidate list, so `source_pdf` is `1/count` — a caller
/// with a better one (a light BVH, a power distribution) calls `reservoir_update` directly.
[[nodiscard]] Reservoir sample_lights(Span<const LightCandidate> candidates,
                                      const ManyLightSettings& settings,
                                      SampleStream& stream) noexcept;

}  // namespace cy::rendering
