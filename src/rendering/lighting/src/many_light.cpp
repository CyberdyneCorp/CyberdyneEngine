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
    u32 seed = pixel_x * 0x9E3779B9U ^ pixel_y * 0x85EBCA6BU ^ frame * 0xC2B2AE35U;
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
    const f32 weight = other.contribution_weight() * other_target_at_here *
                       static_cast<f32>(other.sample_count);
    if (!(weight > 0.0F)) {
        // The neighbour's light contributes nothing here — a different surface, or the light is on
        // the wrong side. Its SAMPLE COUNT still counts: it really did consider those candidates,
        // and dropping the count would inflate this pixel's estimate.
        reservoir.sample_count =
            math::min(reservoir.sample_count + other.sample_count, max_sample_count);
        return;
    }
    reservoir.weight_sum += weight;
    if (stream.next_unit() * reservoir.weight_sum <= weight) {
        reservoir.light = other.light;
        reservoir.target = other_target_at_here;
    }
    reservoir.sample_count =
        math::min(reservoir.sample_count + other.sample_count, max_sample_count);
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

LightingPathStats lighting_path_stats(const ManyLightSettings& settings,
                                      u32 max_lights_per_cluster,
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
