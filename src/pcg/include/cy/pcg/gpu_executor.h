// SPDX-License-Identifier: MIT
#pragma once
// RHI adapter for the deterministic GPU PCG conformance workload. Kept out of cy::pcg so cooks
// and dedicated servers retain the core module's no-device link graph.

#include <cy/backends/rhi/device.h>
#include <cy/pcg/gpu_conformance.h>

namespace cy::pcg::gpu {

/// The outcome of one GPU-vs-CPU candidate agreement run. `cpu` and `device` are the two summaries
/// that must match; `first_mismatch` is the slot index of the first differing record (or `~0U` when
/// none), and `all_records_equal` is the stronger claim that every readback record matched.
struct AgreementReport {
    GpuCandidateSummary cpu;
    GpuCandidateSummary device;
    u32 first_mismatch = ~0U;
    bool all_records_equal = false;
};

/// Execute the canonical candidate workload and compare the readback with an independently
/// evaluated CPU reference. `output` receives the device records for diagnostics and publication.
[[nodiscard]] Expected<AgreementReport, Error> run_candidate_agreement(
    rhi::Device& device, const GpuCandidateParameters& parameters,
    Span<GpuCandidate> output) noexcept;

}  // namespace cy::pcg::gpu
