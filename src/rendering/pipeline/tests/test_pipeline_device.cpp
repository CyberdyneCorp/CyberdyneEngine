// THE FRAME, CAPTURED. M8.c tasks 1b.2 and 1b.4, and the source of task 5.5's before-and-after
// pair.
//
// ================================================================================================
// WHAT THIS SUITE PRODUCES THAT NO EARLIER ONE COULD
// ================================================================================================
//
// M8.b's artefact drew its picture: it projected the frame's own draw list through the frame's own
// matrices and rendered shapes with the CPU, because `FrameAssembly` hands each pass's record
// callback to its caller and no caller supplied one. This suite READS THE FRAME'S OUTPUT IMAGE OFF
// THE DEVICE. Every texel below came out of a fragment shader.
//
// The three pictures it writes are the milestone's, and they are written every run rather than only
// on failure, because task 5.5 wants the before-and-after pair and a picture that only exists when
// something is broken is not a pair:
//
//   pipeline-frame-with-callbacks.png     the frame this layer records
//   pipeline-frame-without-callbacks.png  THE SAME FRAME, assembled identically, with an empty
//                                         `FrameSinks` — which is what the engine did before M8.c
//   pipeline-frame-particles.png          the same frame again with the particle renderer attached
//                                         through `PassExtension`
//
// They land in the suite's working directory; `docs/design/images/` holds the committed copies.
//
// ================================================================================================
// EVERY ASSERTION HERE CAN FAIL
// ================================================================================================
//
// The comparison is between two runs of the SAME code path with one argument changed, so there is
// no tolerance to tune and nothing to regenerate. Delete the `attach` calls in
// `FrameRecorder::sinks()` and the second case's `differing` count goes to zero and the case goes
// red; that is the check, and it was run.

#include "frame_scene.h"

#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstdio>

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
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// A device per case, for the reason tests/render/device.h gives: synchronisation validation keeps
/// per-queue state for the process's lifetime, and recycled handles across two devices in one
/// process produce phantom cross-test hazards that look damning and are not.
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_pipeline_frame";
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
                     "no Vulkan device on this machine; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// Write one capture, and say where. A picture nobody can find is a picture nobody looks at.
void save(const char* name, Span<const u32> texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, texels, kWidth, kHeight).has_value()) {
        return;
    }
    if (render_test::write_png(name, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", name, kWidth, kHeight);
    }
}

/// How many texels are not the clear colour. The clear is a dark neutral rather than black, so
/// "something was shaded here" and "nothing was" are different numbers rather than the same one.
[[nodiscard]] u32 shaded_texels(Span<const u32> texels) noexcept {
    u32 shaded = 0;
    for (const u32 texel : texels) {
        const u32 red = texel & 0xFFU;
        const u32 green = (texel >> 8U) & 0xFFU;
        const u32 blue = (texel >> 16U) & 0xFFU;
        if (red > 24U || green > 24U || blue > 24U) {
            ++shaded;
        }
    }
    return shaded;
}

}  // namespace

CY_TEST_CASE("the frame is CAPTURED: the layer's callbacks put shaded texels on the device") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);

    rendering::assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    CY_CHECK(report.executed);
    CY_CHECK_EQ(scene.recorded().passes, 5U);
    CY_CHECK_EQ(scene.recorded().opaque_draws, report.draws);
    // A frame that renders but trips validation is not a frame that works.
    CY_CHECK_EQ(fixture.validation_errors(), 0U);

    const u32 shaded = shaded_texels(scene.pixels());
    std::fprintf(stderr, "with callbacks: %u of %u texels shaded, %u draws, %u passes recorded\n",
                 shaded, kWidth * kHeight, scene.recorded().opaque_draws, scene.recorded().passes);
    // Twelve cubes in front of the camera cover a real fraction of a 480x270 frame. The bound is
    // deliberately far from what the scene produces: it is a check that ANYTHING was drawn, and the
    // case that follows is what makes it a check that the RIGHT thing was.
    CY_CHECK_GT(shaded, 2000U);
    save("pipeline-frame-with-callbacks.png", scene.pixels());
}

