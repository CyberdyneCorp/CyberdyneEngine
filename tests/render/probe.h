#pragma once
// The convention probe: a device, a pipeline, and one frame through the render graph. Section 5.
//
// ================================================================================================
// WHAT THIS IS FOR, AND WHY IT IS NOT A GOLDEN IMAGE
// ================================================================================================
//
// design.md §3: "Every convention gets a GPU-side assertion, not only a CPU-side one … A golden
// image is not sufficient evidence — a scene can look right with an inverted comparison until
// something intersects."
//
// So this fixture renders triangles and hands back BOTH buffers as numbers: the colour target as
// `u32` texels and the DEPTH target as `f32` depths, copied off the device. The cases then assert
// on values — near maps to 1, far to 0, this pixel is red and that one is not — rather than on a
// perceptual difference against a reference someone regenerated.
//
// Golden images are task 6.3's, and they are the right tool for "does the frame look right". They
// are the wrong tool for "is the depth comparison the right way round", which is what these cases
// ask.
//
// ================================================================================================
// EVERY CASE BUILDS ITS OWN DEVICE
// ================================================================================================
//
// Synchronisation validation keeps per-queue state for the process's lifetime, and recycled handles
// across two devices in one process produce phantom cross-test hazards that look damning and are
// not. (M3's spike, gotcha 6e.) The same reason `smoke.vulkan_frame` does it.
//
// ================================================================================================
// IT SKIPS RATHER THAN FAILS WITH NO DEVICE
// ================================================================================================
//
// Most continuous integration machines have no GPU, and a suite that failed there is a suite
// somebody disables. The skip is loud and names the backend that was selected instead, so "the
// convention suite passed" and "the convention suite found no GPU" are never confusable.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>

// Unconditional: this suite is DECLARED only when CY_RENDERER_VULKAN is on (tests/render/
// CMakeLists.txt), so a guard here would be a guard on a condition that is already true — and one
// that silently turned the whole suite into a set of skips when the feature macro did not reach the
// test target, which is exactly what it did the first time it was written that way.
#include <cy/backends/rhi/vulkan/vulkan_backend.h>

#include "shaders/conventions_spirv.h"

#include <cstdio>

namespace cy::render_test {

/// The probe's push-constant block, matching `ConventionPush` in conventions.slang. Four explicit
/// ROWS rather than a `float4x4`: see that file for why a matrix layout is the one ambiguity worth
/// removing from a test about transposition.
struct ProbePush {
    f32 row0[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    f32 row1[4] = {0.0F, 1.0F, 0.0F, 0.0F};
    f32 row2[4] = {0.0F, 0.0F, 1.0F, 0.0F};
    f32 row3[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
};

static_assert(sizeof(ProbePush) == 80, "the push block must fit the 128-byte portability limit");

/// Write a `cy::Mat4` into the block's four rows.
///
/// `Mat4` is COLUMN-major (`columns[4]`), so row `i` is the `i`th component of each column. Writing
/// it out here — rather than memcpy'ing 64 bytes and hoping — is what makes the transposition
/// explicit at the one place it happens.
inline void set_matrix(ProbePush& push, const Mat4& matrix) noexcept {
    f32* rows[4] = {push.row0, push.row1, push.row2, push.row3};
    for (u32 row = 0; row < 4; ++row) {
        rows[row][0] = matrix.columns[0][row];
        rows[row][1] = matrix.columns[1][row];
        rows[row][2] = matrix.columns[2][row];
        rows[row][3] = matrix.columns[3][row];
    }
}

/// One triangle, in camera-relative space, and the colour it is drawn in.
struct ProbeTriangle {
    Vec3 vertices[3];
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
};

inline constexpr u32 kProbeExtent = 64;
inline constexpr u32 kProbeTexels = kProbeExtent * kProbeExtent;

inline void print_validation(rhi::ValidationSeverity severity, const char* message,
                             void* /*user*/) noexcept {
    const char* label = "info";
    if (severity == rhi::ValidationSeverity::Error) {
        label = "error";
    } else if (severity == rhi::ValidationSeverity::Warning) {
        label = "warning";
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n", label, message != nullptr ? message : "");
}

/// What one probe render produced, read back off the device.
struct ProbeImage {
    explicit ProbeImage(Allocator& allocator) noexcept : color(allocator), depth(allocator) {}

    ProbeImage(const ProbeImage&) = delete;
    ProbeImage& operator=(const ProbeImage&) = delete;

    /// Rgba8Unorm texels, row-major from the TOP-LEFT — which is the layout a Vulkan image copy
    /// produces, and the reason the axis case can say "the upper half" and mean it.
    Array<u32> color;
    /// D32Sfloat depths, the same layout. This is the buffer the reversed-Z case reads: the values
    /// the DEVICE stored, not the ones a shader computed.
    Array<f32> depth;

    [[nodiscard]] u32 color_at(u32 x, u32 y) const noexcept {
        return color[(static_cast<usize>(y) * kProbeExtent) + x];
    }
    [[nodiscard]] f32 depth_at(u32 x, u32 y) const noexcept {
        return depth[(static_cast<usize>(y) * kProbeExtent) + x];
    }
    /// The red channel of a texel, 0..255. The probe draws in flat colours, so one channel answers
    /// "was anything drawn here".
    [[nodiscard]] u32 red_at(u32 x, u32 y) const noexcept { return color_at(x, y) & 0xFFU; }
};

/// A Vulkan device and the one pipeline the probe needs.
class ProbeFixture {
public:
    ProbeFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();

        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_conventions";
        description.enable_validation = true;
        // Without this the layers are loaded and every hazard check is silent.
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&print_validation, nullptr);
        }
    }

