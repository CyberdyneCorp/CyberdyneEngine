#include <cy/rendering/lighting/many_light.h>

#include <cy/core/math/math.h>

namespace cy::rendering {

const char* lighting_path_name(LightingPath path) noexcept {
    switch (path) {
        case LightingPath::Clustered:
            return "clustered";
        case LightingPath::StochasticManyLight:
            return "stochastic-many-light";
        case LightingPath::Count:
            break;
    }
    return "unknown";
}

f32 Reservoir::contribution_weight() const noexcept {
    if (!valid() || sample_count == 0) {
        return 0.0F;
    }
    return weight_sum / (static_cast<f32>(sample_count) * target);
}

void Reservoir::reset() noexcept {
    light = kNoLight;
    target = 0.0F;
    weight_sum = 0.0F;
    sample_count = 0;
}

SampleStream::SampleStream(u32 pixel_x, u32 pixel_y, u32 frame) noexcept {
    // A hash of (x, y, frame) rather than a counter: neighbouring pixels must not walk correlated
    // sequences, or the spatial reuse combines samples that were already the same and the variance
    // does not go down. Wang's integer hash, mixed twice.
    u32 seed = (pixel_x * 0x9E3779B9U) ^ (pixel_y * 0x85EBCA6BU) ^ (frame * 0xC2B2AE35U);
    seed = (seed ^ 61U) ^ (seed >> 16U);
    seed *= 9U;
    seed = seed ^ (seed >> 4U);
    seed *= 0x27D4EB2DU;
    seed = seed ^ (seed >> 15U);
    state_ = seed | 1U;
}

f32 SampleStream::next_unit() noexcept {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 17U;
    state_ ^= state_ << 5U;
    // 24 bits, which is every value an f32 mantissa can hold in [0, 1) without a gap.
    return static_cast<f32>(state_ >> 8U) * (1.0F / 16777216.0F);
}

bool reservoir_update(Reservoir& reservoir, const LightCandidate& candidate, f32 source_pdf,
                      SampleStream& stream) noexcept {
    ++reservoir.sample_count;
    if (!(candidate.unshadowed > 0.0F) || !(source_pdf > 0.0F)) {
        return false;
    }
    // The RIS weight: the target function over the distribution the candidate actually came from.
    const f32 weight = candidate.unshadowed / source_pdf;
    reservoir.weight_sum += weight;
    if (stream.next_unit() * reservoir.weight_sum <= weight) {
        reservoir.light = candidate.index;
        reservoir.target = candidate.unshadowed;
        return true;
    }
    return false;
}

void reservoir_combine(Reservoir& reservoir, const Reservoir& other, f32 other_target_at_here,
                       u32 max_sample_count, SampleStream& stream) noexcept {
    if (!other.valid() || other.sample_count == 0) {
        return;
    }
    // The neighbour's contribution, re-weighted by ITS selected light's target function evaluated
    // HERE. Using `other.target` instead would bias the estimate towards whatever the neighbour was
    // looking at, and the symptom is a soft halo of the wrong colour around every geometric edge.
    const f32 weight =
        other.contribution_weight() * other_target_at_here * static_cast<f32>(other.sample_count);
    if (weight > 0.0F) {
        reservoir.weight_sum += weight;
        if (stream.next_unit() * reservoir.weight_sum <= weight) {
            reservoir.light = other.light;
            reservoir.target = other_target_at_here;
        }
    }
    // The neighbour's SAMPLE COUNT counts whether or not its light contributes here — it really did
    // consider those candidates, and dropping the count would inflate this pixel's estimate.
    reservoir.sample_count += other.sample_count;
    cap_reservoir(reservoir, max_sample_count);
}

void cap_reservoir(Reservoir& reservoir, u32 max_sample_count) noexcept {
    if (max_sample_count == 0 || reservoir.sample_count <= max_sample_count) {
        return;
    }
    // THE WEIGHT SUM IS SCALED WITH THE COUNT. The contribution weight is their ratio, so capping
    // the count alone made it grow by the excess on every combination: a temporal reservoir
    // combined with its own history every frame brightened without bound — the stochastic path's
    // frame went to infinity in a few dozen frames on the device. Scaling both keeps the estimate
    // and bounds how much the history speaks for, which is what the cap is for.
    const f32 scale = static_cast<f32>(max_sample_count) / static_cast<f32>(reservoir.sample_count);
    reservoir.weight_sum *= scale;
    reservoir.sample_count = max_sample_count;
}

Status validate_many_light(const ManyLightSettings& settings) noexcept {
    if (!settings.enabled) {
        return {};
    }
    if (!settings.denoising_available) {
        // Refused rather than downgraded. A silent fallback to the clustered path is exactly the
        // "discovered visually" the requirement rules out — the frame would look right and the
        // thousands of lights the scene was authored around would simply not be there.
        return fail(ErrorCode::InvalidArgument,
                    "stochastic many-light direct lighting requires denoising: its output is a "
                    "noisy estimate and there is no configuration in which it is correct without "
                    "one");
    }
    if (settings.candidates_per_pixel == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "stochastic many-light direct lighting needs at least one candidate per pixel");
    }
    if (settings.visibility_rays_per_pixel == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "stochastic many-light direct lighting needs at least one visibility ray per "
                    "pixel: a selected sample whose visibility is never resolved is an unshadowed "
                    "light, which is what this path exists to avoid");
    }
    if (settings.max_history_samples == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a temporal reservoir with no sample cap stops responding to the world "
                    "changing");
    }
    return {};
}

