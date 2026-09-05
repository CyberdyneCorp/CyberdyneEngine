#pragma once
// A Vulkan device for a render suite: created, validated, counted and torn down. Shared by
// `render.golden` and `render.frames`, which both need exactly this and nothing more.
//
// ================================================================================================
// EVERY CASE BUILDS ITS OWN DEVICE
// ================================================================================================
//
// Synchronisation validation keeps per-queue state for the process's lifetime, and recycled handles
// across two devices in one process produce phantom cross-test hazards that look damning and are
// not. (M3's spike, gotcha 6e.) probe.h says the same thing for the conventions suite, and
// `smoke.vulkan_frame` does it for the same reason.
//
// ================================================================================================
// IT COUNTS ERRORS RATHER THAN ONLY PRINTING THEM
// ================================================================================================
//
// `rhi-and-render-graph`: a frame that renders but trips validation is not a frame that works. The
// callback increments a counter the case asserts on, so a run cannot be green while the log it
// nobody read is full of errors — which is precisely how M3's per-frame descriptor defect reached
// the artefact while the sample printed "exit 0 (clean)".
//
// The counter is CUMULATIVE and never reset. A case that renders many frames reads it after each
// one and can therefore name the frame at which the count first moved, which is the diagnosis a
// per-frame defect needs and a total at the end does not give.
//
// ================================================================================================
// IT SKIPS RATHER THAN FAILS WITH NO DEVICE
// ================================================================================================
//
// Most continuous integration machines have no GPU, and a suite that failed there is a suite
// somebody disables. The skip is loud and names the backend that was selected instead, so "the
// suite passed" and "the suite found no GPU" are never confusable.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/system_allocator.h>

// Unconditional: both suites that include this header are DECLARED only when CY_RENDERER_VULKAN is
// on (tests/render/CMakeLists.txt), so a guard here would guard a condition that is already true.
#include <cy/backends/rhi/vulkan/vulkan_backend.h>

#include <cstdio>

namespace cy::render_test {

/// Counts errors into `user` and prints everything. Installed by `DeviceFixture`.
inline void count_validation(rhi::ValidationSeverity severity, const char* message,
                             void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// A device of the named backend, with validation and synchronisation validation on, and a count of
/// the errors it reported.
class DeviceFixture {
public:
    DeviceFixture(const char* backend, const char* application) noexcept
        : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();

        rhi::DeviceDescription description;
        description.application_name = application;
        description.enable_validation = true;
        // Off by default even when the layers are on, and without it none of the synchronisation
        // hazards are reported at all. (M3 spike, gotcha 6h.)
        description.enable_synchronisation_validation = true;
        description.request_async_compute = false;
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

    [[nodiscard]] bool is(rhi::BackendKind kind) const noexcept {
        return device_.has_value() && device_.value()->capabilities().backend() == kind;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return allocator_; }
    /// Errors reported since the device was created. Cumulative; never reset.
    [[nodiscard]] u32 validation_errors() const noexcept { return validation_errors_; }

    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no Vulkan device on this machine; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 validation_errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

}  // namespace cy::render_test
