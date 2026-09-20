// The same PCG compute workload through Metal and Vulkan. Each build selects one backend, while
// the shader, CPU reference, comparisons, and digest remain identical.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#if defined(CY_PCG_TEST_METAL)
#    include <cy/backends/rhi-metal/backend.h>
#else
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/pcg/gpu_executor.h>

#include <array>
#include <cstdio>

namespace {

#if defined(CY_PCG_TEST_METAL)
constexpr const char* kBackend = cy::rhi::metal::kMetalBackendName;
#else
constexpr const char* kBackend = "vulkan";
#endif

class Fixture {
public:
    Fixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
#if defined(CY_PCG_TEST_METAL)
        (void)cy::rhi::metal::register_metal_backend();
#else
        (void)cy::rhi::vulkan::register_vulkan_backend();
#endif
        cy::rhi::DeviceDescription description;
        description.application_name = "integration.pcg_gpu";
        description.enable_validation = true;
        device_ = cy::rhi::create_device(allocator_, kBackend, description, selection_);
    }

    ~Fixture() {
        if (device_) {
            (void)(*device_)->wait_idle();
            cy::rhi::destroy_device(allocator_, *device_);
        }
    }

    [[nodiscard]] cy::rhi::Device& device() const noexcept { return **device_; }
    [[nodiscard]] bool available() const noexcept {
        return device_.has_value() && !selection_.fell_back && selection_.selected != nullptr;
    }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

}  // namespace

CY_TEST_CASE("GPU PCG candidates agree with the CPU reference and react to the seed") {
    Fixture fixture;
    CY_TEST_MESSAGE("requested GPU backend must be available; absence is not agreement");
    CY_REQUIRE(fixture.available());

    constexpr cy::pcg::GpuCandidateParameters parameters{
        .seed = 0xC7B1D53AU, .count = 4096, .density_threshold = 24576};
    std::array<cy::pcg::GpuCandidate, parameters.count> output{};
    const auto report = cy::pcg::gpu::run_candidate_agreement(fixture.device(), parameters, output);
    CY_REQUIRE(report.has_value());
    CY_CHECK(report->all_records_equal);
    CY_CHECK_EQ(report->first_mismatch, ~0U);
    CY_CHECK_EQ(report->cpu.digest, report->device.digest);
    CY_CHECK_GT(report->device.accepted, 0U);
    CY_CHECK_GT(report->device.rejected, 0U);

    cy::pcg::GpuCandidateParameters changed = parameters;
    ++changed.seed;
    const auto changed_report =
        cy::pcg::gpu::run_candidate_agreement(fixture.device(), changed, output);
    CY_REQUIRE(changed_report.has_value());
    CY_CHECK(changed_report->all_records_equal);
    CY_CHECK_NE(report->device.digest, changed_report->device.digest);

    char evidence[320] = {};
    (void)std::snprintf(evidence, sizeof(evidence),
                        "gpu-pcg backend=%s device=%s candidates=%u accepted=%u rejected=%u "
                        "digest=0x%016llx",
                        kBackend, fixture.device().capabilities().device_name(), parameters.count,
                        report->device.accepted, report->device.rejected,
                        static_cast<unsigned long long>(report->device.digest));
    CY_TEST_MESSAGE(evidence);
}
