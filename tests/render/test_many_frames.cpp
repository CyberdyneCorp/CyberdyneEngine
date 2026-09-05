// MANY FRAMES ON THE DEVICE. M3's carried-forward debt, task 1.3.
//
// ================================================================================================
// WHY THIS SUITE EXISTS
// ================================================================================================
//
// Every device suite M3 shipped rendered exactly ONE frame. That is how this defect reached the
// milestone's artefact:
//
//     `VulkanDevice::allocate_descriptor_set(layout, per_frame)` took the flag, recorded it, and
//     then allocated from the CURRENT FRAME'S pool whichever value it had — and a frame pool is
//     reset the moment its slot comes round. A set the caller asked to be persistent was therefore
//     recycled after `frames_in_flight` frames, and from that frame on every draw bound a
//     VkDescriptorSet the driver had already destroyed: 24 validation errors a frame from frame 3,
//     on a run that still printed "exit 0 (clean)".
//
// A one-frame suite cannot see a defect that begins at frame 3. Neither can a suite that renders
// many frames and looks at the total afterwards without ever asking WHICH frame broke.
//
// So the shape of this suite is: render past the ring several times over, ask after EVERY frame
// whether validation is still silent, and assert on properties that hold frame after frame rather
// than on one photograph.
//
// ================================================================================================
// HOW IT DIFFERS FROM THE MULTI-FRAME CASE IN `render.golden`
// ================================================================================================
//
// `render.golden`'s "the frame survives more frames than the device holds in flight" renders the
// SAME camera every time and compares the last frame against the committed reference. That is the
// regression test for the defect above and it stays there, with the image it needs.
//
// These cases move the camera every frame, which is what makes the per-frame uniform ring and the
// per-frame descriptor pools actually turn over, and they compare the run against ITSELF: the frame
// report must be the same after the first, and the image at phase 0 rendered at the END of a long
// run must be BIT-IDENTICAL to the one rendered at the start. No reference file is involved, so
// nothing here can be laundered by regenerating a golden image, and the claim is exact rather than
// within a tolerance.
//
// ================================================================================================
// "NO VALIDATION ERRORS" IS NOT ENOUGH ON ITS OWN, AND THAT IS WHY CASE 2 EXISTS
// ================================================================================================
//
// A descriptor that points at recycled memory can also be a frame that happens to look right, and a
// set that had quietly stopped naming the shadow map would trip no layer at all. Validation
// silence is one of the two claims here; frame-over-frame identity is the other.

#include <cy/test/test.h>

#include <cy/backends/rhi/device.h>
#include <cy/core/memory/array.h>

#include "device.h"
#include "renderer.h"
#include "scene.h"

#include <cstdio>

namespace {

using cy::render_test::DeviceFixture;
using cy::sample::first_light::FrameReport;
using cy::sample::first_light::Renderer;
using cy::sample::first_light::RendererOptions;
using cy::sample::first_light::Scene;
using cy::sample::first_light::SceneDescription;

/// The same viewport `render.golden` photographs. Small: this suite renders tens of frames and the
/// budget for a render case is five seconds.
constexpr cy::u32 kWidth = 192;
constexpr cy::u32 kHeight = 108;

/// How many times round the ring of frames in flight. Once would prove nothing that
/// `frames_in_flight + 1` does not; eight is enough that a pool which is corrupted rather than
/// merely reused has been re-corrupted several times, and at this viewport it still costs well
/// under the render budget.
constexpr cy::u32 kRings = 8;

/// "no frame did this", for the frame indices the cases report. Not 0, because 0 is a frame.
constexpr cy::u32 kNoFrame = 0xFFFFFFFFU;

/// The passes the sample declares. Five on the first frame and four afterwards: the texture upload
/// is an ABSENT PASS rather than a branch, which is the property `render.null_frame` asserts off a
/// device and which nothing asserted on one.
constexpr cy::u32 kFirstFramePasses = 5;
constexpr cy::u32 kSteadyFramePasses = 4;

/// A renderer over a device, prepared for the sample's scene. Every case needs exactly this.
struct Run {
    explicit Run(DeviceFixture& fixture) noexcept
        : scene(fixture.allocator()), renderer(fixture.allocator(), fixture.device()) {}

    [[nodiscard]] cy::Status prepare() noexcept {
        if (cy::Status built = scene.build(SceneDescription{}); !built) {
            return built;
        }
        RendererOptions options;
        options.width = kWidth;
        options.height = kHeight;
        return renderer.prepare(scene, options);
    }