CY_TEST_CASE("the identical frame WITHOUT the callbacks is blank, and that difference is 1b") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);

    // THE BEFORE. Same scene, same assembly, same graph, same device frame — an empty `FrameSinks`.
    rendering::assembly::AssemblyReport blank;
    CY_REQUIRE(scene.render(RecordMode::None, blank).has_value());
    CY_CHECK(blank.executed);
    CY_CHECK_GT(blank.draws, 0U);
    CY_CHECK_EQ(scene.recorded().passes, 0U);
    Array<u32> before(allocator());
    CY_REQUIRE(before.resize(scene.pixels().size()).has_value());
    for (usize index = 0; index < before.size(); ++index) {
        before[index] = scene.pixels()[index];
    }
    const u32 blank_shaded = shaded_texels(before.span());
    save("pipeline-frame-without-callbacks.png", before.span());

    // THE AFTER.
    rendering::assembly::AssemblyReport recorded;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, recorded).has_value());
    const u32 shaded = shaded_texels(scene.pixels());
    const u32 differing = scene.differing_texels(before.span());
    std::fprintf(stderr, "before: %u shaded; after: %u shaded; %u of %u texels differ\n",
                 blank_shaded, shaded, differing, kWidth * kHeight);

    // The assembly decided the same frame both times — the layer changes what is RECORDED and
    // nothing else.
    CY_CHECK_EQ(recorded.draws, blank.draws);
    CY_CHECK_EQ(recorded.passes_declared, blank.passes_declared);
    // And the pictures are different. Remove the callbacks and this number is zero.
    CY_CHECK_GT(differing, 2000U);
    CY_CHECK_GT(shaded, blank_shaded);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the particle renderer draws through the layer, and the picture says so") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);

    rendering::assembly::AssemblyReport without;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, without).has_value());
    Array<u32> plain(allocator());
    CY_REQUIRE(plain.resize(scene.pixels().size()).has_value());
    for (usize index = 0; index < plain.size(); ++index) {
        plain[index] = scene.pixels()[index];
    }

    rendering::assembly::AssemblyReport with;
    CY_REQUIRE(scene.render(RecordMode::CallbacksAndParticles, with).has_value());
    const u32 differing = scene.differing_texels(plain.span());
    std::fprintf(stderr, "%u particles in one draw changed %u of %u texels\n",
                 scene.particle_report().particles, differing, kWidth * kHeight);

    CY_CHECK_EQ(scene.recorded().extensions_run, 1U);
    CY_CHECK_EQ(scene.particle_report().draws, 1U);
    CY_CHECK_EQ(scene.particle_report().particles, kParticleCount);
    // The effect is composited into the frame's own colour target through a pipeline whose layout
    // is compatible with the frame's. If the set layouts had diverged, the sets the recorder bound
    // would have been invalidated and this number would be zero — with validation errors beside it.
    CY_CHECK_GT(differing, 500U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
    save("pipeline-frame-particles.png", scene.pixels());
}

CY_TEST_CASE("many frames on the device, and a teardown while it is still busy") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    auto* scene = new FrameScene(allocator());
    CY_REQUIRE(scene->build(fixture.device()).has_value());

    // A one-frame suite cannot see a defect that begins at frame 3 — M4's `render.frames` suite's
    // own sentence, and the reason the ring is read after EVERY frame rather than at the end.
    const u32 turns = (fixture.device().frames_in_flight() * 8U) + 1U;
    for (u32 frame = 0; frame < turns; ++frame) {
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene->render(RecordMode::CallbacksAndParticles, report).has_value());
        if (fixture.validation_errors() != 0U) {
            std::fprintf(stderr, "validation first spoke at frame %u\n", frame);
        }
        CY_REQUIRE_EQ(fixture.validation_errors(), 0U);
    }
    delete scene;
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
