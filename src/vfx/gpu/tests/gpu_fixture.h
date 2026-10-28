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
#if defined(CY_TEST_VFX_METAL)
#    include <cy/backends/rhi-metal/backend.h>
#else
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/test/test.h>
#include <cy/vfx/gpu/gpu_pass.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

#include <cmath>
#include <cstdio>
#include <cstring>
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
#if defined(CY_TEST_VFX_METAL)
        (void)rhi::metal::register_metal_backend();
        constexpr const char* backend = "metal";
#else
        (void)rhi::vulkan::register_vulkan_backend();
        constexpr const char* backend = "vulkan";
#endif
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = name;
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        description.request_async_compute = async;
        device_ = rhi::create_device(allocator_, backend, description, selection_);
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

    /// The requested backend answered — the device may still be one these cases cannot draw on.
    [[nodiscard]] bool has_backend() const noexcept {
        return device_.has_value() && device_.value()->capabilities().backend() ==
#if defined(CY_TEST_VFX_METAL)
                                          rhi::BackendKind::Metal;
#else
                                          rhi::BackendKind::Vulkan;
#endif
    }
    [[nodiscard]] bool has_gpu() const noexcept { return has_backend() && !compatibility_path(); }
    /// A Metal device with no global texture table: argument buffers below tier 2, which is what
    /// the hosted macOS runner's paravirtual GPU has. The native path these cases draw through
    /// binds descriptor sets that device refuses, so they skip on it and say so.
    [[nodiscard]] bool compatibility_path() const noexcept {
        return has_backend() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Metal &&
               device_.value()->global_texture_table().is_null();
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] Allocator& gpu_allocator() const noexcept { return allocator_; }
    [[nodiscard]] u32 validation_errors() const noexcept { return validation_errors_; }

    void report_skip() const noexcept {
        if (compatibility_path()) {
            std::fprintf(stderr,
                         "no usable Metal device: '%s' is on the compatibility path, with no "
                         "global texture table\n",
                         device_.value()->capabilities().device_name());
            return;
        }
        std::fprintf(stderr,
                     "no requested GPU on this machine; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    static void count_validation(rhi::ValidationSeverity severity, const char* message,
                                 void* user) noexcept {
        if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
            ++*static_cast<u32*>(user);
        }
        std::fprintf(stderr, "GPU validation %s: %s\n",
                     severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                     message != nullptr ? message : "");
    }

    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 validation_errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// Read one attribute component out of the GPU block, at the precision the compiler chose.
///
/// THE GPU BLOCK IS NOT THE CPU BLOCK, so this walks `gpu_array_base_words` and
/// `gpu_words_per_particle` rather than the layout's byte offsets. A helper that reused
/// `load_component` would read `color` — one byte a particle on the host, one word here — from
/// three quarters of the wrong place.
[[nodiscard]] inline f32 read_gpu(Span<const u32> block, const AttributeLayout& layout,
                                  const char* attribute, u32 particle, u32 component,
                                  u32 capacity) noexcept {
    const AttributeSlot* slot = layout.find(Name::intern(attribute));
    if (slot == nullptr || slot->elided || component >= slot->components) {
        return 0.0F;
    }
    const u32 base = gpu_array_base_words(layout, *slot, capacity);
    const u32 words = gpu_words_per_particle(*slot);
    if (words == 0) {
        // Unreachable for a live slot — `gpu_words_per_particle` is at least one for any slot with
        // a component — and stated rather than assumed, because this helper's whole job is to index
        // a buffer and an unchecked divisor here would index it wrongly rather than loudly.
        return 0.0F;
    }
    const u32 components_per_word = ((slot->components + words) - 1U) / words;
    const u32 word_index = component / components_per_word;
    const u32 lane = component % components_per_word;
    const usize index =
        static_cast<usize>(base) + (static_cast<usize>(particle) * words) + word_index;
    if (index >= block.size()) {
        return 0.0F;
    }
    const u32 word = block[index];
    switch (slot->precision) {
        case Precision::Float32: {
            f32 value = 0.0F;
            std::memcpy(&value, &word, sizeof(f32));
            return value;
        }
        case Precision::Float16: {
            const u16 half = static_cast<u16>((word >> (lane * 16U)) & 0xFFFFU);
            // The same widening `load_component` performs; spelled here because this file may not
            // reach into the layout module's private helpers.
            const u32 sign = (half & 0x8000U) << 16U;
            const u32 exponent = (half >> 10U) & 0x1FU;
            const u32 mantissa = half & 0x3FFU;
            u32 bits = 0;
            if (exponent == 0) {
                bits = sign;
            } else if (exponent == 31U) {
                bits = sign | 0x7F800000U | (mantissa << 13U);
            } else {
                bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
            }
            f32 value = 0.0F;
            std::memcpy(&value, &bits, sizeof(f32));
            return value;
        }
        case Precision::Unorm8:
            return static_cast<f32>((word >> (lane * 8U)) & 0xFFU) * (1.0F / 255.0F);
        case Precision::Snorm16: {
            i32 raw = static_cast<i32>((word >> (lane * 16U)) & 0xFFFFU);
            raw = (raw & 0x8000) != 0 ? raw - 65536 : raw;
            const f32 value = static_cast<f32>(raw) * (1.0F / 32767.0F);
            return value < -1.0F ? -1.0F : value;
        }
        case Precision::Auto:
            break;
    }
    return 0.0F;
}

[[nodiscard]] inline f32 relative_difference(f32 a, f32 b) noexcept {
    const f32 magnitude = std::fmax(std::fabs(a), std::fabs(b));
    return magnitude > 1.0F ? std::fabs(a - b) / magnitude : std::fabs(a - b);
}

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
    if (pass.particles_buffer().is_null() || pass.alive_buffer().is_null() ||
        pass.indices_buffer().is_null() ||
        pass.particles_resource() == cy::rendering::kInvalidResource ||
        pass.alive_resource() == cy::rendering::kInvalidResource ||
        pass.indices_resource() == cy::rendering::kInvalidResource) {
        std::fprintf(stderr, "VFX render inputs were not published after declaration\n");
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
