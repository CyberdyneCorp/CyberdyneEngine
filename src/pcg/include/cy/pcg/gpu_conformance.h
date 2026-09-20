#pragma once
// The canonical CPU half of the GPU PCG conformance workload. The GPU adapter is a separate
// target: cy::pcg retains its deliberate no-RHI boundary.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::pcg {

struct GpuCandidateParameters {
    u32 seed = 0;
    u32 count = 0;
    u32 density_threshold = 0;
    u32 reserved = 0;
};

/// One fixed-width record per candidate slot. Coordinates are unsigned 16.16 region-local values;
/// accepted is a word rather than a bool so the CPU and shader ABI has no implementation-defined
/// packing.
struct GpuCandidate {
    u32 slot = 0;
    u32 x = 0;
    u32 z = 0;
    u32 accepted = 0;

    friend constexpr bool operator==(GpuCandidate, GpuCandidate) noexcept = default;
};

struct GpuCandidateSummary {
    u64 digest = 0;
    u32 accepted = 0;
    u32 rejected = 0;
};

[[nodiscard]] constexpr u32 gpu_candidate_mix(u32 value) noexcept {
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    value *= 0x846CA68BU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] GpuCandidate gpu_candidate_at(u32 seed, u32 slot, u32 density_threshold) noexcept;
void generate_gpu_candidates(const GpuCandidateParameters& parameters,
                             Span<GpuCandidate> output) noexcept;
[[nodiscard]] GpuCandidateSummary summarize_gpu_candidates(
    const GpuCandidateParameters& parameters, Span<const GpuCandidate> candidates) noexcept;
[[nodiscard]] GpuCandidateSummary summarize_gpu_candidate_reference(
    const GpuCandidateParameters& parameters) noexcept;

}  // namespace cy::pcg
