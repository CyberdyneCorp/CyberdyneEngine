#pragma once
// The device, the rig and the mesh both skinning suites use. M8.d.
//
// ONE RIG, TWO SUITES. `render.skinning` compares the dispatch's output buffers against
// `cpu_reference_skin`; `render.skinned_draw` rasterises those same buffers and asks where the
// pixels landed. Neither has a rig of its own, for the reason tests/render/README.md gives about
// golden images: a second fixture drifts from the first inside a milestone.
//
// THE RIG IS THE ELBOW, and it is the same one src/servers/render/geometry/tests/
// test_skin_dispatch.cpp works out on paper. Bone 0 is the upper arm at rest, so its skinning
// matrix is the identity. Bone 1 is the forearm turned a quarter turn about +Z around an elbow at
// (1, 0, 0), so its skinning matrix is `T(1,0,0) · Rz(90°) · T(-1,0,0)` — which sends a point
// (x, y, 0) to (1 − y, x − 1, 0). A vertex at (3, 0, 0) therefore lands at (1, 2, 0), and that is
// the number both suites are written against.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/geometry/skin_dispatch.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace cy::skin_test {

/// A device of its own per case, for the reason tests/render/device.h gives: synchronisation
/// validation keeps per-queue state for the process's lifetime, and recycled handles across two
/// devices in one process produce phantom cross-test hazards that look damning and are not.
class DeviceFixture {
public:
    explicit DeviceFixture(const char* name) noexcept
        : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = name;
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        description.request_async_compute = false;
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

/// The elbow pose as two skinning matrices, in the `Mat4` form `PoseWorld` publishes.
///
/// Built from `Mat4` rather than written out as rows on purpose: `SkinPass::upload` transposes
/// through `pack_bone_matrix`, and a fixture that pre-transposed would make that transposition
/// untested at exactly the place it matters.
[[nodiscard]] inline std::vector<Mat4> elbow_pose(f32 turn_radians) noexcept {
    const f32 c = std::cos(turn_radians);
    const f32 s = std::sin(turn_radians);
    const Mat4 rotation =
        Mat4::from_columns(Vec4{c, s, 0.0F, 0.0F}, Vec4{-s, c, 0.0F, 0.0F},
                           Vec4{0.0F, 0.0F, 1.0F, 0.0F}, Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    const Vec3 elbow{1.0F, 0.0F, 0.0F};
    const Mat4 forearm =
        Mat4::from_translation(elbow) * rotation * Mat4::from_translation(Vec3{-elbow.x, 0, 0});
    return {Mat4::identity(), forearm};
}

/// Pack four lanes of influence.
[[nodiscard]] inline render::geometry::GpuSkinInfluence bound_to(u8 bone) noexcept {
    const u8 indices[4] = {bone, 0, 0, 0};
    const u8 weights[4] = {255, 0, 0, 0};
    return render::geometry::skin_influence(indices, weights);
}

}  // namespace cy::skin_test