    /// One frame at a phase of the orbit. Phase is a pure function of the frame index, so the whole
    /// run reproduces from its length alone.
    [[nodiscard]] cy::Expected<FrameReport, cy::Error> at(cy::f32 phase) noexcept {
        return renderer.render(scene, scene.camera_at(phase));
    }

    Scene scene;
    Renderer renderer;
};

/// Everything about a frame that the DECLARATIONS decide, and which therefore cannot legitimately
/// change from one steady-state frame to the next. `transient_bytes` and `plan_hash` are in here
/// because a placement that moved between frames would mean the aliaser saw a different lifetime
/// for the same declarations.
bool same_shape(const FrameReport& a, const FrameReport& b) noexcept {
    return a.submits == b.submits && a.passes_recorded == b.passes_recorded &&
           a.passes_culled == b.passes_culled && a.barriers == b.barriers &&
           a.barrier_batches == b.barrier_batches &&
           a.queue_ownership_transfers == b.queue_ownership_transfers &&
           a.transient_bytes == b.transient_bytes && a.plan_hash == b.plan_hash &&
           a.draws == b.draws && a.triangles == b.triangles;
}

void print_report(const char* label, const FrameReport& report) noexcept {
    std::fprintf(stderr,
                 "%s: submits=%u passes=%u culled=%u barriers=%u batches=%u transfers=%u "
                 "transient=%llu plan=%llx draws=%u triangles=%u\n",
                 label, report.submits, report.passes_recorded, report.passes_culled,
                 report.barriers, report.barrier_batches, report.queue_ownership_transfers,
                 static_cast<unsigned long long>(report.transient_bytes),
                 static_cast<unsigned long long>(report.plan_hash), report.draws, report.triangles);
}

/// Copy the colour target out of the renderer, because the next frame overwrites it in place.
[[nodiscard]] cy::Status capture(const Renderer& renderer, cy::Array<cy::u32>& out) noexcept {
    const cy::Span<const cy::u32> texels = renderer.color_texels();
    if (texels.size() != static_cast<cy::usize>(kWidth) * kHeight) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "the readback is not the viewport's size");
    }
    out.clear();
    return out.append(texels);
}

/// The index of the first texel that differs, or `kNoFrame` when the two images are identical.
/// EXACT: two renders of the same frame on the same device have no licence to differ in a bit.
cy::u32 first_difference(const cy::Array<cy::u32>& a, const cy::Array<cy::u32>& b) noexcept {
    if (a.size() != b.size()) {
        return 0;
    }
    for (cy::usize index = 0; index < a.size(); ++index) {
        if (a[index] != b[index]) {
            return static_cast<cy::u32>(index);
        }
    }
    return kNoFrame;
}

}  // namespace

// ==================================================================================================
// CASE 1. The one the debt asked for: many frames, and the question asked after each of them.
// ==================================================================================================
CY_TEST_CASE("render.frames: validation stays clean over many turns of the ring") {
    DeviceFixture fixture("vulkan", "cy_test_render_frames");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }

    const cy::u32 in_flight = fixture.device().frames_in_flight();
    CY_REQUIRE(in_flight > 0U);
    const cy::u32 frames = (in_flight * kRings) + 1U;

    Run run(fixture);
    CY_REQUIRE(run.prepare().has_value());

    // The frame at which validation first said anything, and the frame at which the derivation
    // first changed shape. Both are recorded rather than asserted inside the loop: a per-frame
    // assertion would report the LAST frame to fail as loudly as the first, and it is the first
    // that names the defect.
    cy::u32 first_validation_error = kNoFrame;
    cy::u32 first_shape_change = kNoFrame;
    FrameReport first{};
    FrameReport steady{};

    for (cy::u32 index = 0; index < frames; ++index) {
        // A different camera every frame, so the per-frame uniform ring and the per-frame
        // descriptor pools genuinely turn over rather than being rewritten with the same bytes.
        const cy::f32 phase = static_cast<cy::f32>(index) / static_cast<cy::f32>(frames);
        cy::Expected<FrameReport, cy::Error> frame = run.at(phase);
        CY_REQUIRE(frame.has_value());

        if (fixture.validation_errors() != 0U && first_validation_error == kNoFrame) {
            first_validation_error = index;
        }
        if (index == 0) {
            first = *frame;
        } else if (index == 1) {
            steady = *frame;
        } else if (!same_shape(steady, *frame) && first_shape_change == kNoFrame) {
            first_shape_change = index;
            print_report("frames: steady", steady);
            print_report("frames: changed", *frame);
        }
    }

    if (first_validation_error != kNoFrame) {
        std::fprintf(stderr,
                     "frames: validation first reported an error at frame %u of %u, with %u in "
                     "flight — a defect that begins after the ring turns, which is exactly what a "
                     "one-frame suite cannot see.\n",
                     first_validation_error, frames, in_flight);
    }
    CY_CHECK_EQ(first_validation_error, kNoFrame);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);

    // The derivation is the same frame after frame. A resource that was recreated, a pass that
    // started being culled or an alias placement that moved would all show up here before they
    // showed up as a picture.
    CY_CHECK_EQ(first_shape_change, kNoFrame);

    // The upload pass exists once and then does not: an ABSENT pass, not a branch. Asserted on the
    // device here; `render.null_frame` asserts the same thing with no GPU at all.
    CY_CHECK_EQ(first.passes_recorded, kFirstFramePasses);
    CY_CHECK_EQ(steady.passes_recorded, kSteadyFramePasses);
    CY_CHECK_EQ(first.passes_culled, 0U);
    CY_CHECK_EQ(steady.passes_culled, 0U);
    CY_CHECK_EQ(steady.submits, 1U);
    CY_CHECK_GT(steady.draws, 0U);
    // Fewer barriers once the upload is gone, which is the other half of "the pass is absent".
    CY_CHECK_LT(steady.barriers, first.barriers);
}

