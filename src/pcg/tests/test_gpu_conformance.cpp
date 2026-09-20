#include <cy/test/test.h>

#include <cy/pcg/gpu_conformance.h>

#include <array>

CY_TEST_CASE("the GPU PCG reference is populated reproducible and seed-sensitive") {
    constexpr cy::pcg::GpuCandidateParameters parameters{
        .seed = 0xC7B1D53AU, .count = 4096, .density_threshold = 24576};
    std::array<cy::pcg::GpuCandidate, parameters.count> first{};
    std::array<cy::pcg::GpuCandidate, parameters.count> second{};
    cy::pcg::generate_gpu_candidates(parameters, first);
    cy::pcg::generate_gpu_candidates(parameters, second);

    const auto first_summary = cy::pcg::summarize_gpu_candidates(parameters, first);
    const auto second_summary = cy::pcg::summarize_gpu_candidates(parameters, second);
    const auto direct_summary = cy::pcg::summarize_gpu_candidate_reference(parameters);
    CY_CHECK_EQ(first_summary.digest, second_summary.digest);
    CY_CHECK_EQ(first_summary.digest, direct_summary.digest);
    CY_CHECK_EQ(first_summary.digest, 0x9CD2AF3B172F8888ULL);
    CY_CHECK_EQ(first_summary.accepted, second_summary.accepted);
    CY_CHECK_GT(first_summary.accepted, 0U);
    CY_CHECK_GT(first_summary.rejected, 0U);

    cy::pcg::GpuCandidateParameters changed = parameters;
    ++changed.seed;
    cy::pcg::generate_gpu_candidates(changed, second);
    const auto changed_summary = cy::pcg::summarize_gpu_candidates(changed, second);
    CY_CHECK_NE(first_summary.digest, changed_summary.digest);
    CY_CHECK_EQ(changed_summary.digest, 0xD89AF118B7B412DCULL);
}