    ~ProbeFixture() {
        if (!device_.has_value()) {
            return;
        }
        rhi::Device& device = *device_.value();
        (void)device.wait_idle();
        if (!pipeline_.is_null()) {
            device.destroy_graphics_pipeline(pipeline_);
        }
        if (!layout_.is_null()) {
            device.destroy_pipeline_layout(layout_);
        }
        if (!vertex_shader_.is_null()) {
            device.destroy_shader_module(vertex_shader_);
        }
        if (!fragment_shader_.is_null()) {
            device.destroy_shader_module(fragment_shader_);
        }
        if (!vertices_.is_null()) {
            device.destroy_buffer(vertices_);
        }
        if (!color_readback_.is_null()) {
            device.destroy_buffer(color_readback_);
        }
        if (!depth_readback_.is_null()) {
            device.destroy_buffer(depth_readback_);
        }
        rhi::destroy_device(allocator_, device_.value());
    }

    ProbeFixture(const ProbeFixture&) = delete;
    ProbeFixture& operator=(const ProbeFixture&) = delete;

    [[nodiscard]] bool have_vulkan() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return allocator_; }
    [[nodiscard]] const rhi::BackendSelection& selection() const noexcept { return selection_; }

    /// Report the skip. Printed rather than passed through doctest's stringifier, which renders a
    /// `const char*` as its address rather than its text — which is how a skip message ends up
    /// saying "the backend selected was '0x5eef565be4ad'".
    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no Vulkan device on this machine; the backend selected was '%s' "
                     "because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

    /// Create the pipeline and the buffers. Separate from the constructor so a case that skips does
    /// no work at all.
    [[nodiscard]] Status prepare() noexcept;

    /// Render `triangles` through the render graph and read both targets back.
    ///
    /// `depth_write` off is how the depth-test cases draw a second triangle without disturbing the
    /// depth the first one wrote.
    [[nodiscard]] Status render(Span<const ProbeTriangle> triangles, const Mat4& relative_to_clip,
                                ProbeImage& out) noexcept;

    /// What the record callbacks are handed. Public because `RecordFn` is a plain function pointer,
    /// so the callbacks are free functions rather than members and cannot see a private nested
    /// type.
    struct PassState;

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
    rhi::ShaderModuleHandle vertex_shader_;
    rhi::ShaderModuleHandle fragment_shader_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::BufferHandle vertices_;
    rhi::BufferHandle color_readback_;
    rhi::BufferHandle depth_readback_;
};

}  // namespace cy::render_test
