// SPDX-License-Identifier: MIT
// The frame WITHOUT the temporal upscaler is the frame from before it existed.
// `render.upscale_off`, Vulkan only.
//
// Temporal upscaling split the frame's one extent into a render extent and an output extent and
// moved every scene pass, the jitter, the shaders' extent and the cluster grid onto the first. With
// no upscaler the two are equal, and this suite is the proof that equal means IDENTICAL: the
// pipeline suites' scene, rendered three ways — still under temporal antialiasing, with the camera
// and a box moving, and with bloom — compared byte for byte against references this file wrote
// against main's tree before the change (`CY_RENDER_UPDATE_GOLDEN=1`, with the frame, assembly,
// recorder and temporal sources checked out from main).
//
// It is a target of its own, linking nothing of `cy::rendering-upscale`, precisely so that it can
// be built against the tree from before the change to write those references.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace cy;
using namespace cy::pipeline_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_upscale_off";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &errors_);
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
    [[nodiscard]] u32 validation_errors() const noexcept { return errors_; }
    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no requested graphics device on this machine; the backend selected was '%s' "
                     "because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

const char* reference_path(const char* name) noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/%s", CY_UPSCALE_TEST_DIR, name);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Compare a frame against a committed reference, or — under `CY_RENDER_UPDATE_GOLDEN` — write it.
void check_against_reference(const char* name, Span<const u32> pixels) {
    render_test::Image rendered(allocator());
    CY_REQUIRE(render_test::adopt(rendered, pixels, kWidth, kHeight).has_value());
    if (updating_references()) {
        CY_CHECK(render_test::write_png(reference_path(name), rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     reference_path(name));
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    const Status read = render_test::read_png(reference_path(name), reference);
    if (!read) {
        std::fprintf(stderr, "%s: %s\n", reference_path(name), read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr, "%s against the frame before the change: %u differing, worst delta %u\n",
                 name, comparison.differing, comparison.max_channel_delta);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);
}

/// Pinned, so every run draws the same jitter sequence.
void pin(rendering::assembly::AssemblyDescription& description, void* /*user*/) noexcept {
    description.pin_jitter = true;
    description.pinned_jitter_index = 0;
}

/// Box 1 in front of the row, where the moving case slides it.
void place_box(u32 which, Vec3& centre, f32& half, void* /*user*/) noexcept {
    if (which == 1U) {
        centre = Vec3{-1.6F, -1.275F, -5.0F};
        half = 0.5F;
    }
}

}  // namespace

CY_TEST_CASE("without the upscaler the frame is byte for byte the frame from before") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    {
        // STILL, under temporal antialiasing: the prepass, the shading, the resolve and the
        // post-process, six frames into a pinned jitter sequence.
        FrameScene scene(allocator());
        FrameSceneHooks hooks;
        hooks.configure = &pin;
        scene.set_hooks(hooks);
        CY_REQUIRE(scene.build(fixture.device()).has_value());
        scene.set_read_back(true);
        for (u32 frame = 0; frame < 6; ++frame) {
            rendering::assembly::AssemblyReport report;
            CY_REQUIRE(scene.render(RecordMode::CallbacksAndParticles, report).has_value());
        }
        check_against_reference("off_still_taa.png", scene.pixels());
    }
    {
        // MOVING: the camera walks and a box slides, so the camera and per-object motion, the
        // reprojection and the history all differ from frame to frame.
        FrameScene scene(allocator());
        FrameSceneHooks hooks;
        hooks.configure = &pin;
        hooks.place_box = &place_box;
        scene.set_hooks(hooks);
        CY_REQUIRE(scene.build(fixture.device()).has_value());
        scene.set_read_back(true);
        for (u32 frame = 0; frame < 6; ++frame) {
            scene.set_eye(Vec3{0.08F * static_cast<f32>(frame), 0.0F, 0.0F});
            CY_REQUIRE(
                scene.move_box(1, Vec3{-1.6F + (0.4F * static_cast<f32>(frame)), -1.275F, -5.0F})
                    .has_value());
            rendering::assembly::AssemblyReport report;
            CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        }
        check_against_reference("off_moving_taa.png", scene.pixels());
    }
    {
        // BLOOM: the chain at the frame's extent, between the resolve and the post-process.
        FrameScene scene(allocator());
        FrameSceneHooks hooks;
        hooks.configure = &pin;
        scene.set_hooks(hooks);
        rendering::BloomSettings bloom;
        CY_REQUIRE(scene.build(fixture.device(), &bloom).has_value());
        scene.set_read_back(true);
        for (u32 frame = 0; frame < 4; ++frame) {
            rendering::assembly::AssemblyReport report;
            CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        }
        check_against_reference("off_bloom.png", scene.pixels());
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
