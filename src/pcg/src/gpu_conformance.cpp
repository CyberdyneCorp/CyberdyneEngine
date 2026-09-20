#include <cy/pcg/gpu_conformance.h>

#include <cy/pcg/dataset.h>

namespace cy::pcg {

GpuCandidate gpu_candidate_at(u32 seed, u32 slot, u32 density_threshold) noexcept {
    const u32 identity = gpu_candidate_mix(seed ^ (slot * 0x9E3779B9U));
    const u32 second = gpu_candidate_mix(identity ^ 0xA511E9B3U);
    const u32 density = gpu_candidate_mix(identity + 0x63D83595U);
    return GpuCandidate{
        .slot = slot,
        .x = identity & 0xFFFFU,
        .z = second & 0xFFFFU,
        .accepted = (density & 0xFFFFU) < density_threshold ? 1U : 0U,
    };
}

void generate_gpu_candidates(const GpuCandidateParameters& parameters,
                             Span<GpuCandidate> output) noexcept {
    const usize count = output.size() < parameters.count ? output.size() : parameters.count;
    for (usize index = 0; index < count; ++index) {
        output[index] = gpu_candidate_at(parameters.seed, static_cast<u32>(index),
                                         parameters.density_threshold);
    }
}

GpuCandidateSummary summarize_gpu_candidates(const GpuCandidateParameters& parameters,
                                             Span<const GpuCandidate> candidates) noexcept {
    Digest digest;
    digest.u32_value(parameters.seed);
    digest.u32_value(parameters.count);
    digest.u32_value(parameters.density_threshold);

    GpuCandidateSummary summary;
    const usize count = candidates.size() < parameters.count ? candidates.size() : parameters.count;
    for (usize index = 0; index < count; ++index) {
        const GpuCandidate& candidate = candidates[index];
        digest.u32_value(candidate.slot);
        digest.u32_value(candidate.x);
        digest.u32_value(candidate.z);
        digest.u32_value(candidate.accepted);
        summary.accepted += candidate.accepted != 0 ? 1U : 0U;
    }
    summary.rejected = static_cast<u32>(count) - summary.accepted;
    digest.u32_value(summary.accepted);
    summary.digest = digest.value();
    return summary;
}

GpuCandidateSummary summarize_gpu_candidate_reference(
    const GpuCandidateParameters& parameters) noexcept {
    Digest digest;
    digest.u32_value(parameters.seed);
    digest.u32_value(parameters.count);
    digest.u32_value(parameters.density_threshold);

    GpuCandidateSummary summary;
    for (u32 slot = 0; slot < parameters.count; ++slot) {
        const GpuCandidate candidate =
            gpu_candidate_at(parameters.seed, slot, parameters.density_threshold);
        digest.u32_value(candidate.slot);
        digest.u32_value(candidate.x);
        digest.u32_value(candidate.z);
        digest.u32_value(candidate.accepted);
        summary.accepted += candidate.accepted;
    }
    summary.rejected = parameters.count - summary.accepted;
    digest.u32_value(summary.accepted);
    summary.digest = digest.value();
    return summary;
}

}  // namespace cy::pcg
