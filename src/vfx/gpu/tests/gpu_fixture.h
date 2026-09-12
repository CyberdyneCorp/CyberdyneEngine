#pragma once
// The device and the cooked effect the VFX GPU suite runs. M10 task 5.1.
//
// ONE EFFECT, AND IT IS THE ONE EVERY OTHER VFX SUITE ALREADY USES. `src/vfx/tests/effects.h`
// authors the spark plume that `integration.vfx_compiler` asks what was compiled of,
// `integration.vfx` asks what was simulated of and `render.vfx` asks what was drawn of. This suite
// asks what the DEVICE computed of the same asset, and it uses the same builder for the reason
// `tests/render/README.md` gives about golden images: a second scene drifts from the first inside a
// milestone, and then two suites disagree about a defect neither of them can see.
//
// Every part of the plume earns its place in this suite too:
//
//   * `velocity` written by Initialise and read by Update is the FUSION, and the fused kernel is
//     the one the spawn pass runs — so the alive marker `2` and the compaction's promotion of it
//     are exercised rather than described.
//   * `scratch`, elided by liveness, is a slot the GPU block does not allocate either: the same
//     `AttributeLayout` drives `gpu_block_words`, so an elided attribute costs no device memory and
//     the read-back is shorter than the declaration would suggest.
//   * `color` at `Unorm8` is four components to a word on the device and one byte a particle on the
//     host. That is the whole of <cy/vfx/gpu_layout.h>'s warning, made concrete: the two blocks
//     have different sizes and the same values, which is why this suite compares BY VALUE.

#include "effects.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/test/test.h>
#include <cy/vfx/gpu/gpu_pass.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

#include <cstdio>
#include <vector>

namespace cy::vfx_gpu_test {

using namespace cy::vfx;

/// A device of its own per case, for the reason `tests/render/device.h` gives: synchronisation
/// validation keeps per-queue state for the process's lifetime, and recycled handles across two
/// devices in one process produce phantom cross-test hazards that look damning and are not.
///
/// `request_async_compute` is a CONSTRUCTOR ARGUMENT here and not a constant, because one case runs
/// the same simulation on both queues and requires the two to agree. A fixture that could not turn
/// the queue off would make `vfx-system`'s "async execution SHALL be disableable" a claim about a
/// field nobody set.
class DeviceFixture {
public:
    explicit DeviceFixture(const char* name, bool async = true) noexcept
        : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = name;
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        description.request_async_compute = async;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &validation_errors_);
        }
    }

    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] Allocator& gpu_allocator() const noexcept { return allocator_; }
    [[nodiscard]] u32 validation_errors() const noexcept { return validation_errors_; }

    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no Vulkan device on this machine; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    static void count_validation(rhi::ValidationSeverity severity, const char* message,
                                 void* user) noexcept {
        if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
            ++*static_cast<u32*>(user);
        }
        std::fprintf(stderr, "vulkan validation %s: %s\n",
                     severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                     message != nullptr ? message : "");
    }

    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 validation_errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// Execute one graph and drain it. One frame, ended rather than merely waited on, for the reason
/// `test_skin_pass.cpp` records: `begin_frame` recycles the oldest in-flight frame's pools, and a
/// device whose frames are all still open refuses to start another. This suite runs many frames.
[[nodiscard]] inline bool run_frame(DeviceFixture& gpu, Allocator& allocator, gpu::VfxGpuPass& pass,
                                    bool reset) noexcept {
    if (!gpu.device().begin_frame().has_value()) {
        return false;
    }
    cy::rendering::RenderGraph graph(allocator);
    const Status declared = reset ? pass.declare_reset(graph) : pass.declare(graph);
    if (!declared) {
        std::fprintf(stderr, "declare failed: %s\n", declared.error().message);
        return false;
    }
    cy::rendering::GraphExecutor executor(allocator, gpu.device());
    auto executed =
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
    if (!executed.has_value()) {
        std::fprintf(stderr, "execute failed: %s\n", executed.error().message);
        return false;
    }
    if (!gpu.device().wait_idle().has_value()) {
        return false;
    }
    return gpu.device().end_frame().has_value();
}

}  // namespace cy::vfx_gpu_test