LightingPath active_lighting_path(const ManyLightSettings& settings) noexcept {
    if (!settings.enabled || !validate_many_light(settings)) {
        return LightingPath::Clustered;
    }
    return LightingPath::StochasticManyLight;
}

LightingPathStats lighting_path_stats(const ManyLightSettings& settings, u32 max_lights_per_cluster,
                                      u32 lights_dropped_by_bound) noexcept {
    LightingPathStats stats;
    stats.path = active_lighting_path(settings);
    // The two halves are mutually exclusive by construction: "a light limit that does not apply is
    // more confusing than one that does", so under the stochastic path the per-cluster numbers are
    // not merely ignored, they are absent.
    if (stats.path == LightingPath::StochasticManyLight) {
        stats.candidates_per_pixel = settings.candidates_per_pixel;
        stats.visibility_rays_per_pixel = settings.visibility_rays_per_pixel;
        return stats;
    }
    stats.max_lights_per_cluster = max_lights_per_cluster;
    stats.lights_dropped_by_bound = lights_dropped_by_bound;
    return stats;
}

f32 shading_allocation_ms(const BudgetArbiter& arbiter) noexcept {
    return arbiter.allocation_ms(BudgetSubsystem::MaterialEvaluation);
}

LightingPathDecision LightingPathSelector::update(const ManyLightSettings& settings,
                                                  const LightingPathInputs& inputs) noexcept {
    LightingPathDecision decision;
    const LightingPath before = path_;
    if (active_lighting_path(settings) != LightingPath::StochasticManyLight) {
        path_ = LightingPath::Clustered;
        fitting_frames_ = 0;
        decision.reason = "the stochastic path is not available under this profile";
    } else if (path_ == LightingPath::Clustered) {
        const bool overflowing = inputs.lights_dropped_by_bound > 0;
        const bool over_budget = inputs.allocation_ms > 0.0F && inputs.clustered_ms >= 0.0F &&
                                 inputs.clustered_ms > inputs.allocation_ms;
        if (overflowing || over_budget) {
            path_ = LightingPath::StochasticManyLight;
            left_at_lights_ = inputs.local_lights;
            left_at_ms_ = inputs.clustered_ms;
            fitting_frames_ = 0;
            decision.reason = overflowing
                                  ? "the clustered path dropped lights at its per-cluster bound"
                                  : "the clustered lighting costs more than the arbiter grants";
        } else {
            decision.reason = "the clustered path fits its bound and its allocation";
        }
    } else {
        // THE CLUSTERED PATH'S COST, PREDICTED: the light loop's cost scales with the lights it
        // walks, so the cost it was left at, scaled by the lights there are now.
        const f32 scale = left_at_lights_ > 0 ? static_cast<f32>(inputs.local_lights) /
                                                    static_cast<f32>(left_at_lights_)
                                              : 0.0F;
        const f32 predicted_ms = left_at_ms_ * scale;
        const bool fewer = static_cast<f32>(inputs.local_lights) <=
                           return_margin_ * static_cast<f32>(left_at_lights_);
        const bool affordable = inputs.allocation_ms <= 0.0F || left_at_ms_ < 0.0F ||
                                predicted_ms <= return_margin_ * inputs.allocation_ms;
        fitting_frames_ = fewer && affordable ? fitting_frames_ + 1U : 0U;
        if (fitting_frames_ >= dwell_frames_) {
            path_ = LightingPath::Clustered;
            fitting_frames_ = 0;
            decision.reason =
                "the scene fits the clustered path again, with margin, for long enough";
        } else {
            decision.reason = "the scene has outgrown the clustered path";
        }
    }
    decision.path = path_;
    decision.changed = path_ != before;
    return decision;
}

Reservoir sample_lights(Span<const LightCandidate> candidates, const ManyLightSettings& settings,
                        SampleStream& stream) noexcept {
    Reservoir reservoir;
    const auto count = static_cast<u32>(candidates.size());
    if (count == 0) {
        return reservoir;
    }
    const f32 source_pdf = 1.0F / static_cast<f32>(count);
    const u32 draws = math::max(settings.candidates_per_pixel, 1U);
    for (u32 draw = 0; draw < draws; ++draw) {
        const auto index = static_cast<u32>(stream.next_unit() * static_cast<f32>(count));
        (void)reservoir_update(reservoir, candidates[math::min(index, count - 1U)], source_pdf,
                               stream);
    }
    return reservoir;
}

}  // namespace cy::rendering