// ==================================================================================================
// CASE 2. The claim validation silence cannot make: the same input still produces the same picture
// after the ring has turned eight times.
// ==================================================================================================
CY_TEST_CASE("render.frames: the frame at the end of a long run is bit-identical to the first") {
    DeviceFixture fixture("vulkan", "cy_test_render_frames");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }

    const cy::u32 in_flight = fixture.device().frames_in_flight();
    CY_REQUIRE(in_flight > 0U);

    Run run(fixture);
    CY_REQUIRE(run.prepare().has_value());

    // Two frames at phase 0 before the run, not one: the first frame also uploads the texture, so
    // comparing against it would be comparing against a frame of a different shape. The baseline is
    // the first STEADY frame.
    CY_REQUIRE(run.at(0.0F).has_value());
    CY_REQUIRE(run.at(0.0F).has_value());
    cy::Array<cy::u32> before(fixture.allocator());
    CY_REQUIRE(capture(run.renderer, before).has_value());

    // The orbit. Every frame binds a different view, so nothing here is the frame under test.
    const cy::u32 frames = in_flight * kRings;
    for (cy::u32 index = 0; index < frames; ++index) {
        const cy::f32 phase = static_cast<cy::f32>(index + 1U) / static_cast<cy::f32>(frames);
        CY_REQUIRE(run.at(phase).has_value());
    }

    CY_REQUIRE(run.at(0.0F).has_value());
    cy::Array<cy::u32> after(fixture.allocator());
    CY_REQUIRE(capture(run.renderer, after).has_value());

    const cy::u32 difference = first_difference(before, after);
    if (difference != kNoFrame) {
        std::fprintf(stderr,
                     "frames: after %u frames the same camera produced a different image, first at "
                     "texel %u (%u, %u): %08x became %08x.\n",
                     frames, difference, difference % kWidth, difference / kWidth,
                     before[difference], after[difference]);
    }
    CY_CHECK_EQ(difference, kNoFrame);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// ==================================================================================================
// CASE 3. THE CONTROL. Without it, case 2 would pass just as happily on a renderer that ignored the
// camera entirely — and so would every "the image did not change" claim ever made about it.
// ==================================================================================================
CY_TEST_CASE("render.frames: the orbit the run flies through does change the image") {
    DeviceFixture fixture("vulkan", "cy_test_render_frames");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }

    Run run(fixture);
    CY_REQUIRE(run.prepare().has_value());

    CY_REQUIRE(run.at(0.0F).has_value());
    CY_REQUIRE(run.at(0.0F).has_value());
    cy::Array<cy::u32> at_zero(fixture.allocator());
    CY_REQUIRE(capture(run.renderer, at_zero).has_value());

    // A quarter turn: the pillar's shadow falls somewhere else entirely, so this is not a claim
    // about a few texels of resampling.
    CY_REQUIRE(run.at(0.25F).has_value());
    cy::Array<cy::u32> at_quarter(fixture.allocator());
    CY_REQUIRE(capture(run.renderer, at_quarter).has_value());

    CY_CHECK_NE(first_difference(at_zero, at_quarter), kNoFrame);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
