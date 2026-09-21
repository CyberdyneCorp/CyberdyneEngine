// SPDX-License-Identifier: MIT
#pragma once
// The canonical CPU half of the GPU PCG conformance workload. The GPU adapter is a separate
// target: cy::pcg retains its deliberate no-RHI boundary.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::pcg {

/// Input to the deterministic GPU candidate workload. `seed` and `count` bound the run; the shader
/// keeps a candidate when its mixed value clears `density_threshold`. `reserved` pads to 16 bytes
/// so the record's layout matches the shader's constant-buffer view without implementation-defined
/// alignment.
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

/// One-record digest of a candidate run. `digest` folds every accepted record's coordinates, so a
/// mismatch between the CPU reference and the GPU readback shows up as a single-word compare
/// before per-slot inspection.
struct GpuCandidateSummary {
    u64 digest = 0;
    u32 accepted = 0;
    u32 rejected = 0;
};

/// The canonical hash the CPU reference and the shader both compute over a raw slot value. Kept in
/// the header so the two implementations cannot silently drift.
[[nodiscard]] constexpr u32 gpu_candidate_mix(u32 value) noexcept {
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    value *= 0x846CA68BU;
    value ^= value >> 16U;
    return value;
}

/// The reference evaluation of a single candidate slot. The shader computes the same record for
/// the same `(seed, slot, density_threshold)`; this function is what the CPU comparison uses.
[[nodiscard]] GpuCandidate gpu_candidate_at(u32 seed, u32 slot, u32 density_threshold) noexcept;

/// Fill `output` with the deterministic candidate sequence the shader is expected to produce. Used
/// by tests and cooks that need the CPU-side reference in bulk.
void generate_gpu_candidates(const GpuCandidateParameters& parameters,
                             Span<GpuCandidate> output) noexcept;

/// Fold an already-materialised candidate array into a summary. Callers hand in either the CPU
/// reference or the GPU readback and compare the two summaries.
[[nodiscard]] GpuCandidateSummary summarize_gpu_candidates(
    const GpuCandidateParameters& parameters, Span<const GpuCandidate> candidates) noexcept;

/// Produce the CPU reference summary without materialising the intermediate array. The shorter
/// path used by the executor when only the digest is needed.
[[nodiscard]] GpuCandidateSummary summarize_gpu_candidate_reference(
    const GpuCandidateParameters& parameters) noexcept;

}  // namespace cy::pcg
